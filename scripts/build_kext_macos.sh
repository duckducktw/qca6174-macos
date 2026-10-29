#!/usr/bin/env bash
# Build an unsigned x86_64 legacy kext on macOS with Xcode + MacKernelSDK.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_ROOT="${MAC_KERNEL_SDK:-$ROOT/MacKernelSDK}"
OUT="${1:-$ROOT/build/macos}"
OBJ="$OUT/obj"
BUNDLE="$OUT/QCA6174Wifi.kext"
BIN="$BUNDLE/Contents/MacOS/QCA6174Wifi"

if [[ "$(uname -s)" != Darwin ]]; then
  echo "error: this script requires macOS/Xcode" >&2
  exit 2
fi
if [[ ! -f "$SDK_ROOT/Library/x86_64/libkmod.a" ]]; then
  echo "error: MacKernelSDK missing at $SDK_ROOT" >&2
  exit 2
fi

SDK="$(xcrun --sdk macosx --show-sdk-path)"
CLANG="$(xcrun --find clang)"
CLANGXX="$(xcrun --find clang++)"
rm -rf "$OUT"
mkdir -p "$OBJ" "$BUNDLE/Contents/MacOS"

COMMON=(
  -arch x86_64
  -isysroot "$SDK"
  -mmacosx-version-min=11.0
  -mkernel
  -fno-builtin
  -fno-common
  -fno-stack-protector
  -fno-pic
  -fno-exceptions
  -fno-rtti
  -Wall -Wextra -Werror
  -I"$ROOT/build-support"
  -I"$SDK_ROOT/Headers"
  -I"$ROOT/src/common"
  -I"$ROOT/kext/QCA6174Wifi"
)

"$CLANGXX" "${COMMON[@]}" -std=gnu++14 -fapple-kext -nostdinc++ \
  -c "$ROOT/kext/QCA6174Wifi/QCA6174Wifi.cpp" -o "$OBJ/QCA6174Wifi.o"

for src in qca6174_regs hw_params fw_load_sm; do
  "$CLANG" "${COMMON[@]}" -std=gnu11 \
    -c "$ROOT/src/common/$src.c" -o "$OBJ/$src.o"
done

"$CLANGXX" -arch x86_64 -isysroot "$SDK" -mmacosx-version-min=11.0 \
  -nostdlib -Wl,-kext -Wl,-fatal_warnings \
  -L"$SDK_ROOT/Library/x86_64" \
  "$OBJ"/*.o -lkmod -o "$BIN"

cp "$ROOT/kext/QCA6174Wifi/Info.plist" "$BUNDLE/Contents/Info.plist"
chmod -R u+rwX,go+rX "$BUNDLE"

file "$BIN"
plutil -lint "$BUNDLE/Contents/Info.plist"
otool -hv "$BIN"
shasum -a 256 "$BIN"
echo "built: $BUNDLE"
