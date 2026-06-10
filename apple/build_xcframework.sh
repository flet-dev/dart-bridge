#!/usr/bin/env bash
#
# Build dart_bridge.xcframework — a multi-slice static library for Apple
# platforms (iOS device, iOS simulator, macOS) consumed by serious_python_darwin.
#
# Requires:
#   - Xcode (xcrun, xcodebuild)
#   - $PYTHON_HEADERS_DIR set to a directory containing Python.h. Typically
#     extracted from python-ios-dart-${V}.tar.gz / python-macos-dart-${V}.tar.gz.
#   - The xcframework's static libs export PyInit_dart_bridge but the
#     consuming app (via serious_python_darwin) brings the actual Python
#     framework that resolves Py* symbols at app link time.
#
# Output: dist/dart_bridge.xcframework + dist/dart_bridge-apple.xcframework.zip

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

: "${PYTHON_HEADERS_DIR:?Set PYTHON_HEADERS_DIR to a dir containing Python.h}"

BUILD="$ROOT/build/apple"
DIST="$ROOT/dist"
rm -rf "$BUILD" "$DIST/dart_bridge.xcframework" "$DIST/dart_bridge-apple.xcframework.zip"
mkdir -p "$BUILD" "$DIST"

SRC_FILES=(
  "$ROOT/src/dart_bridge.c"
  "$ROOT/src/dart_api/dart_api_dl.c"
)

# Compile per-arch object files for each (sdk, arch) tuple, then libtool them
# into per-slice static archives. Slices match Apple's xcframework conventions:
#
#   iphoneos:        arm64                 (real iOS devices)
#   iphonesimulator: arm64 x86_64          (Apple Silicon + Intel sims)
#   macosx:          arm64 x86_64          (Apple Silicon + Intel macOS)

build_slice() {
  local sdk="$1"; shift
  local archs=("$@")
  local slice_objs=()

  echo "--- Building slice: $sdk (${archs[*]}) ---"

  local sdk_path
  sdk_path=$(xcrun --sdk "$sdk" --show-sdk-path)
  local min_flag=""
  case "$sdk" in
    iphoneos|iphonesimulator) min_flag="-mios-version-min=13.0" ;;
    macosx) min_flag="-mmacosx-version-min=10.15" ;;
  esac

  for arch in "${archs[@]}"; do
    for src in "${SRC_FILES[@]}"; do
      local name
      name=$(basename "$src" .c)
      local out="$BUILD/$sdk-$arch-$name.o"
      xcrun --sdk "$sdk" clang \
        -arch "$arch" \
        -isysroot "$sdk_path" \
        $min_flag \
        -DPy_LIMITED_API=0x030c0000 \
        -DDART_SHARED_LIB \
        -fvisibility=hidden \
        -I "$PYTHON_HEADERS_DIR" \
        -I "$ROOT/src" \
        -c "$src" -o "$out"
      slice_objs+=("$out")
    done
  done

  local slice_archive="$BUILD/libdart_bridge-$sdk.a"
  libtool -static -o "$slice_archive" "${slice_objs[@]}"
  echo "  -> $slice_archive"
}

build_slice iphoneos arm64
build_slice iphonesimulator arm64 x86_64
build_slice macosx arm64 x86_64

echo "--- Creating xcframework ---"
xcodebuild -create-xcframework \
  -library "$BUILD/libdart_bridge-iphoneos.a" \
  -library "$BUILD/libdart_bridge-iphonesimulator.a" \
  -library "$BUILD/libdart_bridge-macosx.a" \
  -output "$DIST/dart_bridge.xcframework"

echo "--- Zipping artifact ---"
(cd "$DIST" && zip -qr dart_bridge-apple.xcframework.zip dart_bridge.xcframework)

echo "Done: $DIST/dart_bridge-apple.xcframework.zip"
ls -lh "$DIST/dart_bridge-apple.xcframework.zip"
