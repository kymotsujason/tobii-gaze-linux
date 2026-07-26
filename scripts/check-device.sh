#!/usr/bin/env bash
# Confirms the tracker is present AND writable without root.
set -euo pipefail
line=$(lsusb | grep -i '2104:0313') || { echo "FAIL: tracker not enumerated"; exit 1; }
echo "found: $line"
bus=$(echo "$line" | sed 's/Bus \([0-9]*\).*/\1/')
dev=$(echo "$line" | sed 's/.*Device \([0-9]*\):.*/\1/')
node="/dev/bus/usb/$bus/$dev"
ls -l "$node"
[ -w "$node" ] && echo "PASS: writable without root" || { echo "FAIL: not writable, udev rule not applied or device not replugged"; exit 1; }
