# Building

This project has two independent build surfaces.

## 1. Host-testable logic (builds on Linux and macOS)

`src/common/` is freestanding C11 (no IOKit). It is built and unit-tested with
clang (or gcc) on this Linux host:

```sh
make unit            # crafted-buffer unit tests (ASan+UBSan)
make fixtures        # decompress /lib/firmware ath10k images into build/fixtures/
make fixture-probe   # parse the REAL firmware-6.bin / board-2.bin
make cxx-smoke       # prove the common C links through the kext's extern "C" edge
make test            # all of the above
./scripts/run_verification.sh   # full suite, saves outputs to artifacts/verification/
```

Toolchain on this host: `clang 21.1.8`, `make`, `zstd`. Override the compiler
with `make CC=gcc` (the sources are warning-clean under both).

## 2. The macOS kext (CANNOT be built here)

> **This Linux host has no Xcode and no macOS kernel SDK, so the kext in
> `kext/QCA6174Wifi/` CANNOT be compiled or loaded here. This is stated plainly
> rather than faked.** What *was* verified on Linux is that the shared
> `src/common` logic compiles and links through the same `extern "C"` boundary
> the kext uses (`make cxx-smoke`). The IOKit `.cpp` itself is unbuilt.

On a real macOS machine with Xcode installed:

1. Create a **Generic Kernel Extension** target (or a command-line `kextlibs`
   build). Add:
   - `kext/QCA6174Wifi/QCA6174Wifi.cpp`, `QCA6174Wifi.hpp`
   - `src/common/qca6174_regs.c`, `hw_params.c`, `fw_load_sm.c` (the kext links
     the same C objects the host tests use)
   - `kext/QCA6174Wifi/Info.plist` as the target Info.plist
2. Framework: link against `Kernel.framework`; `OSBundleLibraries` already lists
   `com.apple.kpi.{iokit,libkern,mach}` and `com.apple.iokit.IOPCIFamily`. Adjust
   the KPI versions to the minimum macOS you target.
3. Build (example skeleton — adapt paths/SDK):
   ```sh
   xcodebuild -target QCA6174Wifi -configuration Release
   # or clang++ with: -mkernel -fapple-kext -nostdlib \
   #   -isysroot $(xcrun --sdk macosx --show-sdk-path) \
   #   -I$(SDK)/System/Library/Frameworks/Kernel.framework/Headers
   ```
4. Build on macOS with the pinned MacKernelSDK:
   ```sh
   git clone https://github.com/acidanthera/MacKernelSDK.git
   git -C MacKernelSDK checkout 3f750085caa17ec3a7880f11c11bf4f48cd6a164
   ./scripts/build_kext_macos.sh
   ```
   GitHub Actions runs this on `macos-15-intel`; download a successful artifact
   with `gh run download <run-id> -n QCA6174Wifi-macos-unsigned`.
5. The product is `build/macos/QCA6174Wifi.kext`. It is unsigned; do not
   permanently install it before the one-session procedure in docs/INSTALL.md.

### Before building for real hardware
The register constants are pinned in docs/ATTRIBUTION.md. Keep direct raw
chip-id MMIO disabled until the ath10k HIF/BAR access path is independently
verified; host tests validate arithmetic, not live hardware semantics.
