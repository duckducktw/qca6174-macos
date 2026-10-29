# Attribution & provenance

This project reimplements, in clean-room style, the *public on-disk ABI* and a
small amount of *hardware register layout* information for the Qualcomm Atheros
QCA6174, as documented by the upstream Linux `ath10k` driver. No source code was
copied verbatim; the C here is original and bounds-checked for kernel use.

## Upstream source

- **Project:** Linux kernel, `drivers/net/wireless/ath/ath10k/`
- **License:** ISC (compatible with this project's ISC license)
- **Canonical tree:**
  https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/ath/ath10k
- **Reference also understood (NOT copied):** OpenIntelWireless/itlwm — consulted
  only to understand the shape of macOS IOKit Wi-Fi glue. itlwm is GPL/ported-BSD
  and architecturally Intel-specific; none of its code or license is imported.

> The installed linux-headers package does not include ath10k C sources, so the
> review used Linux Code Browser's generated source for revision
> `v6.19-rc8-185-g2687c848e578` and records the checked constants below.

## What is grounded where

| Element | Source | Verification on this host |
|---|---|---|
| `ATH10K_FW_MAGIC` = "QCA-ATH10K", header alignment (12 B) | ath10k `core.c` | **Empirically confirmed** by `xxd` of real `firmware-6.bin` (see `artifacts/verification/10_fixture_headers.txt`) |
| `enum ath10k_fw_ie_type` (IE ids 0..7) | ath10k `core.h` | IDs 0/1/2/3/4/5/6 observed in the real image; parser reports 7 known IEs, 0 unknown |
| `ATH10K_BOARD_MAGIC` = "QCA-ATH10K-BOARD", header alignment (20 B) | ath10k `bmi.h`/`core.c` | **Empirically confirmed** by `xxd` of real `board-2.bin` |
| `enum ath10k_bd_ie_type`, `*_board_type` (nested TLV) | ath10k `bmi.h` | Confirmed: 90 board entries parsed, target key matched |
| QCA6174 target/BMI hw-version constants (`0x0502...` etc.) | ath10k `hw.h` / `core.c` | **Verified** against Code Browser Linux revision `v6.19-rc8-185-g2687c848e578`; these are target-version signatures, not raw SOC chip-id values |
| QCA6174 CE bases/stride, chip-id table offset, qcax CE register offsets | ath10k `hw.c` | **Verified** against the same revision: chip-id `0xf0`; CE0 `0x34400`; CE stride `0x400`; control/src-write/dst-write offsets `0x10/0x3c/0x40`. Direct BAR0 chip-id read remains disabled until the host access path is independently verified |

## Firmware images

The firmware/board files under `/lib/firmware/ath10k/QCA6174/hw3.0/` are
redistributed by `linux-firmware` under their own license
(`LICENSE.QualcommAtheros_ath10k`, see the `notice_ath10k_firmware-*.txt` files
in that directory). They are **not** copied into this repository; the test
harness decompresses them into `build/` at test time only. Do not commit
firmware blobs here.

## Source pin and remaining validation

The constants above were checked against Linux Code Browser revision
`v6.19-rc8-185-g2687c848e578` on 2026-10-29. Before enabling **any write** or
the currently-disabled direct chip-id read, verify how ath10k's HIF maps
`SOC_CHIP_ID_ADDRESS` on QCA6174 and preserve that exact access path.
