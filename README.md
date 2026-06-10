# dart-bridge

In-process Dart ↔ Python byte transport — the C library powering
[`serious_python`](https://github.com/flet-dev/serious-python)'s Dart-to-Python
FFI bridge and (transitively) [Flet](https://flet.dev)'s socket-free transport
on `flet build` outputs.

This repository **publishes pre-built native binaries** consumed by the
`serious_python_*` Flutter plugins. End users don't typically interact with
this repo directly — they depend on `serious_python` (or `flet`) and get the
bridge transparently.

## What's in here

- `src/dart_bridge.c` — single C source containing both the Dart-callable
  surface (`DartBridge_InitDartApiDL`, `DartBridge_EnqueueMessage`) and the
  Python built-in module (`PyInit_dart_bridge`, `set_enqueue_handler_func`,
  `send_bytes`). Statically linked together — no dlsym/dlopen plumbing
  needed because both halves share globals trivially.
- `src/serious_python_run.c` — Python lifecycle: `Py_Initialize`,
  `PyRun_SimpleFile` / `PyRun_SimpleString`, env / sys.argv setup, worker-
  thread execution. Lifted from the platform-specific implementations in
  `serious_python_{darwin,linux,windows,android}`. Exposes
  `serious_python_run(config)` (sync or async via Dart port),
  `serious_python_register_extension(name, init)` (additional inittab
  entries beyond dart_bridge), `serious_python_request_stop()`,
  `serious_python_finalize()`.
- `src/dart_api/` — Dart SDK headers (vendored from the Dart SDK).

## Released binaries

Every tagged release attaches the following artifacts:

| Platform | Artifact |
|---|---|
| Linux x86_64 | `libdart_bridge-linux-x86_64.so` |
| Linux aarch64 | `libdart_bridge-linux-aarch64.so` |
| Windows x86_64 (Release CRT) | `dart_bridge-windows-x86_64.dll` |
| Windows x86_64 (Debug CRT) | `dart_bridge_d-windows-x86_64.dll` |
| Android arm64-v8a | `libdart_bridge-android-arm64-v8a.so` |
| Android armeabi-v7a | `libdart_bridge-android-armeabi-v7a.so` |
| Android x86_64 | `libdart_bridge-android-x86_64.so` |
| Apple (iOS device + iOS sim + macOS) | `dart_bridge-apple.xcframework.zip` |

abi3 (`Py_LIMITED_API=0x030c0000`) — one binary per `(platform × arch)` works
for any CPython 3.12+. No per-Python-version matrix needed.

Download URL pattern:

```
https://github.com/flet-dev/dart-bridge/releases/download/v<ver>/<artifact>
```

## Local build

```bash
# Linux (uses system Python headers)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Apple xcframework
./apple/build_xcframework.sh
```

For Windows / Android cross-builds, see `scripts/build_windows.ps1` and
`scripts/build_android.sh` — they handle the toolchain setup CI uses.

## Versioning

Plain semver, decoupled from CPython. The bridge is abi3-stable across all
3.12+ Pythons; one binary per (platform × arch) covers every supported
runtime. `serious_python` pins to a specific dart-bridge version via its
plugin build scripts.

## License

MIT — see [LICENSE](LICENSE).
