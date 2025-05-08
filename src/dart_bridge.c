#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include "dart_api/dart_api_dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define EXPORT __declspec(dllexport)
#define SET_ENV(key, value) _putenv_s(key, value)
#else
#include <pthread.h>
#include <unistd.h>
#define EXPORT __attribute__((visibility("default")))
#define SET_ENV(key, value) setenv(key, value, 1)
#endif

typedef struct {
    const char* appPath;
    const char* script;
    const char** modulePaths;
    int modulePathsCount;
    const char** envKeys;
    const char** envValues;
    int envCount;
    int sync;  // 1 = sync, 0 = async (threaded)
} DartBridgeRunArgs;

void run_python(DartBridgeRunArgs* args) {
    // Set environment variables
    for (int i = 0; i < args->envCount; i++) {
        SET_ENV(args->envKeys[i], args->envValues[i]);
    }

    // Build PYTHONPATH
    if (args->modulePathsCount > 0) {
        size_t total_len = 0;
        const char sep =
#if defined(_WIN32)
            ';';
#else
            ':';
#endif
        for (int i = 0; i < args->modulePathsCount; i++) {
            total_len += strlen(args->modulePaths[i]) + 1;
        }
        char* pythonpath = malloc(total_len);
        pythonpath[0] = '\0';
        for (int i = 0; i < args->modulePathsCount; i++) {
            strcat(pythonpath, args->modulePaths[i]);
            if (i < args->modulePathsCount - 1) {
                size_t len = strlen(pythonpath);
                pythonpath[len] = sep;
                pythonpath[len + 1] = '\0';
            }
        }
        SET_ENV("PYTHONPATH", pythonpath);
        free(pythonpath);
    }

    Py_Initialize();

    if (args->script && strlen(args->script) > 0) {
        PyRun_SimpleString(args->script);
    } else if (args->appPath) {
        FILE* file = fopen(args->appPath, "rb");
        if (file) {
            PyRun_SimpleFileEx(file, args->appPath, 1);  // 1 = close file
        } else {
            fprintf(stderr, "Failed to open Python file: %s\n", args->appPath);
        }
    }

    if (args->sync) {
        Py_Finalize();
    }
}

#if defined(_WIN32)
#include <process.h>
unsigned __stdcall python_thread_main(void* arg) {
    run_python((DartBridgeRunArgs*)arg);
    return 0;
}
#else
void* python_thread_main(void* arg) {
    run_python((DartBridgeRunArgs*)arg);
    return NULL;
}
#endif

// called from Dart via FFI
#ifdef _WIN32
__declspec(dllexport)
#endif
EXPORT void DartBridge_RunPython(DartBridgeRunArgs* args) {
    if (args->sync) {
        run_python(args);
    } else {
#if defined(_WIN32)
        _beginthreadex(NULL, 0, python_thread_main, args, 0, NULL);
#else
        pthread_t thread;
        pthread_create(&thread, NULL, python_thread_main, args);
        pthread_detach(thread);
#endif
    }
}

PyObject* global_enqueue_handler_func = NULL;

// called from Python
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

// called from Dart via FFI
#ifdef _WIN32
__declspec(dllexport)
#endif
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

// called from Dart via FFI
#ifdef _WIN32
__declspec(dllexport)
#endif
intptr_t DartBridge_InitDartApiDL(void* data) {
  return Dart_InitializeApiDL(data);
}

// called from Python
static PyObject* send_bytes(PyObject* self, PyObject* args) {
  int64_t port;
  const char* buffer;
  Py_ssize_t length;

  // Expecting a tuple: (port, bytes)
  if (!PyArg_ParseTuple(args, "Ly#", &port, &buffer, &length)) {
    return NULL;
  }

  if (port == 0) {
    PyErr_SetString(PyExc_RuntimeError, "Dart port is 0 (invalid)");
    return NULL;
  }

  Dart_CObject obj;
  obj.type = Dart_CObject_kTypedData;
  obj.value.as_typed_data.type = Dart_TypedData_kUint8;
  obj.value.as_typed_data.length = (int32_t)length;
  obj.value.as_typed_data.values = (void*)buffer;

  bool ok = Dart_PostCObject_DL(port, &obj);
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