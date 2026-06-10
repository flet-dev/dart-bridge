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
  "$ROOT/src/serious_python_run.c"
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

  echo "--- Building slice: $sdk (${archs[*]}) ---"

  local sdk_path
  sdk_path=$(xcrun --sdk "$sdk" --show-sdk-path)
  local min_flag=""
  case "$sdk" in
    iphoneos)        min_flag="-mios-version-min=13.0" ;;
    iphonesimulator) min_flag="-mios-simulator-version-min=13.0" ;;
    macosx)          min_flag="-mmacosx-version-min=10.15" ;;
  esac

  # For each arch, produce a per-arch static archive. xcframework slices
  # must be either single-arch archives OR a single fat (lipo'd) archive
  # per slice. libtool-static of multi-arch .o files in one archive
  # produces a multi-platform archive that xcodebuild -create-xcframework
  # rejects ("binaries with multiple platforms are not supported"), so we
  # libtool per-arch and then lipo the per-arch archives into one fat .a.

  local arch_archives=()
  for arch in "${archs[@]}"; do
    local arch_objs=()
    for src in "${SRC_FILES[@]}"; do
      local name
      name=$(basename "$src" .c)
      local obj="$BUILD/$sdk-$arch-$name.o"
      xcrun --sdk "$sdk" clang \
        -arch "$arch" \
        -isysroot "$sdk_path" \
        $min_flag \
        -DPy_LIMITED_API=0x030c0000 \
        -DDART_SHARED_LIB \
        -fvisibility=hidden \
        -I "$PYTHON_HEADERS_DIR" \
        -I "$ROOT/src" \
        -c "$src" -o "$obj"
      arch_objs+=("$obj")
    done
    local arch_archive="$BUILD/libdart_bridge-$sdk-$arch.a"
    libtool -static -o "$arch_archive" "${arch_objs[@]}"
    arch_archives+=("$arch_archive")
  done

  # CocoaPods rejects xcframeworks whose slices have differing library
  # binary names ("contains static libraries with differing binary names").
  # Stage each slice under its own subdir so xcodebuild -create-xcframework
  # preserves a uniform `libdart_bridge.a` in every slice.
  local slice_dir="$BUILD/$sdk"
  mkdir -p "$slice_dir"
  local slice_archive="$slice_dir/libdart_bridge.a"
  if [ "${#arch_archives[@]}" -eq 1 ]; then
    cp "${arch_archives[0]}" "$slice_archive"
  else
    lipo -create -output "$slice_archive" "${arch_archives[@]}"
  fi
  echo "  -> $slice_archive"
  lipo -info "$slice_archive" 2>/dev/null || true
}

build_slice iphoneos arm64
build_slice iphonesimulator arm64 x86_64
build_slice macosx arm64 x86_64

echo "--- Creating xcframework ---"
xcodebuild -create-xcframework \
  -library "$BUILD/iphoneos/libdart_bridge.a" \
  -library "$BUILD/iphonesimulator/libdart_bridge.a" \
  -library "$BUILD/macosx/libdart_bridge.a" \
  -output "$DIST/dart_bridge.xcframework"

echo "--- Zipping artifact ---"
(cd "$DIST" && zip -qr dart_bridge-apple.xcframework.zip dart_bridge.xcframework)

echo "Done: $DIST/dart_bridge-apple.xcframework.zip"
ls -lh "$DIST/dart_bridge-apple.xcframework.zip"
