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
  surface (`DartBridge_InitDartApiDL`, `DartBridge_EnqueueMessage(port, ...)`)
  and the Python built-in module (`PyInit_dart_bridge`,
  `set_enqueue_handler_func(port, callable)`, `send_bytes(port, payload)`).
  Statically linked together — no dlsym/dlopen plumbing needed because both
  halves share the keyed handler list trivially. The 64-bit Dart native
  port doubles as the channel key in both directions, so multiple
  PythonBridge instances (UI channel + logging channel + ...) coexist
  without colliding.
- `src/serious_python_run.c` — Python lifecycle: `Py_Initialize`,
  `PyRun_SimpleFile` / `PyRun_SimpleString`, env / sys.argv setup, worker-
  thread execution. Lifted from the platform-specific implementations in
  `serious_python_{darwin,linux,windows,android}`. Exposes
  `serious_python_run(config)` (sync or async via Dart port),
  `serious_python_register_extension(name, init)` (additional inittab
  entries beyond dart_bridge), `serious_python_request_stop()`,
  `serious_python_finalize()`, and the multiprocessing child-interception
  pair described below.
- `src/dart_api/` — Dart SDK headers (vendored from the Dart SDK).

## Multiprocessing child interception (1.5.0+)

Python's `multiprocessing` module (`spawn`/`forkserver` start methods and the
resource tracker) launches helper processes by re-executing `sys.executable`
with CPython-generated command lines. In an app embedding Python through this
library, `sys.executable` points at the **host executable**. Without
interception, each worker re-launches the host app — typically a full GUI —
instead of running the multiprocessing helper protocol
([flet-dev/flet#4283](https://github.com/flet-dev/flet/issues/4283)).

`dart_bridge` exports entry points that let the host binary act as a plain
Python interpreter for exactly those helper invocations:

```c
int serious_python_is_mp_invocation(int argc, char** argv);
int serious_python_main(int argc, char** argv);           // delegates to Py_BytesMain

// Windows wide-char variants for wWinMain argv:
int serious_python_is_mp_invocation_w(int argc, wchar_t** argv);
int serious_python_main_w(int argc, wchar_t** argv);      // delegates to Py_Main
```

Host contract: call the detector as the **first thing** in `main`,
`wWinMain`, or `main.swift`, before any UI, COM, Flutter, GTK, AppKit, or
engine initialization:

```c
if (serious_python_is_mp_invocation(argc, argv)) {
    return serious_python_main(argc, argv);
}
```

`serious_python_is_mp_invocation` recognizes CPython multiprocessing helper
commands by matching `--multiprocessing-fork` anywhere in `argv`, or a `-c`
payload beginning with `from multiprocessing.` or
`import sys; from multiprocessing.`. The match is intentionally prefix-based:
these command lines are CPython implementation details and have changed across
minor releases, while the helpers remain under `multiprocessing.*`.

`serious_python_main` prepares the child process, scrubs `PYTHONINSPECT`,
registers the in-binary `dart_bridge` module with the inittab, and delegates
to the standard CPython command-line entry point. The child locates the
embedded stdlib/site-packages through the `PYTHONHOME`/`PYTHONPATH`
environment inherited from the parent process; `serious_python_run` sets those
process-wide before initializing Python.

On Windows, prefer the `_w` variants from `wWinMain` so Unicode argv values are
passed directly to `Py_Main` instead of being decoded through the ANSI code
page.

On Apple platforms, exported entry points are also marked `used`: the
`dart_bridge` archive is statically linked into the host app, and some public
symbols are referenced only through `dlsym`/Dart FFI. The marker prevents the
host linker's dead-strip pass from discarding exports that have no ordinary C
call site.

## Released binaries

Every tagged release attaches the following artifacts:

| Platform | Artifact |
|---|---|
| Linux x86_64 | `libdart_bridge-linux-x86_64.so` |
| Linux aarch64 | `libdart_bridge-linux-aarch64.so` |
| Windows x86_64 (Release CRT) | `dart_bridge-windows-x86_64.dll` |
| Windows x86_64 (Debug CRT) | `dart_bridge_d-windows-x86_64.dll` |
| Android arm64-v8a / armeabi-v7a / x86_64 | `libdart_bridge-android-<abi>-py<ver>.so` |
| Apple (iOS device + iOS sim + macOS) | `dart_bridge-apple.xcframework.zip` |

abi3 (`Py_LIMITED_API=0x030c0000`) makes one binary work for any CPython
3.12+ on Linux/Windows/Apple. Android is the exception: python-build-
standalone ships `libpython3.so` as a GNU linker script that resolves to
`libpython3.<ver>.so`, so the resulting `DT_NEEDED` entry is version-
specific and we publish a binary per `(abi × python_version)`.

armeabi-v7a (32-bit ARM) is published for every supported Python minor
(3.12, 3.13, 3.14), matching python-build's per-minor `android_abis`.

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
