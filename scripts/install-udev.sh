#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
sudo cp "$ROOT/vendor/tobiifree/assets/99-tobii.rules" /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger
echo "Installed. UNPLUG AND REPLUG the tracker now, then re-run scripts/check-device.sh"
