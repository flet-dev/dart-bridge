// dart_bridge — in-process Dart ↔ Python byte transport.
//
// Compiled into a single libdart_bridge binary (.so / .dll / static lib in
// xcframework) consumed by serious_python's platform plugins. Two C-callable
// surfaces live here:
//
//   1. Dart-callable: DartBridge_InitDartApiDL, DartBridge_EnqueueMessage,
//      dart_bridge_post_to_dart. Looked up by Dart via FFI.
//
//   2. Python-callable: a built-in module `dart_bridge` registered via
//      PyImport_AppendInittab (see serious_python_run.c) before Py_Initialize.
//      Exposes two methods to Python code:
//        - set_enqueue_handler_func(callable)
//        - send_bytes(port, payload)
//
// Both halves share a single global cell `dart_bridge_global_enqueue_handler_func`
// holding the Python callable that DartBridge_EnqueueMessage hands incoming
// bytes to. Static linking inside one binary keeps Dart's view and Python's
// view of that cell trivially identical.

#define PY_SSIZE_T_CLEAN
// Limited API / abi3: one compiled binary works across all Python 3.12+
// minor versions. Every Py* symbol below is in the Limited API since 3.2
// (3.4 for PyGILState_*).
#define Py_LIMITED_API 0x030c0000
#include <Python.h>
#include <stdint.h>
#include <stdio.h>
#include "dart_api/dart_api_dl.h"

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

// Initialised to NULL; set_enqueue_handler_func swaps in a PyObject* callable
// when Python registers a handler. DartBridge_EnqueueMessage invokes it.
static PyObject* global_enqueue_handler_func = NULL;

// ---------------------------------------------------------------------------
// Dart-callable
// ---------------------------------------------------------------------------

EXPORT intptr_t DartBridge_InitDartApiDL(void* data) {
    return Dart_InitializeApiDL(data);
}

EXPORT void DartBridge_EnqueueMessage(const char* data, size_t len) {
    // Drop messages sent before Python has finished Py_Initialize. Acquiring
    // the GIL against an uninitialized interpreter triggers a fatal
    // PyMUTEX_LOCK failure (the gil->mutex is uninitialized). Dart's retry
    // loop will resend until Python is up.
    if (!Py_IsInitialized()) {
        return;
    }

    PyGILState_STATE gstate = PyGILState_Ensure();

    if (!global_enqueue_handler_func) {
        fprintf(stderr, "[dart_bridge] enqueue handler is not registered\n");
        PyGILState_Release(gstate);
        return;
    }

    PyObject* arg = PyBytes_FromStringAndSize(data, len);
    if (!arg) {
        PyErr_Print();
        PyGILState_Release(gstate);
        return;
    }

    PyObject* result = PyObject_CallFunctionObjArgs(
        global_enqueue_handler_func, arg, NULL);
    if (!result) {
        PyErr_Print();
    }

    Py_XDECREF(arg);
    Py_XDECREF(result);
    PyGILState_Release(gstate);
}

// ---------------------------------------------------------------------------
// Python-callable (built-in module via PyImport_AppendInittab)
// ---------------------------------------------------------------------------

static PyObject* py_set_enqueue_handler_func(PyObject* self, PyObject* args) {
    PyObject* func;

    if (!PyArg_ParseTuple(args, "O:set_enqueue_handler_func", &func)) {
        return NULL;
    }
    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "parameter must be callable");
        return NULL;
    }

    Py_XINCREF(func);
    Py_XDECREF(global_enqueue_handler_func);
    global_enqueue_handler_func = func;

    Py_RETURN_NONE;
}

static PyObject* py_send_bytes(PyObject* self, PyObject* args) {
    int64_t port;
    const char* buffer;
    Py_ssize_t length;

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
     "Post a bytes payload to a Dart ReceivePort identified by its native port."},
    {"set_enqueue_handler_func", py_set_enqueue_handler_func, METH_VARARGS,
     "Register the Python callable that receives bytes posted from Dart."},
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
