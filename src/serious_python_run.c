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
// env vars (setenv) pre-Py_Initialize, and C API calls plus a bootstrap
// script post-init, for sys.path / sys.argv adjustments.

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
// reimplement them using Py_CompileString + PyEval_EvalCode, which ARE in
// the Limited API (since 3.2). Behavior matches CPython's own
// PyRun_SimpleStringFlags / PyRun_SimpleFileExFlags (see Python/pythonrun.c).
static int sp_pyrun_string_in_globals(const char *source, const char *filename, PyObject *globals) {
    PyObject *code = Py_CompileString(source, filename, Py_file_input);
    if (!code) {
        PyErr_Print();
        return -1;
    }

    PyObject *result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!result) {
        PyErr_Print();
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

// Run source in __main__'s globals (PyRun_SimpleString semantics).
static int sp_pyrun_string(const char *source, const char *filename) {
    PyObject *main_mod = PyImport_AddModule("__main__");  // borrowed
    if (!main_mod) {
        PyErr_Print();
        return -1;
    }
    PyObject *globals = PyModule_GetDict(main_mod);  // borrowed
    if (!globals) {
        PyErr_Print();
        return -1;
    }
    return sp_pyrun_string_in_globals(source, filename, globals);
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
#include <wchar.h>
#define EXPORT __declspec(dllexport)
#define SP_PATH_SEP "\\"
#define SP_PYPATH_SEP ";"
// Dart passes FFI strings as UTF-8. On Windows, narrow CRT APIs interpret
// char* paths and environment values using the process ANSI code page, so
// non-ASCII paths can be corrupted. Convert to UTF-16 at the OS/CRT boundary
// and use the wide APIs instead.
static wchar_t* sp_utf8_to_wide(const char* s) {
    if (!s) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    if ((size_t)wlen > ((size_t)-1) / sizeof(wchar_t)) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, wlen) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}
static int sp_setenv(const char* k, const char* v) {
    wchar_t* wk = sp_utf8_to_wide(k);
    wchar_t* wv = sp_utf8_to_wide(v ? v : "");
    int rc = -1;
    if (wk && wv) rc = _wputenv_s(wk, wv);
    free(wk);
    free(wv);
    return rc;
}
static int sp_unsetenv(const char* k) {
    wchar_t* wk = sp_utf8_to_wide(k);
    int rc = -1;
    if (wk) rc = _wputenv_s(wk, L"");
    free(wk);
    return rc;
}
static int sp_getenv_present(const char* k) {
    wchar_t* wk = sp_utf8_to_wide(k);
    if (!wk) return 0;
    size_t needed = 0;
    errno_t rc = _wgetenv_s(&needed, NULL, 0, wk);
    free(wk);
    return rc == 0 && needed > 0;
}
#else
#include <pthread.h>
#include <unistd.h>
// `used` is mainly for Darwin/Mach-O: dart_bridge is statically linked into
// the host app, and some public entry points are discovered later via dlsym
// / Dart FFI. Mark exported functions as used so the host linker's dead-strip
// pass does not discard exports with no ordinary C call site.
#define EXPORT __attribute__((visibility("default"), used))
#define SP_PATH_SEP "/"
#define SP_PYPATH_SEP ":"
static int sp_setenv(const char* k, const char* v) { return setenv(k, v, 1); }
static int sp_unsetenv(const char* k) { return unsetenv(k); }
static int sp_getenv_present(const char* k) { return getenv(k) != NULL; }
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
EXPORT void serious_python_hard_exit(int exit_code);
EXPORT int  serious_python_is_mp_invocation(int argc, char** argv);
EXPORT int  serious_python_main(int argc, char** argv);
#if defined(_WIN32)
// Wide-char variants for Windows hosts (wWinMain argv is wchar_t**; going
// through the narrow versions would decode through the ANSI code page).
EXPORT int  serious_python_is_mp_invocation_w(int argc, wchar_t** argv);
EXPORT int  serious_python_main_w(int argc, wchar_t** argv);
#endif

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

// Apply env vars before Python startup so PYTHONHOME, PYTHONPATH, and runtime
// bridge ports are visible during initialization. Treat failures as fatal;
// continuing with a partially configured embedded interpreter gives misleading
// startup errors.
static int sp_apply_env(sp_state_t* st) {
    for (size_t i = 0; i < st->env_count; i++) {
        if (sp_setenv(st->env_keys[i], st->env_values[i]) != 0) {
            fprintf(stderr, "[serious_python_run] failed to set env var %s\n",
                    st->env_keys[i] ? st->env_keys[i] : "<null>");
            return -1;
        }
    }
    return 0;
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

// Python program run by sp_apply_module_paths with private globals holding
// `_sp_paths` (the module-path list), so its imports and temporaries cannot
// leak into the user's __main__. The plugins also pass these paths through
// PYTHONPATH, so drop the existing occurrences before re-inserting the list
// at the front in the configured order. Register each directory with
// site.addsitedir() so its `.pth` files are processed — CPython never does
// this for PYTHONPATH entries; non-directories (such as Android zips) stay
// on sys.path unchanged.
static const char* SP_MODULE_PATHS_SCRIPT =
    "import sys, os, site\n"
    "_sp_norm = {os.path.normcase(os.path.abspath(_p)) for _p in _sp_paths}\n"
    "sys.path[:] = [_p for _p in sys.path\n"
    "               if os.path.normcase(os.path.abspath(_p)) not in _sp_norm]\n"
    "sys.path[:0] = _sp_paths\n"
    "for _p in _sp_paths:\n"
    "    if os.path.isdir(_p):\n"
    "        site.addsitedir(_p)\n";

// Shared failure tail for sp_apply_module_paths: release the temporaries,
// surface the pending Python error, and flag the failure on stderr.
static int sp_paths_failed(PyObject* list, PyObject* globals) {
    Py_XDECREF(list);
    Py_XDECREF(globals);
    if (PyErr_Occurred()) PyErr_Print();
    fprintf(stderr, "[serious_python_run] sys.path injection failed\n");
    return -1;
}

// Put module_paths on sys.path and register them as site directories: build
// the list with the C API, expose it as `_sp_paths` in a private globals
// dict, and run SP_MODULE_PATHS_SCRIPT there. The paths travel as objects,
// never as quoted source text, so nothing needs escaping.
// Done after Py_Initialize because PyConfig.module_search_paths is unavailable
// under the Python 3.12 Limited API targeted by this library.
static int sp_apply_module_paths(sp_state_t* st) {
    if (st->module_paths_count == 0) return 0;

    PyObject* list = PyList_New((Py_ssize_t)st->module_paths_count);
    if (!list) return sp_paths_failed(NULL, NULL);
    for (size_t i = 0; i < st->module_paths_count; i++) {
        PyObject* item = PyUnicode_FromString(st->module_paths[i]);
        // PyList_SetItem steals `item`, releasing it even when it fails.
        if (!item || PyList_SetItem(list, (Py_ssize_t)i, item) != 0) {
            return sp_paths_failed(list, NULL);
        }
    }

    PyObject* globals = PyDict_New();
    if (!globals) return sp_paths_failed(list, NULL);

    // PyEval_GetBuiltins is the only builtins accessor in the 3.12 Limited
    // API (PyEval_GetFrameBuiltins is 3.13+); with no frame executing it
    // returns the interpreter's builtins.
    PyObject* builtins = PyEval_GetBuiltins();  // borrowed
    if (!builtins) {
        PyErr_SetString(PyExc_RuntimeError, "builtins are not available");
        return sp_paths_failed(list, globals);
    }
    if (PyDict_SetItemString(globals, "__builtins__", builtins) != 0
        || PyDict_SetItemString(globals, "_sp_paths", list) != 0) {
        return sp_paths_failed(list, globals);
    }
    Py_DECREF(list);  // the globals dict holds its own reference now

    int rc = sp_pyrun_string_in_globals(SP_MODULE_PATHS_SCRIPT, "<sp_module_paths>", globals);
    Py_DECREF(globals);
    if (rc != 0) return sp_paths_failed(NULL, NULL);
    return 0;
}

// Set sys.argv to [program_name] (default "python").
static int sp_apply_program_name(sp_state_t* st) {
    const char* name = st->program_name ? st->program_name : "python";
    PyObject* argv = PyList_New(1);
    if (!argv) {
        PyErr_Print();
        return -1;
    }
    PyObject* item = PyUnicode_FromString(name);
    // PyList_SetItem steals `item`, releasing it even when it fails.
    if (!item || PyList_SetItem(argv, 0, item) != 0
        || PySys_SetObject("argv", argv) != 0) {
        Py_DECREF(argv);
        PyErr_Print();
        return -1;
    }
    Py_DECREF(argv);  // sys's dict holds its own reference now
    return 0;
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
#if defined(_WIN32)
    wchar_t* wpath = sp_utf8_to_wide(st->app_path);
    FILE* fp = NULL;
    if (wpath) {
        errno_t err = _wfopen_s(&fp, wpath, L"rb");
        if (err != 0) fp = NULL;
    }
    free(wpath);
#else
    FILE* fp = fopen(st->app_path, "rb");
#endif
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
        if (sp_apply_env(st) != 0) {
            return 1;
        }
        sp_signal_session_from_env(st);
        return 0;
    }

    if (sp_apply_env(st) != 0) {
        return 1;
    }

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
#if defined(_WIN32)
    // Force Python UTF-8 Mode before Py_Initialize so the embedded interpreter
    // uses UTF-8 for default text encoding on Windows locales whose ANSI code
    // page is not UTF-8. PyPreConfig would be the modern API, but this path is
    // built with Py_LIMITED_API, where PyPreConfig is unavailable.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4996)
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    Py_UTF8Mode = 1;
#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
#endif

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

// Terminate the process WITHOUT running atexit handlers or C++ static
// destructors. Does not return.
//
// The interpreter runs on a detached thread (see sp_worker) that may still be
// executing native extension code when the host decides to quit. A normal
// exit() runs __cxa_finalize, destroying the C++ statics inside every loaded
// extension module out from under that thread (pybind11 type-caster maps,
// numpy's internals), which faults on whichever one it touches next. The host
// is exiting anyway, so there is nothing left to clean up that the kernel will
// not reclaim; skipping teardown removes the window entirely.
//
// Callers: the native app runners on desktop window close, and the Dart side
// when the Python program requests an exit (dart:io's exit() would run the
// same teardown).
EXPORT void serious_python_hard_exit(int exit_code) {
#if defined(_WIN32)
    // _exit()/ExitProcess still run DLL_PROCESS_DETACH, where the CRT's
    // DllMain runs each DLL's static destructors, the very thing being
    // avoided. TerminateProcess is the only primitive that skips it.
    TerminateProcess(GetCurrentProcess(), (UINT)exit_code);
#else
    _exit(exit_code);
#endif
}

// ---------------------------------------------------------------------------
// Multiprocessing child-process support
//
// Python's `multiprocessing` spawn/forkserver paths, plus the resource tracker,
// launch helper processes by re-executing `sys.executable` with a CPython
// command line. In serious_python-hosted apps, the launcher points
// `sys.executable` at the host app binary, so those helpers would otherwise
// start another GUI app instead of a headless Python interpreter.
// See https://github.com/flet-dev/flet/issues/4283.
//
// The host runner should call `serious_python_is_mp_invocation` before any
// UI/engine initialization. If it returns non-zero, the runner should exit with
// `serious_python_main(argc, argv)`, turning the host binary into a plain
// interpreter for that multiprocessing helper process only.
//
// Command-line shapes the detector is meant to catch:
//
//   spawn worker:      [exe, *flags, '-c', 'from multiprocessing.spawn import
//                       spawn_main; spawn_main(...)', '--multiprocessing-fork']
//   frozen-style:      [exe, '--multiprocessing-fork', 'k=v', ...]
//   resource tracker:  [exe, *flags, '-c', 'from multiprocessing.
//                       resource_tracker import main;main(fd)']
//   forkserver:        [exe, *flags, '-c', 'from multiprocessing.forkserver
//                       import main; ...']
// ---------------------------------------------------------------------------

static int sp_starts_with(const char* s, const char* prefix) {
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}

EXPORT int serious_python_is_mp_invocation(int argc, char** argv) {
    if (!argv) return 0;
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (strcmp(argv[i], "--multiprocessing-fork") == 0) {
            return 1;
        }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            const char* prog = argv[i + 1];
            // intentionally kept broad/prefix-based to avoid hardcoding
            if (sp_starts_with(prog, "from multiprocessing.") ||
                sp_starts_with(prog, "import sys; from multiprocessing.")) {
                return 1;
            }
        }
    }
    return 0;
}

// Shared preparation before handing the process to Py_Main/Py_BytesMain.
// Returns 0 to proceed, non-zero exit code on a hard configuration error.
static int sp_child_preflight(void) {
    // Prevent re-exec'd multiprocessing children from inheriting PYTHONINSPECT,
    // or they may stay open in interactive mode after the `-c` helper command finishes.
    sp_unsetenv("PYTHONINSPECT");

#if defined(_WIN32)
    // Py_Main/Py_BytesMain run their own pre-initialization and read PYTHONUTF8
    // from the environment. Set it here so multiprocessing helper processes
    // match the parent interpreter's UTF-8 behavior.
    sp_setenv("PYTHONUTF8", "1");
#endif

    if (!sp_getenv_present("PYTHONHOME") && !sp_getenv_present("PYTHONPATH")) {
        fprintf(stderr,
                "[serious_python_main] neither PYTHONHOME nor PYTHONPATH is "
                "set; the embedded stdlib cannot be located. multiprocessing "
                "children must inherit the host app's environment.\n");
        return 64;
    }

    // Make the built-in dart_bridge module importable in the child too. The child
    // normally should not talk to Dart, but inherited user/import paths may import
    // dart_bridge-aware code; the import should fail soft rather than because the
    // built-in module is missing. Must happen before Py_Main initializes Python.
    if (PyImport_AppendInittab("dart_bridge", PyInit_dart_bridge) != 0) {
        fprintf(stderr,
                "[serious_python_main] inittab append failed for dart_bridge\n");
        // Non-fatal: proceed without the builtin.
    }
    return 0;
}

// Run this process as a plain CPython interpreter for the given multiprocessing
// command line. Returns the interpreter's exit code; the caller should exit the
// process with it without running any other app code.
//
// The interpreter locates the embedded stdlib/site-packages through the
// PYTHONHOME/PYTHONPATH environment variables inherited from the parent
// process. The parent's `serious_python_run` sets them process-wide before
// Py_Initialize, so multiprocessing children inherit them.
EXPORT int serious_python_main(int argc, char** argv) {
    int rc = sp_child_preflight();
    if (rc != 0) return rc;

    // Py_BytesMain (stable ABI, 3.8+) runs the full standard interpreter
    // lifecycle: parses argv (-B/-s/-c/...), honors PYTHONHOME/PYTHONPATH,
    // executes the '-c' payload with sys.argv = ['-c', *rest] (which is what
    // multiprocessing.spawn.spawn_main asserts on), and returns the exit
    // code after finalization.
    return Py_BytesMain(argc, argv);
}

#if defined(_WIN32)
static int sp_wstarts_with(const wchar_t* s, const wchar_t* prefix) {
    return s && wcsncmp(s, prefix, wcslen(prefix)) == 0;
}

EXPORT int serious_python_is_mp_invocation_w(int argc, wchar_t** argv) {
    if (!argv) return 0;
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (wcscmp(argv[i], L"--multiprocessing-fork") == 0) {
            return 1;
        }
        if (wcscmp(argv[i], L"-c") == 0 && i + 1 < argc) {
            const wchar_t* prog = argv[i + 1];
            if (sp_wstarts_with(prog, L"from multiprocessing.") ||
                sp_wstarts_with(prog, L"import sys; from multiprocessing.")) {
                return 1;
            }
        }
    }
    return 0;
}

EXPORT int serious_python_main_w(int argc, wchar_t** argv) {
    int rc = sp_child_preflight();
    if (rc != 0) return rc;
    return Py_Main(argc, argv);
}
#endif
