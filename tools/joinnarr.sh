#!/bin/bash
# Full join narrative from airportd's own log for the last 3 minutes, filtered to the
# lines that explain a manual join (no WiFiUsage noise).
log show --last 180s --predicate 'process == "airportd"' --style compact 2>/dev/null \
  | grep -iE 'Joining|Join |join failed|assoc|ASSOCIATE|Disassociat|error=\(|scan' \
  | grep -viE 'WiFiUsageSession|joinStateDidChange|QUERY JOIN HISTORY|WiFiUsageRealTime' \
  | tail -25
