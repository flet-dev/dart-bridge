# Changelog

## 1.6.0

### Apple: `dart_bridge.xcframework` is now a dynamic framework

`apple/build_xcframework.sh` now produces `dart_bridge.xcframework` as a **dynamic
framework** (a `.framework` with a dylib) instead of a static library.

**Why.** The FFI entry points (`serious_python_run`, `DartBridge_InitDartApiDL`,
`DartBridge_EnqueueMessage`, `PyInit_dart_bridge`) are resolved at runtime via
`dlsym` — Dart's `DynamicLibrary.process()` and Python's `import dart_bridge`. A
static library linked into the host app **executable** does not export its symbols
(an iOS executable exports nothing by default, and the release build strips local
symbols), so `dlsym(RTLD_DEFAULT)` could not find them and release/device builds
crashed at startup with `Failed to lookup symbol 'serious_python_run'`. A dynamic
framework exports its default-visibility symbols and is loaded as its own image,
so `dlsym` resolves them — matching the Android `.so` (CMake `add_library(...
SHARED)`) that already worked. Only release/archive (device) builds were affected;
debug/simulator builds don't dead-strip and appeared to work.

**Consumer impact.** `serious_python_darwin` embeds + signs the framework (as it
already does for `Python.xcframework`) instead of static-linking it — no
`-all_load`/`-force_load` retention or dead-strip keep-alive is needed anymore.
`Py_*`/`Dart_*` symbols are left undefined (`-undefined dynamic_lookup`) and bound
at load time from the app's embedded `Python.framework` and the Dart runtime.

The Linux, Windows, and Android builds are unchanged (already dynamic via
CMake `SHARED`).
