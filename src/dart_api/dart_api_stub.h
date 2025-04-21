#ifndef DART_API_STUB_H
#define DART_API_STUB_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t Dart_Port;

typedef enum {
  Dart_CObject_kNull = 0,
  Dart_CObject_kBool = 1,
  Dart_CObject_kInt32 = 2,
  Dart_CObject_kInt64 = 3,
  Dart_CObject_kDouble = 4,
  Dart_CObject_kString = 5,
  Dart_CObject_kArray = 6,
  Dart_CObject_kTypedData = 7,
  Dart_CObject_kExternalTypedData = 8,
} Dart_CObject_Type;

typedef enum {
  Dart_TypedData_kByteData = 0,
  Dart_TypedData_kInt8 = 1,
  Dart_TypedData_kUint8 = 2,
  Dart_TypedData_kInt16 = 3,
  Dart_TypedData_kUint16 = 4,
  Dart_TypedData_kInt32 = 5,
  Dart_TypedData_kUint32 = 6,
  Dart_TypedData_kInt64 = 7,
  Dart_TypedData_kUint64 = 8,
  Dart_TypedData_kFloat32 = 9,
  Dart_TypedData_kFloat64 = 10,
  Dart_TypedData_kUint8Clamped = 11,
} Dart_TypedData_Type;

typedef struct {
  Dart_TypedData_Type type;
  int32_t length;
  void* values;
} Dart_CObject__TypedData;

typedef union {
  Dart_CObject__TypedData typed_data;
} Dart_CObject_Value;

typedef struct {
  Dart_CObject_Type type;
  Dart_CObject_Value value;
} Dart_CObject;

// Declared for your Cython module to use
bool Dart_PostCObject_DL(Dart_Port port_id, Dart_CObject* message);

// Declared so Dart can call it at startup (you do NOT call this from Cython!)
intptr_t Dart_InitializeApiDL(void* data);

#ifdef __cplusplus
}
#endif

#endif  // DART_API_STUB_H