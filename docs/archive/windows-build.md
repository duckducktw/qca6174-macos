# Build the native Sequoia kext in Windows

Tested locally: Windows x86_64, Python 3.13, Zig 0.16.0 / Clang 21.1,
Apple ld64 253.9 in Cygwin, and rcodesign 0.29.0. No administrator, macOS,
WSL, VM, private credentials or remote build service is required.

## Toolchain

Obtain [Zig 0.16.0](https://ziglang.org/download/0.16.0/) for Windows and
[rcodesign 0.29.0](https://github.com/indygreg/apple-platform-rs/releases/tag/apple-codesign%2F0.29.0)
for `x86_64-pc-windows-msvc`. Verify their published checksums before use.
The repository already contains MacKernelSDK headers and the kmod archives.

LLD's Mach-O linker does not implement the required `-kext` output mode. Build
the Cygwin-compatible Apple ld64 port instead. From this repository in PowerShell:

```powershell
python -m pip install --target .cache/windows-kext/python zstandard==0.25.0
python scripts/windows/prepare-cygwin.py
git clone -c core.autocrlf=false --depth 1 --branch cctools-877.8-ld64-253.9-1 https://github.com/tpoechtrager/cctools-port.git .cache/windows-kext/cctools
git -C .cache/windows-kext/cctools apply "$PWD/scripts/windows/cctools-cygwin.patch"
& .cache/windows-kext/cygwin/bin/bash.exe --noprofile --norc scripts/windows/build-ld64.sh
```

The package extractor downloads the exact versions and SHA-512 hashes in
`cygwin-packages.json`, validates them and extracts into `.cache/`. It never runs
the Cygwin installer. The patch adapts old dlfcn/endian declarations to modern
Cygwin and preserves macOS 11+ deployment version numbers. GCC uses `_GNU_SOURCE`.
If a mirror no longer retains a pinned package, use `--mirror` with another Cygwin
mirror serving the same bytes; do not remove hash verification.

## Build and validate

Replace the Zig/rcodesign paths below with the extracted executables:

```powershell
python scripts/windows/build.py --zig C:/tools/zig/zig.exe --ld64 .cache/windows-kext/cctools/cctools/ld64/src/ld/ld.exe --cygwin-bin .cache/windows-kext/cygwin/bin --codesign C:/tools/rcodesign.exe
```

Output: `build/windows-native/Ath10kMac.kext`. To preserve an earlier build, use
`--output build/another-name`. Existing bundles are never replaced automatically.
An optional `--kernel-collection C:/path/BootKernelExtensions.kc` checks import names
against that local collection. This file is not downloaded, uploaded or committed.

The builder regenerates and validates firmware, compiles all eight translation
units, links a real `MH_KEXT_BUNDLE`, generates the Lunnen native personality,
signs it ad-hoc, and validates the resulting file. Commands, tool exits and hashes
are saved alongside the bundle. A stack-frame limit is enforced.

`src/kext/Ath10kPayload.hpp` and `firmware/prepared/` are generated from the original
tracked firmware containers. They are ignored build outputs and must not be added
back to source control. Both Make and the Windows builder prepare them before use.

The LLVM option `-mllvm -disable-atexit-based-global-dtor-lowering` is necessary:
without it, this compiler adds `__cxa_atexit` despite the kext frontend options.
Keep module termination tables rather than linking userspace runtime substitutes.

The validator checks x86_64/kext format, bounds and relocation indices, rejects
weak and stack/exception/runtime imports, verifies every page of every SHA-1/SHA-256
CodeDirectory and the signed Info/requirements/resources, and checks that the two
workloop getters use different fields. Optional import matching only establishes
symbol-name presence, not kernel export policy or complete private-ABI compatibility.

`rcodesign verify` 0.29.0 reports an empty-CMS parse error on these ad-hoc signatures
and warns that its verifier is unreliable. It is not reported as a pass. The builder
uses independent page/special-slot hash checks instead. Apple verification and an
actual boot test remain outstanding.

These scripts have no EFI install, rollback or reboot operation. A successful build
does not establish that the native Wi-Fi menu, connections or WPA3 work.

The default is the full native driver (diagnostic stage 0). For the isolated controller diagnostic use `--diagnostic-stage 3`. No companion application is built or required. Cross-building the merged revision on Windows still requires a fresh Windows validation run.
