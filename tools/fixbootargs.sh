#!/bin/bash
# Restore the VM's boot-args to the full set plus ath10knoauto=1 (drop-in knob used to
# keep the driver's own auto-join out of airportd's way while testing click-to-join).
set -e
BASE="-v keepsyms=1 debug=0x144 serial=3 msgbuf=1048576 -no_compat_check"
JOIN=$(nvram boot-args 2>/dev/null | sed -n 's/.*\(ath10kjoin=[^ ]*\).*/\1/p')
NEW="$BASE ath10knoauto=1"
if [ -n "$JOIN" ]; then NEW="$NEW $JOIN"; fi
sudo -S -p "" nvram boot-args="$NEW"
nvram boot-args | tr ' ' '\n' | sed 's/:[^ ]*$/:<redacted>/'
