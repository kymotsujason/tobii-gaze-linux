#!/usr/bin/env bash
# Task 13, spec section 13 item 2: does the ET5 keep its calibration across a
# power cycle?
#
# THE CONFOUND THIS SCRIPT EXISTS TO REMOVE. The daemon replays calibration by
# itself: applySavedCalibration() in applications/tobiifreed/src/main.zig logs
# "re-applied N-byte calibration after reconnect" whenever saved_cal_len is
# non-zero. saved_cal is written by cal_apply and lives only in that process,
# so a replug measured against a daemon that has calibrated in this session
# answers a different question: it tells you the daemon replayed, not that the
# device remembered. Restarting the daemon first empties saved_cal without
# touching the device's own power, which is what makes the replug measure the
# device.
#
# Run this AFTER `gaze-cal calibrate` has succeeded. It needs the human twice:
# for the nine dots of each accuracy pass, and for the physical unplug. Only
# pulling the cable removes VBUS, so scripts/daemon-tests/reset_tobii cannot
# substitute: a port reset leaves the device powered.
#
# No `set -e`: the accuracy passes exit non-zero when the tracker sees no eyes,
# and that is a result to report rather than a reason to abandon the run.
set -uo pipefail

CAL=${CAL:-/home/jason/Documents/tobii-eye-tracker/gaze-cal/build/gaze-cal}
LOG=${LOG:-$HOME/.local/share/tobii-gaze/gaze-cal.log}

step() { printf '\n=== %s ===\n' "$*"; }

pause() {
    # stderr, not stdout: every caller runs inside $( ), which would otherwise
    # swallow the prompt and leave the operator staring at a silent terminal.
    printf '\n>>> %s\n>>> Press Enter when ready.\n' "$*" >&2
    read -r _
}

# The median error line the accuracy pass prints, or "n/a".
run_accuracy() {
    local label=$1 out
    pause "LOOK AT EACH WHITE DOT in turn. Nine dots, about 1.3 s each."
    out=$("$CAL" accuracy --label "$label" 2>&1)
    printf '%s\n' "$out" >&2
    printf '%s\n' "$out" | grep -m1 "median error" || echo "median error: n/a"
}

wait_for_device() {
    local deadline=$((SECONDS + ${1:-60}))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if "$CAL" status 2>/dev/null | grep -q "device_present=1"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

[ -x "$CAL" ] || { echo "no gaze-cal at $CAL"; exit 2; }

if ! "$CAL" status | grep -q "device_present=1"; then
    echo "the daemon does not see the tracker; fix that before starting"
    exit 2
fi
if [ ! -f "$HOME/.local/share/tobii-gaze/calibration.bin" ]; then
    echo "no saved calibration. Run '$CAL calibrate' first: this script measures"
    echo "whether that calibration survives, and cannot make one."
    exit 2
fi

step "A. baseline, calibrated, daemon still holding the blob"
A=$(run_accuracy after-cal)

step "B. daemon restarted, so its replay buffer is empty"
echo "The device is not power-cycled here. Anything that survives B is held by"
echo "the device rather than by the daemon."
systemctl --user restart tobiifreed
sleep 2
wait_for_device 60 || { echo "the daemon did not come back"; exit 1; }
"$CAL" status
B=$(run_accuracy after-daemon-restart)

SINCE=$(date '+%Y-%m-%d %H:%M:%S')

step "C. physical unplug, which is the actual experiment"
pause "UNPLUG the tracker's USB cable now. Wait five seconds. Plug it back in."
echo "waiting for the daemon to see the device again..."
wait_for_device 120 || { echo "the tracker did not come back"; exit 1; }
sleep 3
"$CAL" status
echo
echo "daemon log since the unplug:"
journalctl --user -u tobiifreed --since "$SINCE" --no-pager 2>/dev/null \
    | grep -Ei "reconnect|re-applied|calibration|display area" | tail -20
echo
if journalctl --user -u tobiifreed --since "$SINCE" --no-pager 2>/dev/null \
        | grep -q "re-applied"; then
    echo "WARNING: the daemon replayed a calibration, so C does not measure the"
    echo "device. That means the restart in step B did not take effect."
fi
C=$(run_accuracy after-replug)

step "D. saved blob re-applied from disk"
"$CAL" apply-saved
D=$(run_accuracy after-apply-saved)

step "RESULT"
printf 'A calibrated, daemon holds the blob : %s\n' "$A"
printf 'B after a daemon restart            : %s\n' "$B"
printf 'C after a physical power cycle      : %s\n' "$C"
printf 'D after apply-saved                 : %s\n' "$D"
cat <<'EOF'

How to read this. B tells you whether the DEVICE holds the calibration when the
daemon has forgotten it. C tells you whether it survives losing power. If C is
close to A and B, calibration persisted and --apply-saved is insurance. If C is
much worse and D returns it to A, calibration is lost on every power cycle and
applying the saved blob is mandatory rather than optional.

Everything above is appended to the log for later reading.
EOF
echo "log: $LOG"
