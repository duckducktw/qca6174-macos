# Project status — honest L0/L1/L2/L3 map

**Truthfulness rule:** loading a kext is *not* Wi-Fi. This document states what
is actually implemented and tested versus what only has a skeleton or is
unimplemented. Nothing below is simulated or stubbed-as-success.

## What the layer labels mean here

| Layer | Meaning in this project |
|---|---|
| **L0** | Driver matches the PCI function, attaches, enables memory/bus-master, and maps BAR0. Raw MMIO identification is intentionally disabled pending access-path validation. |
| **L1** | Chip bring-up: register (MMIO) access to a woken SOC, Copy-Engine init, firmware + board download via BMI, HTC/WMI/HTT channels ready. |
| **L2** | 802.11 MAC up: scan, authenticate, associate; a network interface exists. |
| **L3** | IP connectivity (DHCP, traffic). |

> Separate from these driver layers, the task background notes that a pure
> PCI-ID spoof of QCA6174→AR9380 reaches at most **PCIe link power states
> L0/L1** and cannot scan/connect. This project does **not** rely on any such
> spoof; it is a native match on `168c:003e`.

## Current state

### L0 — PCI attach & identification: **SKELETON COMPLETE (M0), untested on HW**
- `kext/QCA6174Wifi/Info.plist` matches **only** `168c:003e`
  (`IOPCIPrimaryMatch 0x003e168c`), non-Apple bundle id. ✔ validated
  (`artifacts/verification/60_info_plist.txt`).
- `QCA6174Wifi::start()` casts `IOPCIDevice`, enables memory + bus-master, maps
  BAR0, validates the mapping, and returns `false` cleanly on every failure
  path. Raw chip-id MMIO is deliberately disabled until the HIF/BAR access path
  is verified. No fake Wi-Fi interface is published. ✔ code complete.
- **Not yet proven on hardware** — the kext cannot be compiled or loaded on this
  Linux host (no Xcode/macOS SDK). See docs/BUILD.md.

### L1 — chip bring-up: **PARTIAL (host-side only)**
Implemented and unit-tested on the host:
- **Firmware container parser** (`fw_file.c`) — parses real `firmware-6.bin`
  (version `WLAN.RM.4.4.1-00309-`, fw/otp images, wmi/htt op versions). ✔
- **Board-data parser** (`board_file.c`) — parses real `board-2.bin` (90
  entries) and finds the exact target key
  `bus=pci,vendor=168c,device=003e,subsystem-vendor=11ad,subsystem-device=0807`. ✔
- **Register/Copy-Engine bounds logic** (`qca6174_regs.c`) — address arithmetic
  + MMIO access gate, unit-tested incl. overflow/alignment. ✔ Constants pinned
  to the upstream revision in docs/ATTRIBUTION.md; live writes are absent.
- **hw-params table + board-key builder** (`hw_params.c`). ✔
- **Firmware-load state machine skeleton** (`fw_load_sm.c`) — advances only
  through the two host stages (parse, board-select) and returns `-ENOTSUP` at
  **BMI** and every stage beyond. It can never report a connected state. ✔

**Not implemented (return `ENOTSUP` / TODO, never faked):**
- SOC wake / register window access on live hardware
- Copy Engine ring setup + DMA
- **BMI** (boot-loader messaging / firmware download)
- **HTC** (host-target control)
- **WMI** (firmware command interface)
- **HTT** (host-target tx/rx)

### L2 — 802.11 MAC: **NOT STARTED**
No scan, no association, no `IO80211` interface. Apple's `IO80211Family` private
API integration is explicitly out of scope for this round.

### L3 — IP: **NOT STARTED**

## One-line summary
**L0 skeleton done (code), L1 host-side parsing/logic done + bring-up skeleton
returning ENOTSUP, L2/L3 not started.** Nothing claims to move packets.
