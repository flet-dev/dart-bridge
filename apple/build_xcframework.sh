#!/usr/bin/env bash
#
# Build dart_bridge.xcframework — a multi-slice DYNAMIC framework for Apple
# platforms (iOS device, iOS simulator, macOS) consumed by serious_python_darwin.
#
# Why dynamic (a .framework with a dylib) and not a static library:
#   The FFI entry points (serious_python_run, DartBridge_InitDartApiDL,
#   DartBridge_EnqueueMessage, PyInit_dart_bridge) are resolved at runtime via
#   dlsym — Dart's `DynamicLibrary.process()` and Python's `import dart_bridge`.
#   A STATIC library linked into the host app executable does NOT export its
#   symbols (an iOS executable exports nothing by default, and the release build
#   strips local symbols), so dlsym(RTLD_DEFAULT) cannot find them and the app
#   crashes at startup with "Failed to lookup symbol 'serious_python_run'". A
#   dynamic framework exports its default-visibility symbols and is loaded as its
#   own image, so dlsym resolves them — exactly like the Android `.so` build
#   (CMake `add_library(... SHARED)`), which is why Android already works.
#
# Requires:
#   - Xcode (xcrun, xcodebuild)
#   - $PYTHON_HEADERS_DIR set to a directory containing Python.h.
#   - Py_* and Dart_* symbols are left undefined (`-undefined dynamic_lookup`)
#     and resolved at runtime from the app's embedded Python.framework and the
#     Flutter/Dart runtime — the same deferral the static build relied on at app
#     link time, now deferred to load time instead.
#
# Output: dist/dart_bridge.xcframework + dist/dart_bridge-apple.xcframework.zip
#
# Release builds additionally provider-sign the outer xcframework; see
# apple/xcframework_signing.sh for the environment it reads. Builds without those
# credentials still succeed and produce an unsigned (non-publishable) artifact.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

: "${PYTHON_HEADERS_DIR:?Set PYTHON_HEADERS_DIR to a dir containing Python.h}"

# shellcheck source=apple/xcframework_signing.sh
. "$ROOT/apple/xcframework_signing.sh"

# Validate signing credentials before compiling: a release run that lost its
# certificate should fail in seconds, not after three slice builds.
preflight_rc=0
xcf_signing_preflight || preflight_rc=$?
[ "$preflight_rc" -le 1 ] || exit 1

BUILD="$ROOT/build/apple"
DIST="$ROOT/dist"
rm -rf "$BUILD" "$DIST/dart_bridge.xcframework" "$DIST/dart_bridge-apple.xcframework.zip"
mkdir -p "$BUILD" "$DIST"

SRC_FILES=(
  "$ROOT/src/dart_bridge.c"
  "$ROOT/src/serious_python_run.c"
  "$ROOT/src/dart_api/dart_api_dl.c"
)

FW_NAME="dart_bridge"
BUNDLE_ID="dev.flet.dartbridge"
FW_VERSION="1.0"

# Emit a minimal framework Info.plist. $1 = dest file, $2 = platform name
# (iPhoneOS / iPhoneSimulator / MacOSX), $3 = min OS version.
write_info_plist() {
  local dest="$1" platform="$2" minos="$3"
  cat > "$dest" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>${FW_NAME}</string>
  <key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>${FW_NAME}</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>${FW_VERSION}</string>
  <key>CFBundleVersion</key><string>${FW_VERSION}</string>
  <key>CFBundleSupportedPlatforms</key><array><string>${platform}</string></array>
  <key>MinimumOSVersion</key><string>${minos}</string>
</dict>
</plist>
PLIST
}

# Build a dynamic .framework for one slice.
#   $1 = sdk (iphoneos|iphonesimulator|macosx)
#   $2 = platform name for Info.plist
#   $3 = min OS version
#   $4 = "flat" (iOS) or "versioned" (macOS) framework layout
#   $5.. = archs
build_slice() {
  local sdk="$1"; shift
  local platform="$1"; shift
  local minos="$1"; shift
  local layout="$1"; shift
  local archs=("$@")

  echo "--- Building slice: $sdk (${archs[*]}) ---"

  local sdk_path
  sdk_path=$(xcrun --sdk "$sdk" --show-sdk-path)
  local min_flag=""
  case "$sdk" in
    iphoneos)        min_flag="-mios-version-min=13.0" ;;
    iphonesimulator) min_flag="-mios-simulator-version-min=13.0" ;;
    macosx)          min_flag="-mmacosx-version-min=10.15" ;;
  esac

  local arch_flags=()
  local a
  for a in "${archs[@]}"; do arch_flags+=(-arch "$a"); done

  # Compile each source once as a fat (multi-arch) object.
  local objs=()
  local src name obj
  for src in "${SRC_FILES[@]}"; do
    name=$(basename "$src" .c)
    obj="$BUILD/$sdk-$name.o"
    xcrun --sdk "$sdk" clang \
      "${arch_flags[@]}" \
      -isysroot "$sdk_path" \
      $min_flag \
      -DPy_LIMITED_API=0x030c0000 \
      -DDART_SHARED_LIB \
      -fvisibility=hidden \
      -I "$PYTHON_HEADERS_DIR" \
      -I "$ROOT/src" \
      -c "$src" -o "$obj"
    objs+=("$obj")
  done

  # Link a fat dynamic library. Py_*/Dart_* stay undefined and are bound at
  # load time from the app's Python.framework and the Dart runtime.
  local dylib="$BUILD/$sdk-$FW_NAME.dylib"
  xcrun --sdk "$sdk" clang \
    -dynamiclib \
    "${arch_flags[@]}" \
    -isysroot "$sdk_path" \
    $min_flag \
    -install_name "@rpath/${FW_NAME}.framework/${FW_NAME}" \
    -Wl,-undefined,dynamic_lookup \
    -o "$dylib" "${objs[@]}"

  # Assemble the .framework bundle.
  local fw="$BUILD/$sdk/${FW_NAME}.framework"
  rm -rf "$fw"
  if [ "$layout" = "versioned" ]; then
    mkdir -p "$fw/Versions/A/Resources"
    cp "$dylib" "$fw/Versions/A/${FW_NAME}"
    write_info_plist "$fw/Versions/A/Resources/Info.plist" "$platform" "$minos"
    ln -sfn A "$fw/Versions/Current"
    ln -sfn "Versions/Current/${FW_NAME}" "$fw/${FW_NAME}"
    ln -sfn "Versions/Current/Resources" "$fw/Resources"
  else
    mkdir -p "$fw"
    cp "$dylib" "$fw/${FW_NAME}"
    write_info_plist "$fw/Info.plist" "$platform" "$minos"
  fi
  echo "  -> $fw"
  lipo -info "$dylib" 2>/dev/null || true
}

build_slice iphoneos        iPhoneOS        13.0  flat      arm64
build_slice iphonesimulator iPhoneSimulator 13.0  flat      arm64 x86_64
build_slice macosx          MacOSX          10.15 versioned arm64 x86_64

echo "--- Creating xcframework ---"
xcodebuild -create-xcframework \
  -framework "$BUILD/iphoneos/${FW_NAME}.framework" \
  -framework "$BUILD/iphonesimulator/${FW_NAME}.framework" \
  -framework "$BUILD/macosx/${FW_NAME}.framework" \
  -output "$DIST/${FW_NAME}.xcframework"

# --- Provider signature ---------------------------------------------------
# Last mutation of the bundle. `xcodebuild -create-xcframework` is the final
# step that writes into it, and the zip below only reads — so this is the one
# point where the artifact is both complete and still unsigned. Anything added
# between here and the zip would break the seal.
#
# dart_bridge already carries the stable provider identifier dev.flet.dartbridge
# (BUNDLE_ID above); it is never rewritten per consuming application, which is
# what lets one signature cover every app that embeds it.
echo "--- Signing xcframework ---"
xcf_sign_tree "$DIST/${FW_NAME}.xcframework"

echo "--- Zipping artifact ---"
# -y stores symlinks AS symlinks. Without it zip follows them, and the macOS
# slice's versioned bundle (`Versions/Current -> A`, `dart_bridge ->
# Versions/Current/dart_bridge`, `Resources -> Versions/Current/Resources`)
# extracts as real files/directories — a malformed framework that macOS codesign
# rejects ("Couldn't resolve framework symlink for .../Versions/Current",
# "code object is not signed at all"), and that stores the dylib three times.
# iOS uses a flat layout with no symlinks, so only macOS was affected.
(cd "$DIST" && zip -qry "${FW_NAME}-apple.xcframework.zip" "${FW_NAME}.xcframework")

# --- Round-trip verification ----------------------------------------------
# The published artifact is the ZIP, not the directory we just signed, so verify
# what consumers actually get. This is the check that would have caught the
# symlink-flattening bug fixed in 1.6.1 as a signature failure rather than as a
# codesign error in someone else's app.
echo "--- Verifying signature after archive round trip ---"
roundtrip_dir=$(mktemp -d)
trap 'rm -rf "$roundtrip_dir"' EXIT
unzip -q "$DIST/${FW_NAME}-apple.xcframework.zip" -d "$roundtrip_dir"
xcf_verify_tree "$roundtrip_dir/${FW_NAME}.xcframework"

echo "Done: $DIST/${FW_NAME}-apple.xcframework.zip"
ls -lh "$DIST/${FW_NAME}-apple.xcframework.zip"
