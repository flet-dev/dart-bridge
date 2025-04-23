# cython: language_level=3

from libc.stdint cimport int64_t
from libc.stdint cimport intptr_t

from cpython.bytes cimport PyBytes_AsStringAndSize

cdef extern from "dart_api_stub.h":

    ctypedef int64_t Dart_Port

    cdef enum Dart_CObject_Type:
        Dart_CObject_kTypedData

    cdef enum Dart_TypedData_Type:
        Dart_TypedData_kUint8

    ctypedef struct Dart_CObject__TypedData:
        Dart_TypedData_Type type
        int length
        void* values

    ctypedef union Dart_CObject_Value:
        Dart_CObject__TypedData typed_data

    ctypedef struct Dart_CObject:
        Dart_CObject_Type type
        Dart_CObject_Value value

    bint Dart_PostCObject_DL(Dart_Port port_id, Dart_CObject* message)

cdef extern from "dart_api_dl.h":
    intptr_t Dart_InitializeApiDL(void* data)

cdef public long InitDartApiDL(void* data):
    return Dart_InitializeApiDL(data)

# Global variable to store Dart port
cdef Dart_Port _dart_port = 0

cdef public void set_dart_send_port(int64_t port_id):
    global _dart_port
    _dart_port = port_id

def send_bytes_to_dart(bytes data):
    cdef Dart_CObject obj
    cdef char* raw
    cdef Py_ssize_t length

    if _dart_port == 0:
        raise ValueError("Dart port is not set")

    if PyBytes_AsStringAndSize(data, &raw, &length) < 0:
        raise ValueError("Failed to extract byte data")

    obj.type = Dart_CObject_kTypedData
    obj.value.typed_data.type = Dart_TypedData_kUint8
    obj.value.typed_data.length = <int>length
    obj.value.typed_data.values = <void*>raw

    if not Dart_PostCObject_DL(_dart_port, &obj):
        raise RuntimeError("Failed to post Dart message")