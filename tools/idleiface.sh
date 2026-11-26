#!/bin/bash
# Leave the interface idle so a manual join is a real join:
#  - drop QCA-TEST from airportd's preferred networks (it was added for testing, and
#    airportd then auto-joins it, which makes the manual join fail with error 37)
#  - disable airportd's own auto-join, then show the resulting state
sudo -S -p "" /usr/sbin/networksetup -removepreferredwirelessnetwork en1 QCA-TEST 2>&1 | head -2
sudo -S -p "" /usr/sbin/networksetup -setairportpower en1 off 2>&1 | head -1
sleep 2
sudo -S -p "" /usr/sbin/networksetup -setairportpower en1 on 2>&1 | head -1
sleep 3
echo "--- preferred list:"
networksetup -listpreferredwirelessnetworks en1 | head -5
echo "--- state:"
ioreg -l -w0 | grep -o 'Ath10kConnectionState" = "[^"]*"' | head -1
ioreg -l -w0 | grep -o 'Ath10kAutoJoinPhase" = "[^"]*"' | head -1
