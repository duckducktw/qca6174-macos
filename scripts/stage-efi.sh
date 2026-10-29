#!/usr/bin/env bash
# Stage an OpenCore EFI copy with Ath10kMac (native) for the Acer SF314-51.
# Runs on the target (Ubuntu). Never touches the live ESP unless --apply.
# Usage: stage-efi.sh <Ath10kMac-Sequoia-stageN.zip> [--apply]
set -euo pipefail
ZIP=${1:?zip}; APPLY=${2:-}
ESP=${ESP:-/mnt/opencore-efi}
WORK=$HOME/qca6174-efi-stage-$(date +%Y%m%d-%H%M%S)
mkdir -p "$WORK"
cp -a "$ESP/EFI" "$WORK/EFI"
cp -a "$WORK/EFI/OC/config.plist" "$WORK/config.plist.orig"
rm -rf "$WORK/EFI/OC/Kexts/Ath10kMac.kext"
unzip -q "$ZIP" -d "$WORK/EFI/OC/Kexts/"
test -x "$WORK/EFI/OC/Kexts/Ath10kMac.kext/Contents/MacOS/Ath10kMac"
python3 - "$WORK/EFI/OC/config.plist" <<'PY'
import plistlib,sys
path=sys.argv[1]; c=plistlib.load(open(path,'rb'))
add=c['Kernel']['Add']
for k in add:
    if k.get('BundlePath') in ('AirportItlwm.kext','itlwm.kext') and k.get('Enabled'):
        k['Enabled']=False; print('disabled',k['BundlePath'])
if not any(k.get('BundlePath')=='Ath10kMac.kext' for k in add):
    add.append({'Arch':'x86_64','BundlePath':'Ath10kMac.kext','Comment':'QCA6174 native (rollback: set Enabled false)',
                'Enabled':True,'ExecutablePath':'Contents/MacOS/Ath10kMac','MaxKernel':'24.99.99','MinKernel':'24.2.0',
                'PlistPath':'Contents/Info.plist'})
    print('added Ath10kMac.kext')
plistlib.dump(c,open(path,'wb'))
PY
for v in "$HOME/Downloads/opencore0.8.8/Utilities/ocvalidate/ocvalidate" "$HOME/.ocat/Database/linux/ocvalidate"; do
  [ -x "$v" ] && { "$v" "$WORK/EFI/OC/config.plist" | tail -5; break; }
done
echo "staged: $WORK"
if [ "$APPLY" = "--apply" ]; then
  sudo -S mount -o remount,rw "$ESP"
  sudo cp -a "$WORK/EFI/OC/Kexts/Ath10kMac.kext" "$ESP/EFI/OC/Kexts/"
  sudo cp "$WORK/EFI/OC/config.plist" "$ESP/EFI/OC/config.plist"
  sync; sudo mount -o remount,ro "$ESP"
  echo "applied; rollback: sudo cp $WORK/config.plist.orig $ESP/EFI/OC/config.plist"
fi
