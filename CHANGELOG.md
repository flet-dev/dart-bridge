# Changelog

## 1.7.0

### Apple: the published xcframework is provider-signed

`dart_bridge.xcframework` is now code-signed with the Flet publishing team's
Apple Distribution identity, with a secure timestamp, before it is zipped.

Xcode records the state of every `.xcframework` an app links against **as its
publisher shipped it**, and writes that into the IPA as
`Signatures/dart_bridge.xcframework-ios.signature`. An unsigned xcframework makes
that receipt read `signed = false` / `isSecureTimestamp = false`, which Apple's
App Store scan reports as `ITMS-91065: Missing signature`. Signing the app does
not fill this in — Xcode re-signs the *embedded copy* with the submitting team's
identity, and the SDK-origin receipt is a separate record.

`apple/xcframework_signing.sh` holds the signing and verification helpers.
Signing happens after `xcodebuild -create-xcframework` and before the zip — the
last point at which the bundle is complete and unmutated — and the signature is
verified again after the zip is extracted into a fresh directory, so an archiving
bug shows up here rather than in a consumer's app.

Release builds run in an isolated `release-signing` CI job that imports the
certificate into a temporary keychain, derives exactly one identity fingerprint,
and deletes the keychain unconditionally. The general build matrix (every push and
PR) has no access to the certificate and still produces unsigned artifacts for
testing; those are skipped on tags and can no longer reach a release.

The outer seal is stamped with `-i dev.flet.dartbridge`, read off the inner
framework's own `CFBundleIdentifier`. An `.xcframework`'s root `Info.plist` is an
`XFWK` manifest with no `CFBundleIdentifier` of its own, so without this codesign
falls back to the bundle's file name and the seal reports a bare
`Identifier=dart_bridge`. Verification asserts the two agree, which also catches a
re-sign that dropped the flag.

`dev.flet.dartbridge` is otherwise unchanged — it was already a stable,
publisher-owned identifier, which is what lets one signature cover every app that
embeds it. The compiled binaries are identical to 1.6.1; only the packaging of the
published artifact changed.

## 1.6.1

### Apple: preserve framework symlinks in the published zip

`apple/build_xcframework.sh` now zips with `-y`, storing symlinks as symlinks.

Without it `zip` follows symlinks, so the macOS slice's versioned bundle —
`Versions/Current -> A`, `dart_bridge -> Versions/Current/dart_bridge`,
`Resources -> Versions/Current/Resources` — extracted as real files and
directories (and stored the dylib three times). The result is a malformed
framework: macOS builds failed with `Couldn't resolve framework symlink for
.../Versions/Current` and `code object is not signed at all` /
`Command CodeSign failed with a nonzero exit code`.

Only macOS was affected — the iOS slices use a flat framework layout with no
symlinks, which is why iOS builds against 1.6.0 succeeded. The compiled binaries
are identical to 1.6.0; only the packaging of the published artifact changed.

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
