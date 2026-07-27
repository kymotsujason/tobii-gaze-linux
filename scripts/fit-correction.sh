#!/usr/bin/env bash
# One guided session that fits the host-side gaze correction and produces every
# number the acceptance test needs. About five minutes, three nine-point sweeps.
#
# ROOM LIGHTS ON. With them off this tracker loses the top-left point entirely,
# which has already cost this project three wasted runs.
#
# The three sweeps, and why each one exists:
#
#   1. fit       at your normal playing position. Fits gx, gy, bx, by into
#                ~/.local/share/tobii-gaze/correction.conf.
#   2. verify    at the SAME position, without moving. This is spec test 5.1:
#                a median of 35 to 50 px passes, above 80 px falsifies the model.
#   3. lateral   after shifting SIDEWAYS, 250 mm or more, at the same distance.
#                This is spec test 5.3, and it is the only test that says
#                whether the head-aware term earns its place.
#
# Sweep 3 must be a SIDEWAYS or UP-AND-DOWN move, not nearer or further. The eye
# projection has no depth term, so moving in depth leaves both candidate forms
# identical and decides nothing. The spec asks for 30 to 40 cm nearer or
# further; that is the one place this script deliberately departs from it, and
# the reasoning is in the task-13b report.
#
# No `set -e`: a sweep that finds no gaze exits non-zero and the script must say
# so rather than vanish.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$ROOT/gaze-cal/build/gaze-cal"
LOG="$HOME/.local/share/tobii-gaze/gaze-cal.log"

if [ ! -x "$CLI" ]; then
    echo "building gaze-cal"
    make -C "$ROOT/gaze-cal" || exit 1
fi

if ! systemctl --user is-active --quiet tobiifreed; then
    echo "tobiifreed is not running. Start it with:" >&2
    echo "  systemctl --user start tobiifreed" >&2
    exit 1
fi

pause() {
    echo
    echo "=============================================================="
    echo "$1"
    echo "=============================================================="
    read -r -p "Press Enter when you are settled. " _
}

pause "Room lights ON. Sit exactly where you play, and stay there for the
next two sweeps. Look at each white dot as it appears."

"$CLI" fit
fit_rc=$?
if [ "$fit_rc" -ne 0 ]; then
    echo
    echo "THE FIT DID NOT PRODUCE A CORRECTION (exit $fit_rc). Nothing was stored."
    echo "The most common cause is a point the tracker could not see. Turn the"
    echo "room lights up and run this script again."
    exit "$fit_rc"
fi

pause "DO NOT MOVE. The next sweep measures the correction that was just
fitted, at the same seating position. This is the acceptance test."

"$CLI" accuracy --label verify-normal
verify_rc=$?

pause "Now move SIDEWAYS. Slide the chair 250 to 300 mm to one side, or as
far as you comfortably can, keeping roughly the same distance from
the screen. Do not move nearer or further: that changes nothing this
test can measure. Sitting clearly higher or lower works too."

"$CLI" accuracy --label lateral
lateral_rc=$?

echo
echo "=============================================================="
echo "Done. fit=$fit_rc verify=$verify_rc lateral=$lateral_rc"
echo
echo "The verify sweep prints its own corrected median against the 35 to 50 px"
echo "prediction. For the head-aware question, score both candidate forms from"
echo "the recorded sweeps:"
echo
echo "  python3 $ROOT/tools/score_correction.py fit lateral"
echo
echo "Everything is in $LOG"
