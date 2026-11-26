> ## ⚠️ This is the `main` branch — the active work is on [`ventura-io80211`](https://github.com/duckducktw/qca6174-macos/tree/ventura-io80211)
>
> `main` is the **staged "M0/M1" milestone tree** described below (PCI probe, firmware/board parsing,
> host-side models). It has been superseded: it is not the driver you want to try.
>
> The active line — macOS 13 Ventura, QCA6174 hw3.x — lives on
> **[`ventura-io80211`](https://github.com/duckducktw/qca6174-macos/tree/ventura-io80211)**, and that
> branch's README is the project front page: status matrix, install, and the reverse engineering of
> Apple's join admission conditions. It is the first public driver to put **QCA6174 scan results into
> macOS's own Wi-Fi UI**; the click-to-join flow is still blocked inside `airportd` and is documented
> there rather than hidden.
>
> ```sh
> git clone -b ventura-io80211 https://github.com/duckducktw/qca6174-macos.git
> ```
>
> | Branch | State |
> |---|---|
> | [`ventura-io80211`](https://github.com/duckducktw/qca6174-macos/tree/ventura-io80211) | **active** — native UI scan + in-kernel WPA2 + reconnect working in the test VM; click-to-join blocked |
> | `main` (this branch) | frozen M0/M1 milestone tree, kept for history |
> | `native-qca6174-dev`, `native-qca6174-11ad-0807`, `full-driver-11ad-0807` | historical pre-Ventura lines |

---

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
