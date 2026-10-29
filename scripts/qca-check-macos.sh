#!/bin/sh
# Run in macOS:  sudo sh /Volumes/EFI/qca-check.sh
# Writes results back to the OpenCore ESP so they can be read from Ubuntu.
OUT="/Volumes/EFI/qca-results-$(date +%Y%m%d-%H%M%S).txt"
{
echo "== system"; sw_vers; uname -a; nvram boot-args 2>/dev/null
echo "== loaded kexts"; kmutil showloaded 2>/dev/null | grep -Ei "Ath10k|QCARome|BlueTool|Airport|Itlwm" || kextstat | grep -Ei "Ath10k|QCARome|BlueTool"
echo "== Wi-Fi PCI / driver"; ioreg -r -c IOPCIDevice -l -w0 | grep -E '"(device-id|subsystem-id|IOName)"|Ath10k' | head -40
ioreg -r -c Ath10kPCIDevice -l -w0 | grep -E "Ath10k[A-Za-z]*\" = " | head -60
echo "== Bluetooth USB"; ioreg -p IOUSB -l -w0 | grep -E '"(idVendor|idProduct|USB Product Name)"|QCARomePatchRAM' | head -40
ioreg -l -w0 | grep QCARomePatchRAM- | head -20
echo "== interfaces"; networksetup -listallhardwareports; ifconfig -a | grep -E "^[a-z]|inet |status"
echo "== system_profiler"; system_profiler SPAirPortDataType SPBluetoothDataType 2>/dev/null | head -80
echo "== kernel log (this boot)"; log show --last boot --style compact --predicate 'sender == "Ath10kMac" OR sender == "QCARomePatchRAM" OR eventMessage CONTAINS[c] "Ath10k" OR eventMessage CONTAINS "QCARomePatchRAM"' 2>/dev/null | tail -300
echo "== panics"; ls -lt /Library/Logs/DiagnosticReports 2>/dev/null | grep -i panic | head -5
} > "$OUT" 2>&1
sync
echo "saved: $OUT"
