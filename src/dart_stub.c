// src/dart_stub.c

#include <stdint.h>
#include <stdbool.h>

typedef int64_t Dart_Port;

typedef struct {
  int type;
  union {
    struct {
      int type;
      int length;
      void* values;
    } typed_data;
  } value;
} Dart_CObject;

// Cross-platform export declaration
#if defined(_WIN32) || defined(_WIN64)
  #define EXPORT __declspec(dllexport)
#else
  #define EXPORT __attribute__((visibility("default")))
#endif

EXPORT bool Dart_PostCObject_DL(Dart_Port port_id, Dart_CObject* message) {
  (void)port_id;
  (void)message;
  return false;  // runtime will override this
}