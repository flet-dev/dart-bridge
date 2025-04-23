#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include "dart_api/dart_api_dl.h"

PyObject* global_enqueue_handler_func = NULL;

static PyObject* set_enqueue_handler_func(PyObject* self, PyObject* args) {
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

void DartBridge_EnqueueMessage(const char* data, size_t len) {
    PyGILState_STATE gstate = PyGILState_Ensure();

    if (!global_enqueue_handler_func) {
        fprintf(stderr, "[dart_bridge.c] global_enqueue_handler_func is NULL\n");
        PyGILState_Release(gstate);
        return;
    }

    PyObject* arg = PyBytes_FromStringAndSize(data, len);
    if (!arg) {
        PyErr_Print();
        fprintf(stderr, "[dart_bridge.c] Failed to create PyBytes\n");
        PyGILState_Release(gstate);
        return;
    }

    PyObject* result = PyObject_CallFunctionObjArgs(global_enqueue_handler_func, arg, NULL);
    if (!result) {
        PyErr_Print();
        fprintf(stderr, "[dart_bridge.c] global_enqueue_handler_func call failed\n");
    }

    Py_XDECREF(arg);
    Py_XDECREF(result);
    PyGILState_Release(gstate);
}

static Dart_Port dart_port = 0;

// called from Dart via FFI
intptr_t DartBridge_InitDartApiDL(void* data) {
  return Dart_InitializeApiDL(data);
}

// called from Dart via FFI
void DartBridge_SetSendPort(int64_t port) {
  dart_port = port;
}

// called from Python
static PyObject* send_bytes(PyObject* self, PyObject* args) {
  const char* buffer;
  Py_ssize_t length;

  if (!PyArg_ParseTuple(args, "y#", &buffer, &length)) {
    return NULL;
  }

  if (dart_port == 0) {
    PyErr_SetString(PyExc_RuntimeError, "Dart port not set");
    return NULL;
  }

  Dart_CObject obj;
  obj.type = Dart_CObject_kTypedData;
  obj.value.as_typed_data.type = Dart_TypedData_kUint8;
  obj.value.as_typed_data.length = (int32_t)length;
  obj.value.as_typed_data.values = (void*)buffer;

  bool ok = Dart_PostCObject_DL(dart_port, &obj);
  if (!ok) {
    PyErr_SetString(PyExc_RuntimeError, "Dart_PostCObject_DL failed");
    return NULL;
  }

  Py_RETURN_TRUE;
}

static PyMethodDef methods[] = {
  {"send_bytes", send_bytes, METH_VARARGS, "Send bytes to Dart"},
  {"set_enqueue_handler_func", set_enqueue_handler_func, METH_VARARGS, "Set the Python handler for C callbacks."},
  {NULL, NULL, 0, NULL}
};

static struct PyModuleDef moduledef = {
  PyModuleDef_HEAD_INIT,
  "dart_bridge", NULL, -1, methods
};

PyMODINIT_FUNC PyInit_dart_bridge(void) {
  return PyModule_Create(&moduledef);
}