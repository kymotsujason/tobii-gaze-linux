#!/usr/bin/env bash
# Runs the daemon for 15 s and reports whether ANY gaze sample arrives.
# This is the go/no-go for the whole project (spec section 13 item 1).
set -uo pipefail

BIN="$(cd "$(dirname "$0")/.." && pwd)/vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed"
LOG=$(mktemp)

if [ ! -x "$BIN" ]; then
    echo "FAIL: daemon binary not found or not executable: $BIN"
    echo "Run scripts/build.sh first."
    exit 1
fi

"$BIN" > "$LOG" 2>&1 &
PID=$!
sleep 15
kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

echo "--- daemon log ---"
cat "$LOG"
echo "------------------"

if grep -q "gaze #" "$LOG"; then
    echo "PASS: gaze samples observed"
    grep -m3 "gaze #" "$LOG"

    # A tracker that streams but never sees an eye (vL=4 vR=4, the
    # "not detected" code) is a real, different failure mode than no
    # samples at all, and the check above can't tell the two apart.
    # Only a handful of samples get logged (first 3, then every 500th),
    # so grep the ones we have rather than requiring a specific index.
    if grep "gaze #" "$LOG" | grep -qE "vL=0|vR=0"; then
        echo "OK: at least one eye detected in a logged sample"
    else
        echo "WARN: no valid eye detected in any logged sample (all vL=4 vR=4)"
        echo "WARN: device is streaming but sees no one -- check seating (~60cm from tracker), lighting, and that nothing blocks the sensor"
    fi
    exit 0
fi

# No gaze samples arrived. Diagnose from the log. These are independent
# checks, not an if/elif chain: a single root cause can trip more than
# one of them (interface-claim failure also trips the generic USB-open
# message), and printing all matches gives more clues than picking one.
MATCHED=0
if grep -qi "not found" "$LOG"; then
    echo "FAIL: device not enumerated (unplugged? udev rule not applied or device not replugged -- see scripts/check-device.sh, Task 2)"
    MATCHED=1
fi
if grep -qi "claim_interface" "$LOG"; then
    echo "FAIL: interface claim failed, another process owns the device (close tobiifree-overlay or any other client and retry)"
    MATCHED=1
fi
if grep -qi "failed to open USB" "$LOG"; then
    echo "FAIL: USB open failed (permissions? see Task 2, scripts/check-device.sh)"
    MATCHED=1
fi
if grep -qi "failed to connect" "$LOG"; then
    echo "FAIL: TTP handshake failed (possible Windows provisioning requirement)"
    MATCHED=1
fi
if [ "$MATCHED" -eq 0 ]; then
    echo "FAIL: no gaze samples in 15 s, and none of the known failure patterns matched -- inspect the log above"
fi
exit 1
