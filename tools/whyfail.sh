#!/bin/bash
# Why did the last join attempt fail? Prints airportd's join-relevant log lines and
# the driver's own view.
echo "=== driver"
ioreg -l -w0 | grep -o 'Ath10kConnectionState" = "[^"]*"' | head -1
ioreg -l -w0 | grep -o 'Ath10kLinkEvent" = "[^"]*"' | head -1
ioreg -l -w0 | grep -o 'Ath10kClientTrace" = "[^"]*"' | cut -c1-240
echo "=== airportd"
log show --last 120s --predicate 'process == "airportd"' --style compact 2>/dev/null \
  | grep -iE 'error=\(|Already associated|Auto-join aborted|Successfully (dis)?associated|setJoinEvent|Join failed' \
  | tail -8
