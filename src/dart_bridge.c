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
#include "dart_api/dart_api_dl.h"

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
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
// are released before the interpreter goes away.
EXPORT void dart_bridge_clear_handlers(void) {
    handler_entry* e = g_handlers;
    g_handlers = NULL;
    while (e) {
        handler_entry* next = e->next;
        Py_XDECREF(e->handler);
        free(e);
        e = next;
    }
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

static PyMethodDef dart_bridge_methods[] = {
    {"send_bytes", py_send_bytes, METH_VARARGS,
     "send_bytes(port, payload) — post a bytes payload to the Dart\n"
     "ReceivePort identified by its native port."},
    {"set_enqueue_handler_func", py_set_enqueue_handler_func, METH_VARARGS,
     "set_enqueue_handler_func(port, callable) — register a Python\n"
     "callable that receives bytes posted from Dart for the given port.\n"
     "Pass None as the callable to unregister."},
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
