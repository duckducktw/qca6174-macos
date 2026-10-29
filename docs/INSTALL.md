# Installing & testing on real hardware (manual, test machine only)

> **Scope / safety:** every step here is performed BY A HUMAN on a dedicated
> test Mac. This repository's tooling never installs, signs, reboots, or edits
> an EFI/NVRAM/remote machine. The task's hard rules forbid it and nothing in
> `scripts/` does it.

## Preconditions
- A test machine (Acer Swift SF314-51 in the target scenario) booting macOS via
  OpenCore with SMBIOS `MacBookPro14,1`.
- The QCA6174 (`168c:003e`, subsystem `11ad:0807`) present at
  `PciRoot(0x0)/Pci(0x1C,0x3)/Pci(0x0,0x0)`.
- firmware on disk at the macOS-side path your bring-up code will load from
  (M0 does not load firmware; this matters only once BMI exists).
- A way to recover (second boot disk / USB OpenCore) — see risks.

## What M0 will and will not do once loaded
- **Will:** match the device, map BAR0, log identification if `-qca6174debug` is
  set, and attach. Visible via `ioreg -l | grep QCA6174Wifi`.
- **Will NOT:** create a Wi-Fi interface, scan, or connect. There is no network
  functionality yet (see docs/STATUS.md).

## Suggested first-light procedure (human)
1. Remove/disable the stale `AirportItlwm.kext` for this device — it is for
   Intel hardware and must not be relied on for the QCA6174.
2. Build `QCA6174Wifi.kext` per docs/BUILD.md on the test Mac.
3. Boot with SIP configured for unsigned kext dev as appropriate, and add the
   boot-arg `-qca6174debug` to see logs.
4. Load for one session WITHOUT installing permanently:
   ```sh
   sudo kmutil load -p /path/to/QCA6174Wifi.kext     # Big Sur+
   # (older: sudo kextutil -v /path/to/QCA6174Wifi.kext)
   ```
5. Observe:
   ```sh
   log stream --predicate 'eventMessage CONTAINS "QCA6174Wifi"' --info
   ioreg -l | grep -i QCA6174
   ```
6. Confirm `start:` logged PCI identity and BAR0 mapping, followed by the
   message that raw chip-id MMIO is intentionally disabled.
7. Unload: `sudo kmutil unload -b com.opensource.qca6174.QCA6174Wifi`.

## Risks before real-hardware testing
- Register constants are pinned to the revision recorded in
  docs/ATTRIBUTION.md, but direct chip-id MMIO remains disabled until its
  HIF/BAR access path is verified. A wrong future *write* could wedge the
  device; M0 performs no MMIO writes.
- **Panic risk:** loading kexts can panic the kernel. Keep a recovery boot path.
  M0 avoids busy loops, uses timeouts-by-construction (no polling yet), and
  releases all resources on every failure path to reduce panic/leak risk.
- **Matching:** the personality matches only `168c:003e`; it will not bind to
  the host's other NICs. Still, review `ioreg` to confirm it attached to the
  intended function.
- **Do not** mix with a PCI-ID spoof to AR9380; this driver is a native match
  and the two approaches conflict.
- **SIP / signing:** unsigned kexts require lowered security; do this only on a
  disposable test install.
