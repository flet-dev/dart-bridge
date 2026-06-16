// dart_bridge — in-process Dart ↔ Python byte transport.
//
// Compiled into a single libdart_bridge binary (.so / .dll / static lib in
// xcframework) consumed by serious_python's platform plugins. Two C-callable
// surfaces live here:
//
//   1. Dart-callable: DartBridge_InitDartApiDL, DartBridge_EnqueueMessage.
//      Looked up by Dart via FFI.
//
//   2. Python-callable: a built-in module `dart_bridge` registered via
//      PyImport_AppendInittab (see serious_python_run.c) before Py_Initialize.
//      Exposes two methods to Python code:
//        - set_enqueue_handler_func(port, callable)   # callable may be None
//        - send_bytes(port, payload)
//
// Multi-channel model (v1.2.0+):
// The 64-bit Dart native port acts as the channel key in both directions:
//   - Python→Dart: send_bytes(port, ...) posts directly to that port.
//   - Dart→Python: DartBridge_EnqueueMessage(port, ...) looks up a Python
//     handler previously registered for that port via set_enqueue_handler_func.
//
// Multiple PythonBridge instances on the Dart side (UI channel, logging
// channel, future camera-stream channel, ...) each own a distinct ReceivePort
// and register a corresponding Python-side handler under that port.
//
// Handler storage is a singly-linked list, mutated only while holding the
// GIL — no additional locking needed. Expected list size is small (<10).

#define PY_SSIZE_T_CLEAN
// Limited API / abi3: one compiled binary works across all Python 3.12+
// minor versions. Every Py* symbol below is in the Limited API since 3.2
// (3.4 for PyGILState_*).
#define Py_LIMITED_API 0x030c0000
#include <Python.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dart_api/dart_api_dl.h"

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#elif defined(__ANDROID__)
#include <android/log.h>
#define EXPORT __attribute__((visibility("default")))
#elif defined(__APPLE__)
#include <os/log.h>
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------------------
// Keyed handler list (port → PyObject* callable)
// ---------------------------------------------------------------------------

typedef struct handler_entry {
    int64_t               port;
    PyObject*             handler;  // strong ref while present
    struct handler_entry* next;
} handler_entry;

// Mutated only while holding the GIL.
static handler_entry* g_handlers = NULL;

// Forward declaration — definition lives further down with the
// session-restart machinery. Called by dart_bridge_clear_handlers around
// Py_Finalize to release session-restart subscribers too.
static void clear_restart_handlers_locked(void);

// Look up a handler by port. Returns borrowed reference (caller is under GIL).
static PyObject* find_handler_locked(int64_t port) {
    for (handler_entry* e = g_handlers; e; e = e->next) {
        if (e->port == port) return e->handler;
    }
    return NULL;
}

// Replace (or remove, if handler==NULL) the entry for `port`. Steals the
// reference on `handler` (caller has Py_INCREF'd). Returns 0 on success.
static int set_handler_locked(int64_t port, PyObject* handler) {
    handler_entry** prev = &g_handlers;
    for (handler_entry* e = *prev; e; prev = &e->next, e = e->next) {
        if (e->port == port) {
            Py_XDECREF(e->handler);
            if (handler) {
                e->handler = handler;
            } else {
                *prev = e->next;
                free(e);
            }
            return 0;
        }
    }
    if (!handler) return 0;  // unregistering an entry that doesn't exist: no-op
    handler_entry* e = (handler_entry*)malloc(sizeof(handler_entry));
    if (!e) {
        Py_DECREF(handler);
        PyErr_NoMemory();
        return -1;
    }
    e->port = port;
    e->handler = handler;
    e->next = g_handlers;
    g_handlers = e;
    return 0;
}

// Called by serious_python_run.c around Py_Finalize so handler PyObjects
// (both the port→callable map AND the session-restart subscribers) are
// released before the interpreter goes away.
EXPORT void dart_bridge_clear_handlers(void) {
    handler_entry* e = g_handlers;
    g_handlers = NULL;
    while (e) {
        handler_entry* next = e->next;
        Py_XDECREF(e->handler);
        free(e);
        e = next;
    }
    clear_restart_handlers_locked();
}

// ---------------------------------------------------------------------------
// Session-restart handler list (Python callbacks fired on Dart VM restart)
//
// On platforms where the OS keeps the process alive across a Dart VM
// restart (notably Android — back-button quit, then re-launch when the OS
// hasn't OOM-killed the process), the new Dart VM allocates fresh
// PythonBridge native ports. The Python program is still running with
// handlers registered on the OLD (now-dead) ports.
//
// `dart_bridge_signal_dart_session` is called by the new Dart VM from
// PythonBridge's first init each launch. If Python is up, it dispatches
// to every registered Python callback with a {label: new_port} dict so
// Python-side consumers (flet's FletDartBridgeServer, the python.dart
// `sys.exit` patcher, etc.) can rewire to the new ports.
// ---------------------------------------------------------------------------

typedef struct restart_entry {
    PyObject*             handler;  // strong ref
    struct restart_entry* next;
} restart_entry;

static restart_entry* g_restart_handlers = NULL;

// Add a callback. Steals the reference. GIL held.
static int add_restart_handler_locked(PyObject* handler) {
    restart_entry* e = (restart_entry*)malloc(sizeof(restart_entry));
    if (!e) {
        Py_DECREF(handler);
        PyErr_NoMemory();
        return -1;
    }
    e->handler = handler;
    e->next = g_restart_handlers;
    g_restart_handlers = e;
    return 0;
}

// Released alongside g_handlers around Py_Finalize.
static void clear_restart_handlers_locked(void) {
    restart_entry* e = g_restart_handlers;
    g_restart_handlers = NULL;
    while (e) {
        restart_entry* next = e->next;
        Py_XDECREF(e->handler);
        free(e);
        e = next;
    }
}

// Reports whether the embedded interpreter is up. Lets the Dart side
// detect process-reuse on its second startup.
EXPORT int dart_bridge_is_python_initialized(void) {
    return Py_IsInitialized() ? 1 : 0;
}

// Build a {label: port} Python dict from parallel C arrays. Returns a new
// reference; NULL on error (PyErr set). Caller holds the GIL.
static PyObject* build_port_map(int n_pairs,
                                const char* const* labels,
                                const int64_t* ports) {
    PyObject* d = PyDict_New();
    if (!d) return NULL;
    for (int i = 0; i < n_pairs; i++) {
        if (!labels[i]) continue;
        PyObject* v = PyLong_FromLongLong((long long)ports[i]);
        if (!v) { Py_DECREF(d); return NULL; }
        if (PyDict_SetItemString(d, labels[i], v) != 0) {
            Py_DECREF(v); Py_DECREF(d); return NULL;
        }
        Py_DECREF(v);
    }
    return d;
}

// Called by Dart on EVERY VM startup with the labeled new port numbers.
// If Python isn't up yet → no-op (fresh-start path uses env vars).
// If Python IS up → dispatch to every registered handler with the port
// map. Errors raised by handlers are printed (don't abort the others).
//
// Parallel arrays kept for FFI simplicity: labels[i] / ports[i] form one
// (key, value) pair. n_pairs is the count.
EXPORT void dart_bridge_signal_dart_session(int n_pairs,
                                            const char* const* labels,
                                            const int64_t* ports) {
    if (!Py_IsInitialized()) {
        return;
    }
    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* port_map = build_port_map(n_pairs, labels, ports);
    if (!port_map) {
        PyErr_Print();
        PyGILState_Release(gstate);
        return;
    }

    for (restart_entry* e = g_restart_handlers; e; e = e->next) {
        PyObject* result = PyObject_CallFunctionObjArgs(e->handler, port_map, NULL);
        if (!result) {
            PyErr_Print();  // don't abort remaining handlers
        }
        Py_XDECREF(result);
    }

    Py_DECREF(port_map);
    PyGILState_Release(gstate);
}

// ---------------------------------------------------------------------------
// stdout/stderr redirection
//
// On Android and iOS, fd 1/2 from a Flutter app go nowhere by default —
// Python `print()` and tracebacks become invisible. After Py_Initialize
// we replace sys.stdout / sys.stderr with file-like wrappers whose
// `write()` calls native_log_write, which dispatches to logcat / os_log /
// fwrite per platform.
// ---------------------------------------------------------------------------

static void native_log_write(int is_stderr, const char* buf, Py_ssize_t len) {
    if (len <= 0) return;
#if defined(__ANDROID__)
    // __android_log_write expects a NUL-terminated string. Buffer is
    // typically short (one write call per print line); copy + NUL-pad.
    char stack_buf[1024];
    char* tmp = stack_buf;
    if ((size_t)len + 1 > sizeof(stack_buf)) {
        tmp = (char*)malloc((size_t)len + 1);
        if (!tmp) return;
    }
    memcpy(tmp, buf, (size_t)len);
    tmp[len] = '\0';
    // Strip a single trailing newline so logcat doesn't double-space —
    // each __android_log_write call already adds its own line break.
    if (len > 0 && tmp[len - 1] == '\n') tmp[len - 1] = '\0';
    __android_log_write(is_stderr ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                        "flet.python", tmp);
    if (tmp != stack_buf) free(tmp);
#elif defined(__APPLE__)
    // os_log truncates at ~1KB by default; for longer payloads we'd want
    // to chunk. Most print()s are well under that. is_stderr maps to
    // OS_LOG_TYPE_ERROR, otherwise OS_LOG_TYPE_DEFAULT.
    char stack_buf[1024];
    char* tmp = stack_buf;
    if ((size_t)len + 1 > sizeof(stack_buf)) {
        tmp = (char*)malloc((size_t)len + 1);
        if (!tmp) return;
    }
    memcpy(tmp, buf, (size_t)len);
    tmp[len] = '\0';
    if (len > 0 && tmp[len - 1] == '\n') tmp[len - 1] = '\0';
    if (is_stderr) {
        os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "%{public}s", tmp);
    } else {
        os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "%{public}s", tmp);
    }
    if (tmp != stack_buf) free(tmp);
#else
    // Desktop: fd 1/2 work — passthrough preserves existing behavior so
    // `flet run` console output, CI logs, etc. keep flowing.
    fwrite(buf, 1, (size_t)len, is_stderr ? stderr : stdout);
    if (is_stderr) fflush(stderr);
#endif
}

// ---------------------------------------------------------------------------
// Dart-callable
// ---------------------------------------------------------------------------

EXPORT intptr_t DartBridge_InitDartApiDL(void* data) {
    return Dart_InitializeApiDL(data);
}

// Returns 0 on success, -1 if no handler is registered for `port` (caller
// may retry — typical reason is that Python hasn't called
// set_enqueue_handler_func yet), or -2 if the interpreter isn't up.
EXPORT int DartBridge_EnqueueMessage(int64_t port, const char* data, size_t len) {
    // Acquiring the GIL against an uninitialized interpreter triggers a
    // fatal PyMUTEX_LOCK failure (gil->mutex is uninitialized). Drop and
    // let the caller retry once Python is up.
    if (!Py_IsInitialized()) {
        return -2;
    }

    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* handler = find_handler_locked(port);
    if (!handler) {
        PyGILState_Release(gstate);
        return -1;
    }

    PyObject* arg = PyBytes_FromStringAndSize(data, (Py_ssize_t)len);
    if (!arg) {
        PyErr_Print();
        PyGILState_Release(gstate);
        return -1;
    }

    PyObject* result = PyObject_CallFunctionObjArgs(handler, arg, NULL);
    if (!result) {
        PyErr_Print();
    }

    Py_XDECREF(arg);
    Py_XDECREF(result);
    PyGILState_Release(gstate);
    return 0;
}

// ---------------------------------------------------------------------------
// Python-callable (built-in module via PyImport_AppendInittab)
// ---------------------------------------------------------------------------

static PyObject* py_set_enqueue_handler_func(PyObject* self, PyObject* args) {
    int64_t   port;
    PyObject* func;

    if (!PyArg_ParseTuple(args, "LO:set_enqueue_handler_func", &port, &func)) {
        return NULL;
    }

    if (func == Py_None) {
        if (set_handler_locked(port, NULL) != 0) return NULL;
        Py_RETURN_NONE;
    }

    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError,
                        "second argument must be callable or None");
        return NULL;
    }

    Py_INCREF(func);
    if (set_handler_locked(port, func) != 0) return NULL;  // steals our ref
    Py_RETURN_NONE;
}

static PyObject* py_send_bytes(PyObject* self, PyObject* args) {
    int64_t      port;
    const char*  buffer;
    Py_ssize_t   length;

    if (!PyArg_ParseTuple(args, "Ly#", &port, &buffer, &length)) {
        return NULL;
    }

    if (port == 0) {
        PyErr_SetString(PyExc_RuntimeError, "Dart port is 0 (invalid)");
        return NULL;
    }

    // Dart_PostCObject_DL is a function pointer populated by Dart_InitializeApiDL.
    // Calling it before init segfaults; surface a clean error instead.
    if (Dart_PostCObject_DL == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Dart API DL not initialized (call DartBridge_InitDartApiDL from Dart first)");
        return NULL;
    }

    Dart_CObject obj;
    obj.type = Dart_CObject_kTypedData;
    obj.value.as_typed_data.type = Dart_TypedData_kUint8;
    obj.value.as_typed_data.length = (int32_t)length;
    obj.value.as_typed_data.values = (void*)buffer;

    if (!Dart_PostCObject_DL(port, &obj)) {
        PyErr_SetString(PyExc_RuntimeError, "Dart_PostCObject_DL failed");
        return NULL;
    }
    Py_RETURN_TRUE;
}

static PyObject* py_add_session_restart_handler(PyObject* self, PyObject* arg) {
    if (!PyCallable_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "argument must be callable");
        return NULL;
    }
    Py_INCREF(arg);
    if (add_restart_handler_locked(arg) != 0) return NULL;  // steals our ref
    Py_RETURN_NONE;
}

// Internal: invoked from the Python file-like wrappers installed as
// sys.stdout / sys.stderr. Two args: (is_stderr: int, text: str). No
// return value (returns None).
static PyObject* py_native_log_write(PyObject* self, PyObject* args) {
    int          is_stderr;
    const char*  text;
    Py_ssize_t   length;
    if (!PyArg_ParseTuple(args, "is#:_native_log_write", &is_stderr, &text, &length)) {
        return NULL;
    }
    native_log_write(is_stderr, text, length);
    Py_RETURN_NONE;
}

static PyMethodDef dart_bridge_methods[] = {
    {"send_bytes", py_send_bytes, METH_VARARGS,
     "send_bytes(port, payload) — post a bytes payload to the Dart\n"
     "ReceivePort identified by its native port."},
    {"set_enqueue_handler_func", py_set_enqueue_handler_func, METH_VARARGS,
     "set_enqueue_handler_func(port, callable) — register a Python\n"
     "callable that receives bytes posted from Dart for the given port.\n"
     "Pass None as the callable to unregister."},
    {"add_session_restart_handler", py_add_session_restart_handler, METH_O,
     "add_session_restart_handler(callable) — register a callback that\n"
     "fires when a new Dart VM signals the running Python program with\n"
     "fresh native port numbers (e.g. Android process reuse). The callback\n"
     "receives a {label: port} dict. Multiple subscribers supported; each\n"
     "gets the full map."},
    {"_native_log_write", py_native_log_write, METH_VARARGS,
     "_native_log_write(is_stderr, text) — internal. Forwards bytes to the\n"
     "platform's native log sink (logcat / os_log / stderr). Used by the\n"
     "sys.stdout / sys.stderr wrappers installed at interpreter start."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef dart_bridge_module = {
    PyModuleDef_HEAD_INIT,
    "dart_bridge", NULL, -1, dart_bridge_methods
};

// Registered with CPython via PyImport_AppendInittab before Py_Initialize.
// User code's `import dart_bridge` resolves here.
PyMODINIT_FUNC PyInit_dart_bridge(void) {
    return PyModule_Create(&dart_bridge_module);
}

// Install Python-level sys.stdout / sys.stderr wrappers that forward
// writes to native_log_write. Called by serious_python_run.c right after
// Py_Initialize so even early bootstrap prints land in logcat / os_log.
//
// We do this in Python (not by replacing fd 1/2 directly) because
// Python's stdio is layered: PyObject_Print → sys.stdout.write → fd. The
// fd-replacement approach (dup2 onto a pipe + a reader thread) works but
// adds a thread + buffering; intercepting at sys.stdout is simpler and
// catches `print` directly. Native crashes that write directly to fd
// won't surface here — those need a different mechanism if we ever care.
EXPORT int dart_bridge_install_stdio_redirect(void) {
    if (!Py_IsInitialized()) return -1;
    PyGILState_STATE gstate = PyGILState_Ensure();

    // Define a tiny file-like class in Python that calls back into
    // dart_bridge._native_log_write. Keep the implementation in Python
    // (rather than building a PyType_Spec by hand) — Limited API safe and
    // far less code. The class is anonymous (lives in __main__).
    static const char* installer_src =
        "import sys, dart_bridge\n"
        "class _DartBridgeNativeWriter:\n"
        "    def __init__(self, is_stderr):\n"
        "        self._is_stderr = 1 if is_stderr else 0\n"
        "    def write(self, text):\n"
        "        if text:\n"
        "            dart_bridge._native_log_write(self._is_stderr, str(text))\n"
        "        return len(text) if text else 0\n"
        "    def flush(self):\n"
        "        pass\n"
        "    def isatty(self):\n"
        "        return False\n"
        "    def fileno(self):\n"
        "        raise OSError('no fileno on native log writer')\n"
        "sys.stdout = _DartBridgeNativeWriter(False)\n"
        "sys.stderr = _DartBridgeNativeWriter(True)\n";

    PyObject* code = Py_CompileString(installer_src, "<dart_bridge_stdio>",
                                      Py_file_input);
    if (!code) {
        PyErr_Print();
        PyGILState_Release(gstate);
        return -1;
    }
    PyObject* main_mod = PyImport_AddModule("__main__");  // borrowed
    if (!main_mod) {
        Py_DECREF(code);
        PyErr_Print();
        PyGILState_Release(gstate);
        return -1;
    }
    PyObject* globals = PyModule_GetDict(main_mod);  // borrowed
    PyObject* result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!result) {
        PyErr_Print();
        PyGILState_Release(gstate);
        return -1;
    }
    Py_DECREF(result);
    PyGILState_Release(gstate);
    return 0;
}
