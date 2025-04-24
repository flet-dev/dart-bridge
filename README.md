# dart_bridge

`dart_bridge` is a Python C extension that enables **bi-directional communication** between embedded Python and a Dart application (such as a Flutter app) via Dart's FFI (Foreign Function Interface) and ports.

It is designed for use cases where you want to **embed Python inside a Dart app**, send messages between Python and Dart, and run long-lived, message-driven Python scripts that cooperate with Dart logic.

---

## 🚀 Features

- ✅ Send messages (bytes) from Python to Dart  
- ✅ Receive messages from Dart into Python via FFI  
- ✅ Cross-thread safe using `PyGILState_Ensure`  
- ✅ Fully compatible with Dart's `dart_api_dl.h` via FFI  
- ✅ Portable across macOS, Linux, Windows, Android, and iOS  
- ✅ Compatible with Python 3.12+ and the `abi3` stable ABI  

---

## 💡 Usage Scenarios

- Embedding Python in Flutter for dynamic scripting, ML, or automation  
- Integrating Python data pipelines or AI models into mobile/desktop apps  
- Building hybrid apps where UI is Flutter but business logic is Python  
- Real-time event or message processing in an embedded Python runtime  

---

## 🔄 Communication Overview

```text
Dart → Python:
- Dart calls native function: `DartBridge_EnqueueMessage(...)`
- Python handler receives bytes via a registered Python function

Python → Dart:
- Python calls `dart_bridge.send_bytes(bytes)`
- Dart receives data via a `ReceivePort` attached to FFI `SendPort`
```

---

## Python API

### `dart_bridge.send_bytes(data: bytes) -> bool`

Sends bytes to Dart via a previously registered Dart port.  
Returns `True` if successful.  
Raises `RuntimeError` if the port is not set or `Dart_PostCObject_DL` fails.

---

### `dart_bridge.set_enqueue_handler_func(func: Callable[[bytes], None])`

Registers a Python function that will be called when Dart sends a message using FFI.  
The function should accept a single `bytes` argument.

This enables Dart → Python messaging.

---

## Dart FFI Requirements

On the Dart side, the following native functions are available via FFI:

```c
// Initialize Dart dynamic linking (must be called once)
intptr_t DartBridge_InitDartApiDL(void* data);

// Register a Dart SendPort for messages from Python
void DartBridge_SetSendPort(int64_t port);

// Send message from Dart into Python (calls the Python handler)
void DartBridge_EnqueueMessage(const char* data, size_t len);
```

These are typically loaded in Dart using `DynamicLibrary.lookupFunction`.

---

## Installation

Install from PyPI:

```bash
pip install dart_bridge
```

Or build from source with:

```bash
pip install .
```

Make sure to include `dart_api_dl.h` from the Dart SDK in your compiler's include path.

---

## Example Usage

### Dart side

```dart
final dylib = DynamicLibrary.open('dart_bridge.so');

final initApi = dylib.lookupFunction<
  IntPtr Function(Pointer<Void>),
  int Function(Pointer<Void>)
>('DartBridge_InitDartApiDL');

final setPort = dylib.lookupFunction<
  Void Function(Int64),
  void Function(int)
>('DartBridge_SetSendPort');

final enqueue = dylib.lookupFunction<
  Void Function(Pointer<Char>, IntPtr),
  void Function(Pointer<Char>, int)
>('DartBridge_EnqueueMessage');

initApi(NativeApi.initializeApiDLData);
setPort(sendPort.nativePort);

final msg = 'hello from Dart';
final ptr = msg.toNativeUtf8().cast<Char>();
enqueue(ptr, msg.length);
calloc.free(ptr);
```

### Python side

```python
import asyncio
import dart_bridge

queue = asyncio.Queue()

def enqueue_from_dart(data: bytes):
    print("Received:", data)
    queue.put_nowait(data)

dart_bridge.set_enqueue_handler_func(enqueue_from_dart)

async def main():
    while True:
        msg = await queue.get()
        dart_bridge.send_bytes(b"ECHO: " + msg)

asyncio.run(main())
```

---

## Thread Safety

- `DartBridge_EnqueueMessage` is safe to call from Dart threads or isolates.
- It uses `PyGILState_Ensure()` internally.
- Your Python handler will run in a background thread. Use `loop.call_soon_threadsafe()` if needed.

---

## License

MIT

---

## Contributing

Pull requests are welcome! This project helps bridge Dart and Python through portable, async-safe FFI communication.
