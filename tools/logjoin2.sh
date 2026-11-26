#!/bin/bash
# One click-join attempt + the airportd log lines that explain the outcome.
cd /tmp || exit 1
./cwjoin QCA-TEST en1 > /tmp/cwjoin.out 2>&1
echo "=== cwjoin"
grep -E 'assoc-ok|assoc-error|ssid-now' /tmp/cwjoin.out
echo "=== driver view"
ioreg -l -w0 | grep -o 'Ath10kConnectionState" = "[^"]*"' | head -1
ioreg -l -w0 | grep -o 'Ath10kAssocWho" = "[^"]*"' | head -1
ioreg -l -w0 | grep -o 'Ath10kClientTrace" = "[^"]*"' | cut -c1-260
echo "=== airportd log (join-related)"
log show --last 40s --predicate 'process == "airportd"' --style compact 2>/dev/null \
  | grep -iE 'join|assoc|disassoc|error=' | grep -viE 'WiFiUsageSession|joinStateDidChange|WiFiUsageRealTime' | tail -15
