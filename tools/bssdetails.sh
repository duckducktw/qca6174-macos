#!/bin/bash
# Trigger one join attempt and print the BSSDetails airportd records for the target,
# which is where a mis-parsed scan record shows up.
cd /tmp || exit 1
./cwjoin QCA-TEST en1 > /tmp/cwjoin.out 2>&1
grep -E 'assoc-ok|assoc-error' /tmp/cwjoin.out
log show --last 90s --predicate 'process == "airportd"' --style compact 2>/dev/null \
  | grep -iE 'BSSDetails|setJoinEvent: Joining|error=\(' | tail -6
