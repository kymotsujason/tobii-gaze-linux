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
#                Optional now. Spec test 5.3 already ran and chose form S, so
#                this measures how fast the correction decays as you leave the
#                seat you fitted at, about 0.66 px per mm, rather than deciding
#                anything.
#
# Sweep 3 must be a SIDEWAYS or UP-AND-DOWN move, not nearer or further: the
# correction has no depth term, so moving in depth measures nothing.
#
# THE CORRECTION IS PER SEATING POSITION. Fit it where you actually play, and
# re-fit if you move your chair for good. Depth does not matter; sideways and
# height do.
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

pause "Room lights ON. Sit exactly where you play, about 600 mm from the
screen and no closer: nearer than about 520 mm the top row of dots
leaves the tracker's view and the fit will refuse. Stay put for the
next two sweeps, and look at each white dot as it appears."

"$CLI" fit
fit_rc=$?
if [ "$fit_rc" -ne 0 ]; then
    echo
    echo "THE FIT DID NOT PRODUCE A CORRECTION (exit $fit_rc). Nothing was stored."
    echo "Read the reason printed just above: it names which points were missing"
    echo "and, from the eye distance it measured, whether the cause was proximity"
    echo "or the room lights. Fix that one thing and run this script again."
    exit "$fit_rc"
fi

pause "DO NOT MOVE. The next sweep measures the correction that was just
fitted, at the same seating position. This is the acceptance test."

"$CLI" accuracy --label verify-normal
verify_rc=$?

pause "OPTIONAL, and only measures how fast the correction decays away from
the seat you just fitted at. Move SIDEWAYS 250 to 300 mm, keeping
roughly the same distance from the screen. Sitting clearly higher or
lower works too. Nearer or further measures nothing.
Press Enter to run it, or Ctrl-C to stop here: the fit and the verify
above are what matter."

"$CLI" accuracy --label lateral
lateral_rc=$?

echo
echo "=============================================================="
echo "Done. fit=$fit_rc verify=$verify_rc lateral=$lateral_rc"
echo
echo "The verify sweep prints its own corrected median against the 35 to 50 px"
echo "prediction, and the lateral one prints how far you moved and what that"
echo "cost. To re-score both candidate forms from the recorded sweeps, which is"
echo "how spec test 5.3 chose form S in the first place:"
echo
echo "  python3 $ROOT/tools/score_correction.py fit lateral"
echo
echo "Everything is in $LOG"
