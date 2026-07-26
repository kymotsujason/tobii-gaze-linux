#!/usr/bin/env bash
# Confirms the tracker is present AND writable without root.
# Matches both product IDs that 99-tobii.rules covers, so a tracker sitting in
# FBL/bootloader mode is reported as such instead of as absent.
# Exit: 0 runtime and writable, 1 not enumerated, 2 bootloader mode, 3 not writable.
set -euo pipefail

VID=2104
PID_RUNTIME=0313
PID_FBL=0102

command -v lsusb >/dev/null 2>&1 || {
  echo "FAIL: lsusb not found; install usbutils" >&2
  exit 1
}
usb=$(lsusb)

mode=runtime
match=$(grep -i "$VID:$PID_RUNTIME" <<<"$usb") || {
  match=$(grep -i "$VID:$PID_FBL" <<<"$usb") || {
    echo "FAIL: tracker not enumerated (no $VID:$PID_RUNTIME runtime device, no $VID:$PID_FBL bootloader device)"
    exit 1
  }
  mode=bootloader
}

# One device per run: take the first match so the node path stays coherent.
line=${match%%$'\n'*}
[ "$line" = "$match" ] || echo "note: more than one $mode device matched, using the first"
echo "found ($mode): $line"

ids=$(sed -n 's/^Bus \([0-9][0-9]*\) Device \([0-9][0-9]*\):.*/\1 \2/p' <<<"$line")
[ -n "$ids" ] || {
  echo "FAIL: cannot parse bus/device out of lsusb line: $line" >&2
  exit 1
}
read -r bus dev <<<"$ids"
node="/dev/bus/usb/$bus/$dev"

[ -e "$node" ] || {
  echo "FAIL: $node does not exist although lsusb lists the device"
  exit 1
}
ls -l "$node"

[ -w "$node" ] || {
  echo "FAIL: $node not writable, udev rule not applied or device not replugged"
  exit 3
}

if [ "$mode" = bootloader ]; then
  echo "OK: writable without root"
  echo "FAIL: tracker is in FBL/bootloader mode ($VID:$PID_FBL), not runtime mode ($VID:$PID_RUNTIME)."
  echo "      Gaze streaming stays unavailable until the runtime firmware is flashed."
  exit 2
fi
echo "PASS: writable without root"
