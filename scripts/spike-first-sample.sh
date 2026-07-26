#!/usr/bin/env bash
# Runs the daemon for 22 s and reports whether ANY gaze sample arrives.
# This is the go/no-go for the whole project (spec section 13 item 1).
#
# 22 s and not 15 s, for two independent reasons:
#   1. A silent device returns .recv on every handshake step, and
#      driveStateMachine runs 200 of them (tracker.zig:258) at ~100 ms per
#      blocking read (libusb_transport.zig:86). "failed to connect" therefore
#      only reaches the log at ~20 s, so a 15 s window killed the daemon
#      before it could ever report the one failure this spike exists to find.
#   2. drainGaze logs samples 1-3 and then every 500th (main.zig:68). At the
#      measured 33.2 Hz, #500 lands at ~15.1 s. Under 15 s the validity check
#      below only ever saw the first ~90 ms, when the sensor is still warming
#      up and legitimately reports vL=4 vR=4.
set -uo pipefail

BIN="$(cd "$(dirname "$0")/.." && pwd)/vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed"

if [ ! -x "$BIN" ]; then
    echo "FAIL: daemon binary not found or not executable: $BIN"
    echo "Run scripts/build.sh first."
    exit 1
fi

# Created after the binary check so the early exit above cannot leak one, and
# the trap is installed only once LOG is known to be set and non-empty.
LOG=$(mktemp) || { echo "FAIL: could not create temp log"; exit 1; }
trap 'rm -f "$LOG"' EXIT

"$BIN" > "$LOG" 2>&1 &
PID=$!
sleep 22
kill "$PID" 2>/dev/null

# Bounded reap. "quit" is a plain non-atomic bool (main.zig:33) read in
# "while (!quit)" (main.zig:559), so a ReleaseSafe build may hoist the load out
# of the loop and never observe SIGTERM. An unbounded wait would then hang
# before the log below is ever printed, leaving the operator with no output.
REMAINING=50
while [ "$REMAINING" -gt 0 ] && kill -0 "$PID" 2>/dev/null; do
    sleep 0.1
    REMAINING=$((REMAINING - 1))
done
if kill -0 "$PID" 2>/dev/null; then
    echo "WARN: daemon still alive 5 s after SIGTERM, sending SIGKILL"
    kill -9 "$PID" 2>/dev/null
fi
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
# Diagnosed directly rather than waiting on the daemon's own 200-step timeout,
# which needs ~20 s of the 22 s window and leaves no margin for a slow start.
if grep -q "handshake step" "$LOG" && ! grep -q "handshake complete" "$LOG"; then
    echo "FAIL: TTP handshake started but never completed (device silent; possible Windows provisioning requirement)"
    MATCHED=1
fi
if [ "$MATCHED" -eq 0 ]; then
    echo "FAIL: no gaze samples in 22 s, and none of the known failure patterns matched -- inspect the log above"
fi
exit 1
