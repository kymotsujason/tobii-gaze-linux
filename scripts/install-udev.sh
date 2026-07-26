#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RULES="$ROOT/vendor/tobiifree/assets/99-tobii.rules"

# Checked before the first sudo so a missing submodule does not cost a password prompt.
if [ ! -f "$RULES" ]; then
  echo "rules file not found: $RULES" >&2
  echo "vendor/tobiifree is not initialised; run: git submodule update --init --recursive" >&2
  exit 1
fi

sudo cp "$RULES" /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=2104
sudo udevadm settle --timeout=10 || echo "warning: udev events did not settle within 10s" >&2
echo "Installed. UNPLUG AND REPLUG the tracker now, then re-run scripts/check-device.sh"
