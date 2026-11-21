# Hardware verification on the Acer Swift SF314-51 (QCA6174 `168c:003e` / Lite-On `11ad:0807`)

Hardware-verified claims in this repository must come from a real macOS boot on
the target laptop. This document is the reproducible procedure and records the
stage trap that wasted one round.

## Build stages — do not deploy the wrong one

| Build | `DIAGNOSTIC_STAGE` | Behaviour |
|---|---|---|
| Functional | `0` | PCI attach → firmware/board download → CE/HTC/WMI → radio start → native `IO80211Interface` |
| Controller-only diagnostic | `3` | Stops before radio startup. No interface, no scan, nothing in System Settings. |

Both packages are named `Ath10kMac.kext` with the same `CFBundleVersion`, so the
only reliable way to know what is deployed is the Mach-O hash:

```sh
shasum -a 256 /Volumes/EFI/EFI/OC/Kexts/Ath10kMac.kext/Contents/MacOS/Ath10kMac
```

Compare against the stage zips produced by CI (`Ath10kMac-Sequoia-stage0.zip` /
`Ath10kMac-Sequoia-stage3.zip`). Stage 3 and stage 0 differ in hash only; a
size check is not enough.

## Procedure

1. On Ubuntu, stage the functional package onto the OpenCore ESP (FAT, mounted at
   `/mnt/opencore-efi` or `/mnt/esp3`):

   ```sh
   unzip -q Ath10kMac-Sequoia-stage0.zip -d /tmp/stage0
   sudo mount -o remount,rw "$ESP"
   sudo rm -rf "$ESP/EFI/OC/Kexts/Ath10kMac.kext"
   sudo cp -a /tmp/stage0/Ath10kMac.kext "$ESP/EFI/OC/Kexts/"
   sync; sudo mount -o remount,ro "$ESP"
   shasum -a 256 "$ESP/EFI/OC/Kexts/Ath10kMac.kext/Contents/MacOS/Ath10kMac"
   ```

   `config.plist` must keep `Kernel > Add > Ath10kMac.kext` enabled
   (`Arch x86_64`, `MinKernel 24.0.0`) and `AirportItlwm.kext` disabled.

2. Keep the pre-change `Ath10kMac.kext` and `config.plist` outside the ESP so a
   rollback needs no network.

3. Reboot into macOS from the OpenCore picker, then run the dump script from the
   ESP (it writes results back to the ESP so Ubuntu can read them):

   ```sh
   sudo sh /Volumes/EFI/qca-check.sh
   ```

   The script records `sw_vers`, loaded kexts, the `Ath10kPCIDevice` /
   `IO80211Interface` / `IO80211Controller` IORegistry nodes,
   `networksetup -listallhardwareports`, `system_profiler SPAirPortDataType`,
   `ifconfig`, and this boot's kernel log filtered to Ath10k.

4. Reboot to Ubuntu and read `/boot/efi/qca-results-*.txt` (the second ESP) or
   mount the OpenCore ESP read-only.

## Acceptance for "native System Settings"

`networksetup -listallhardwareports` lists a Wi-Fi port backed by the driver
interface, `ioreg -c IO80211Interface` shows the instance owned by
`Ath10kPCIDevice`, and the Wi-Fi pane in System Settings lists networks. A
loaded kext alone is **not** acceptance.

## Known trap (2026-11-20)

The ESP was found carrying the **stage 3** binary (hash matching
`Ath10kMac-Sequoia-stage3.zip`), so the radio never started and no interface was
ever published — the Wi-Fi pane could not show anything. Stage 0 was staged in
its place; its result has not been recorded yet.
