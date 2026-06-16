// serious_python_run.c — unified embedded CPython runtime entry point.
//
// Single C function `serious_python_run` that the platform plugins
// (serious_python_{darwin,linux,windows,android}) call from Dart via FFI
// instead of each shipping its own Py_Initialize / PyRun_SimpleFile
// orchestration in Swift / C++ / Dart-FFI.
//
// Limited API (Py_LIMITED_API=0x030c0000) — uses only symbols in the abi3
// stable ABI for Python 3.12+. One binary per (platform × arch) works
// across every 3.12+ runtime. No PyConfig (not in Limited API); we use
// env vars (setenv) pre-Py_Initialize and PyRun_SimpleString post-init
// for sys.path / sys.argv adjustments.

#define PY_SSIZE_T_CLEAN
// Android uses the FULL CPython API (not abi3): it needs PyConfig to start the
// interpreter with site disabled so the serious_python native-module finder is
// installed before `site` runs. The Android binary is already per-CPython-version
// (DT_NEEDED libpython3.X.so), so giving up abi3 portability there costs nothing.
#if !defined(__ANDROID__)
#define Py_LIMITED_API 0x030c0000
#endif
#include <Python.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dart_api/dart_api_dl.h"

// PyRun_SimpleString and PyRun_SimpleFile aren't in the Limited API — the
// abi3 stub library (python3.lib on Windows) doesn't export them. We
// reimplement them using Py_CompileString + PyEval_EvalCode against
// __main__'s globals, which ARE in the Limited API (since 3.2). Behavior
// matches CPython's own PyRun_SimpleStringFlags / PyRun_SimpleFileExFlags
// (see Python/pythonrun.c).
static int sp_pyrun_string(const char *source, const char *filename) {
    PyObject *code = Py_CompileString(source, filename, Py_file_input);
    if (!code) {
        PyErr_Print();
        return -1;
    }

    PyObject *main_mod = PyImport_AddModule("__main__");  // borrowed
    if (!main_mod) {
        Py_DECREF(code);
        PyErr_Print();
        return -1;
    }
    PyObject *globals = PyModule_GetDict(main_mod);  // borrowed

    PyObject *result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!result) {
        PyErr_Print();
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

static int sp_pyrun_file(FILE *fp, const char *filename) {
    // Slurp file into memory then compile + eval.
    if (fseek(fp, 0, SEEK_END) != 0) return -1;
    long size = ftell(fp);
    if (size < 0) return -1;
    rewind(fp);

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) return -1;
    size_t n = fread(buf, 1, (size_t)size, fp);
    if (n != (size_t)size) {
        free(buf);
        return -1;
    }
    buf[size] = '\0';

    int rc = sp_pyrun_string(buf, filename);
    free(buf);
    return rc;
}

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#define EXPORT __declspec(dllexport)
#define SP_PATH_SEP "\\"
#define SP_PYPATH_SEP ";"
static int sp_setenv(const char* k, const char* v) { return _putenv_s(k, v); }
#else
#include <pthread.h>
#include <unistd.h>
#define EXPORT __attribute__((visibility("default")))
#define SP_PATH_SEP "/"
#define SP_PYPATH_SEP ":"
static int sp_setenv(const char* k, const char* v) { return setenv(k, v, 1); }
#endif

// PyInit_dart_bridge lives in dart_bridge.c, linked into the same binary.
extern PyObject* PyInit_dart_bridge(void);

// Release registered Dart→Python handler PyObjects before Py_Finalize. See
// dart_bridge.c.
extern void dart_bridge_clear_handlers(void);

// Install Python-level sys.stdout / sys.stderr wrappers that forward
// writes to the platform native log sink. See dart_bridge.c.
extern int dart_bridge_install_stdio_redirect(void);

// Fire registered Python session-restart handlers with the new port map.
// On Android process reuse (Dart VM restart while libdart_bridge stays
// loaded), this is how the running Python program learns about the new
// Dart native port numbers. See dart_bridge.c.
extern void dart_bridge_signal_dart_session(int n_pairs,
                                            const char* const* labels,
                                            const int64_t* ports);

// ---------------------------------------------------------------------------
// Public C API (mirrors the SeriousPythonRunConfig the Dart side builds)
// ---------------------------------------------------------------------------

typedef PyObject* (*sp_pyinit_func_t)(void);

typedef enum {
    SP_RUN_PATH   = 0,  // run a Python file via PyRun_SimpleFile
    SP_RUN_SCRIPT = 1,  // run a Python source string via PyRun_SimpleString
} sp_run_mode_t;

typedef struct {
    sp_run_mode_t mode;
    const char*   app_path;       // PATH: path to .py file to execute
    const char*   script_source;  // SCRIPT: Python source string
    const char*   program_name;   // sys.argv[0]; nullable
    const char**  module_paths;   // additional sys.path entries; NULL-terminated; nullable
    const char**  env_keys;       // env vars set before Py_Initialize; NULL-terminated
    const char**  env_values;     // parallel to env_keys
    int           sync;           // 0 (default) = worker thread + post to port; 1 = inline
    int64_t       completion_port; // Dart native port for async; ignored if sync=1
} sp_run_config_t;

EXPORT int  serious_python_register_extension(const char* name, sp_pyinit_func_t init_fn);
EXPORT int  serious_python_run(const sp_run_config_t* cfg);
EXPORT int  serious_python_request_stop(void);
EXPORT void serious_python_finalize(void);

// ---------------------------------------------------------------------------
// Registered Python extensions (in addition to dart_bridge)
// ---------------------------------------------------------------------------

typedef struct sp_ext {
    char*            name;
    sp_pyinit_func_t init_fn;
    struct sp_ext*   next;
} sp_ext_t;

static sp_ext_t* g_registered_exts = NULL;

EXPORT int serious_python_register_extension(const char* name, sp_pyinit_func_t init_fn) {
    if (!name || !init_fn) return -1;
    sp_ext_t* e = (sp_ext_t*)calloc(1, sizeof(sp_ext_t));
    if (!e) return -1;
    e->name = strdup(name);
    if (!e->name) { free(e); return -1; }
    e->init_fn = init_fn;
    e->next = g_registered_exts;
    g_registered_exts = e;
    return 0;
}

// ---------------------------------------------------------------------------
// Run state — deep-copy of the caller's config so the worker thread doesn't
// outlive Dart-owned strings.
// ---------------------------------------------------------------------------

typedef struct {
    sp_run_mode_t mode;
    char*         app_path;
    char*         script_source;
    char*         program_name;
    char**        module_paths;
    size_t        module_paths_count;
    char**        env_keys;
    char**        env_values;
    size_t        env_count;
    int64_t       completion_port;
} sp_state_t;

static char** dup_string_array(const char** src, size_t* out_count) {
    *out_count = 0;
    if (!src) return NULL;
    size_t n = 0;
    while (src[n]) n++;
    char** dst = (char**)calloc(n + 1, sizeof(char*));
    if (!dst) return NULL;
    for (size_t i = 0; i < n; i++) {
        dst[i] = strdup(src[i]);
        if (!dst[i]) {
            for (size_t j = 0; j < i; j++) free(dst[j]);
            free(dst);
            return NULL;
        }
    }
    *out_count = n;
    return dst;
}

static void free_string_array(char** arr, size_t count) {
    if (!arr) return;
    for (size_t i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

static void sp_state_free(sp_state_t* st) {
    if (!st) return;
    free(st->app_path);
    free(st->script_source);
    free(st->program_name);
    free_string_array(st->module_paths, st->module_paths_count);
    free_string_array(st->env_keys, st->env_count);
    free_string_array(st->env_values, st->env_count);
    free(st);
}

static sp_state_t* sp_state_from_config(const sp_run_config_t* cfg) {
    sp_state_t* st = (sp_state_t*)calloc(1, sizeof(sp_state_t));
    if (!st) return NULL;

    st->mode = cfg->mode;
    st->completion_port = cfg->completion_port;

    if (cfg->app_path)      st->app_path      = strdup(cfg->app_path);
    if (cfg->script_source) st->script_source = strdup(cfg->script_source);
    if (cfg->program_name)  st->program_name  = strdup(cfg->program_name);

    st->module_paths = dup_string_array(cfg->module_paths, &st->module_paths_count);

    if (cfg->env_keys && cfg->env_values) {
        size_t k_count = 0, v_count = 0;
        st->env_keys   = dup_string_array(cfg->env_keys,   &k_count);
        st->env_values = dup_string_array(cfg->env_values, &v_count);
        st->env_count  = (k_count < v_count) ? k_count : v_count;
    }

    return st;
}

// ---------------------------------------------------------------------------
// Python lifecycle
// ---------------------------------------------------------------------------

// Apply env vars BEFORE Py_Initialize so PYTHONHOME / PYTHONPATH / etc. are
// observed during interpreter startup.
static void sp_apply_env(sp_state_t* st) {
    for (size_t i = 0; i < st->env_count; i++) {
        sp_setenv(st->env_keys[i], st->env_values[i]);
    }
}

// Register dart_bridge + any user-registered extensions with the inittab.
// Must be called BEFORE Py_Initialize (PyImport_AppendInittab requirement).
static int sp_apply_inittab(void) {
    if (PyImport_AppendInittab("dart_bridge", PyInit_dart_bridge) != 0) {
        fprintf(stderr, "[serious_python_run] inittab append failed for dart_bridge\n");
        return -1;
    }
    for (sp_ext_t* e = g_registered_exts; e; e = e->next) {
        if (PyImport_AppendInittab(e->name, e->init_fn) != 0) {
            fprintf(stderr, "[serious_python_run] inittab append failed for %s\n", e->name);
            return -1;
        }
    }
    return 0;
}

// Insert module_paths into sys.path AFTER Py_Initialize. Uses
// PyRun_SimpleString because PyConfig.module_search_paths isn't in the
// Limited API as of Python 3.12.
static int sp_apply_module_paths(sp_state_t* st) {
    if (st->module_paths_count == 0) return 0;

    // Build "sys.path[:0] = [...]" — insert all entries at the front in order.
    // Conservative buffer estimate: per-entry ~3x worst-case escaping.
    size_t total = 64;
    for (size_t i = 0; i < st->module_paths_count; i++) {
        total += strlen(st->module_paths[i]) * 3 + 8;
    }
    char* code = (char*)malloc(total);
    if (!code) return -1;

    char* p = code;
    int n = snprintf(p, total, "import sys\nsys.path[:0] = [");
    p += n;

    for (size_t i = 0; i < st->module_paths_count; i++) {
        // Quote with repr-safe escaping: replace backslash and apostrophe.
        // Simpler approach: use Python triple-quoted raw string? No — use
        // explicit escaping.
        n = snprintf(p, total - (p - code), "%sr'", i == 0 ? "" : ", ");
        p += n;
        const char* src = st->module_paths[i];
        for (; *src && (size_t)(p - code) < total - 4; src++) {
            if (*src == '\'') {
                // r-strings can't contain unescaped quotes that match;
                // fall back to concatenation.
                *p++ = '\''; *p++ = ' '; *p++ = '"'; *p++ = '\''; *p++ = '"';
                *p++ = ' '; *p++ = 'r'; *p++ = '\'';
            } else {
                *p++ = *src;
            }
        }
        *p++ = '\'';
    }
    *p++ = ']';
    *p   = '\0';

    int rc = sp_pyrun_string(code, "<sp_module_paths>");
    free(code);
    if (rc != 0) {
        fprintf(stderr, "[serious_python_run] sys.path injection failed\n");
        return -1;
    }
    return 0;
}

// Set sys.argv[0] to program_name (default "python").
static int sp_apply_program_name(sp_state_t* st) {
    const char* name = st->program_name ? st->program_name : "python";
    char buf[1024];
    // Naive escape: rely on caller not providing wild characters; program
    // name is typically a bundle identifier or "python".
    snprintf(buf, sizeof(buf), "import sys; sys.argv = [r'''%s''']", name);
    return sp_pyrun_string(buf, "<sp_program_name>") == 0 ? 0 : -1;
}

// Run the configured target. Returns exit code (0 = OK).
static int sp_run_target(sp_state_t* st) {
    if (st->mode == SP_RUN_SCRIPT) {
        if (!st->script_source) return 1;
        int rc = sp_pyrun_string(st->script_source, "<sp_script>");
        return rc == 0 ? 0 : 1;
    }
    // SP_RUN_PATH
    if (!st->app_path) return 1;
    FILE* fp = fopen(st->app_path, "rb");
    if (!fp) {
        fprintf(stderr, "[serious_python_run] cannot open %s\n", st->app_path);
        return 1;
    }
    int rc = sp_pyrun_file(fp, st->app_path);
    fclose(fp);
    return rc == 0 ? 0 : 1;
}

// Extract dart_bridge port env vars from this run's config and signal
// the running Python program that a new Dart VM session is active.
//
// This is the process-reuse path: libdart_bridge (and Python) stayed
// loaded from the previous Dart VM, but that VM has been torn down and a
// new one is starting. The new VM passes its fresh native port numbers
// via env vars; we forward them as a labeled map to Python's registered
// session-restart handlers (FletDartBridgeServer's restart loop, the
// python.dart sys.exit patcher, etc.).
static void sp_signal_session_from_env(sp_state_t* st) {
    // Recognised env keys → restart-handler labels. The Dart side stamps
    // both env vars (for fresh start) and calls signalDartSession (for
    // reuse) with parallel arrays; here we reconstruct the labeled form
    // from whichever env vars are present in this run's config.
    static const struct {
        const char* env_key;
        const char* label;
    } known[] = {
        {"FLET_DART_BRIDGE_PORT",      "protocol"},
        {"FLET_DART_BRIDGE_EXIT_PORT", "exit"},
    };
    const int known_count = (int)(sizeof(known) / sizeof(known[0]));

    const char* labels[8];
    int64_t     ports[8];
    int n = 0;

    for (int i = 0; i < known_count && n < (int)(sizeof(labels) / sizeof(labels[0])); i++) {
        // Look the env key up in st->env_keys/values (the deep-copy made
        // by sp_state_from_config). The host process env was already
        // updated by sp_apply_env on fresh start, but on reuse we don't
        // re-call that; the values in st are the authoritative source.
        for (size_t j = 0; j < st->env_count; j++) {
            if (st->env_keys[j] && strcmp(st->env_keys[j], known[i].env_key) == 0
                && st->env_values[j]) {
                long long v = strtoll(st->env_values[j], NULL, 10);
                if (v > 0) {
                    labels[n] = known[i].label;
                    ports[n]  = (int64_t)v;
                    n++;
                }
                break;
            }
        }
    }

    if (n > 0) {
        dart_bridge_signal_dart_session(n, labels, ports);
    }
}

// Drive the full Python lifecycle for this run. Returns the target's
// exit code (or 1 on internal failure).
static int sp_run_python(sp_state_t* st) {
    // Process-reuse path: Python is still up from a previous Dart VM
    // (typical on Android when the OS kept the process alive across a
    // back-button exit). Don't re-Py_Initialize — that would crash.
    // Don't re-run the user's main module — it's still running. Instead:
    //
    // 1. Update the process env vars to this run's values. Python's own
    //    bootstrap reads them once on first start; this keeps the host
    //    env consistent for anything that reads `os.environ` later.
    // 2. Forward the new Dart native port numbers to Python via the
    //    session-restart callback so registered handlers (flet's
    //    FletDartBridgeServer restart loop, the python.dart sys.exit
    //    patcher) can rewire to them.
    if (Py_IsInitialized()) {
        sp_apply_env(st);
        sp_signal_session_from_env(st);
        return 0;
    }

    sp_apply_env(st);

    if (sp_apply_inittab() != 0) {
        return 1;
    }

#ifdef __ANDROID__
    // Start with site disabled so the serious_python native-module finder is
    // installed BEFORE `site` runs (some lib-dynload extension modules could
    // otherwise be imported during site, before the finder exists). PYTHONHOME /
    // PYTHONPATH (applied via sp_apply_env above) are honoured by PyConfig, so the
    // stdlib zip is on sys.path and `_sp_bootstrap` is importable here.
    {
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        config.site_import = 0;
        PyStatus status = Py_InitializeFromConfig(&config);
        PyConfig_Clear(&config);
        if (PyStatus_Exception(status)) {
            fprintf(stderr, "[serious_python_run] Py_InitializeFromConfig failed\n");
            return 1;
        }
    }
#else
    Py_Initialize();

    if (!Py_IsInitialized()) {
        fprintf(stderr, "[serious_python_run] Py_Initialize failed\n");
        return 1;
    }
#endif

    // Install Python-level sys.stdout / sys.stderr wrappers that forward
    // to the platform's native log sink (logcat on Android, os_log on
    // iOS, fd 1/2 passthrough on desktop). Done immediately post-init so
    // even bootstrap-time prints (sys.path injection, program-name
    // setup, the user's module top-level statements) land in logcat
    // rather than vanishing into /dev/null on mobile.
    dart_bridge_install_stdio_redirect();

#ifdef __ANDROID__
    // Install the jniLibs native-module finder, THEN run site (which we disabled
    // at init). The finder resolves CPython extension modules relocated into
    // jniLibs/<abi>/ from their .soref markers in the stdlib/sitepackages zips.
    if (sp_pyrun_string("import _sp_bootstrap; _sp_bootstrap.install()", "<sp_finder>") != 0
        || sp_pyrun_string("import site; site.main()", "<sp_site>") != 0) {
        Py_Finalize();
        return 1;
    }
#endif

    if (sp_apply_program_name(st) != 0 || sp_apply_module_paths(st) != 0) {
        Py_Finalize();
        return 1;
    }

    int exit_code = sp_run_target(st);

    dart_bridge_clear_handlers();
    Py_Finalize();
    return exit_code;
}

// ---------------------------------------------------------------------------
// Async worker thread
// ---------------------------------------------------------------------------

#if defined(_WIN32)
static unsigned __stdcall sp_worker(void* arg) {
#else
static void* sp_worker(void* arg) {
#endif
    sp_state_t* st = (sp_state_t*)arg;
    int exit_code = sp_run_python(st);

    if (st->completion_port != 0 && Dart_PostInteger_DL != NULL) {
        Dart_PostInteger_DL(st->completion_port, (int64_t)exit_code);
    }

    sp_state_free(st);
#if defined(_WIN32)
    return (unsigned)exit_code;
#else
    return NULL;
#endif
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

EXPORT int serious_python_run(const sp_run_config_t* cfg) {
    if (!cfg) return -1;

    sp_state_t* st = sp_state_from_config(cfg);
    if (!st) return -1;

    if (cfg->sync) {
        int exit_code = sp_run_python(st);
        sp_state_free(st);
        return exit_code;
    }

#if defined(_WIN32)
    uintptr_t h = _beginthreadex(NULL, 0, sp_worker, st, 0, NULL);
    if (h == 0) {
        sp_state_free(st);
        return -1;
    }
    CloseHandle((HANDLE)h);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, sp_worker, st) != 0) {
        sp_state_free(st);
        return -1;
    }
    pthread_detach(thread);
#endif

    return 0;
}

// Best-effort cooperative stop: raises KeyboardInterrupt on the main Python
// thread. The Python code is responsible for handling it cleanly.
EXPORT int serious_python_request_stop(void) {
    if (!Py_IsInitialized()) return -1;
    PyGILState_STATE g = PyGILState_Ensure();
    PyErr_SetInterrupt();
    PyGILState_Release(g);
    return 0;
}

EXPORT void serious_python_finalize(void) {
    if (Py_IsInitialized()) {
        dart_bridge_clear_handlers();
        Py_Finalize();
    }
}
