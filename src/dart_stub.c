// dart_stub.c

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

// Weak fallback so the linker doesn't complain
__declspec(dllexport)
bool Dart_PostCObject_DL(Dart_Port port_id, Dart_CObject* message) {
  (void)port_id;
  (void)message;
  return false;  // Runtime will overwrite this
}