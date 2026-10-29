# QCA6174 macOS driver (staged, auditable)

A from-scratch, open-source macOS IOKit kext for the Qualcomm Atheros **QCA6174**
(`168c:003e`, subsystem Lite-On `11ad:0807`), built in honest milestones. This
is **not** a finished Wi-Fi driver — see [`docs/STATUS.md`](docs/STATUS.md) for
the exact L0/L1/L2/L3 reality.

- **M0 — safe PCI match & probe skeleton:** code complete (`kext/`).
- **M1 — PCI/CE basics, firmware/board parsing, load-SM skeleton:** host-side
  parsing + logic complete and unit-tested; on-device bring-up (BMI/HTC/WMI/HTT)
  deliberately returns `ENOTSUP`.

It does **not** reuse the stale Intel `AirportItlwm.kext`, nor spoof the device
as an AR9380.

## Layout

| Path | Purpose |
|---|---|
| `src/common/` | Freestanding C11, no IOKit. Firmware/board TLV parsers, register/Copy-Engine bounds logic, hw-params, firmware-load state machine. Shared by host tests **and** the kext. |
| `kext/QCA6174Wifi/` | The macOS IOKit kext: `Info.plist` (matches only `168c:003e`, non-Apple id) + `QCA6174Wifi.{hpp,cpp}` (M0 probe). **Builds only on macOS/Xcode.** |
| `tests/` | Host unit tests + a tiny harness, plus a probe that parses the real system firmware. |
| `scripts/` | `prepare_fixtures.sh` (decompress system firmware, read-only), `run_verification.sh` (full suite → artifacts). |
| `docs/` | `STATUS.md`, `BUILD.md`, `INSTALL.md`, `ATTRIBUTION.md`. |
| `artifacts/verification/` | Saved real outputs from the last verification run. |

## Quick start (host)

```sh
make test                       # unit + real-firmware probe + C++ boundary smoke
./scripts/run_verification.sh   # same, with outputs saved under artifacts/verification/
```

The macOS kext is built separately with Xcode; see
[`docs/BUILD.md`](docs/BUILD.md). It cannot be built on Linux.

## License
ISC. Firmware/board ABI and register layout derive from the ISC-licensed Linux
`ath10k` driver — see [`docs/ATTRIBUTION.md`](docs/ATTRIBUTION.md).
