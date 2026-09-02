# Paste this into the next session

```
Resume the Tobii ET5 gaze overlay project in /home/jason/Documents/tobii-eye-tracker.

Read CLAUDE.md and docs/RESUME-phase1.md first. Between them they have the design, the
measured hardware facts, the patch workflow, and the current state.
docs/wip/phase1-ledger.md is the full execution record; read the 2026-09-02 entry.

Branch feat/phase1-bringup, which is also this repo's default branch on GitHub. Opus for
implementers and reviewers. Do not close a task without a review verdict, and have reviewers
WRITE their verdict to a file rather than only returning it, because the reply channel drops
subagent messages. Working method is superpowers:subagent-driven-development.

WHERE THINGS STAND. Phase 1 tasks 1-14 are complete and reviewed. Task 13b (host-side form S
correction) is complete AND FITTED ON HARDWARE: correction.conf holds gx 1.15236 gy 1.11549,
and a verify sweep measured 33 px, within one degree, against 249 px raw. Task 15's recorder
(gaze-cal record) is complete. A separate spec and plan added `gaze-cal setup`, a fullscreen
view of the device's track box with both eyes, the distance against the 520 mm floor, the raw
and corrected gaze, and the fit and verify sweeps run from that screen; four subagent tasks,
each reviewed, live check passed. Its final whole-branch review was deliberately NOT run, and
docs/RESUME-phase1.md's "Deferred minors" section is the list it would have triaged.

WHAT IS ACTUALLY LEFT.
1. Task 15 step 2: five minutes of real osu through `gaze-cal record traces/osu-YYYYMMDD.csv`.
   Needs the human playing. It refuses without a usable form S fit; never use --raw for this.
   Then the brief's Step 3 python: median dt near 30.3 ms, invalid fraction in low single
   digits.
2. Task 16: tools/fit_filter.py, refit the one euro constants in degrees against that trace.
3. The human was investigating whether the TRACKER'S PHYSICAL ANGLE explains the top-of-screen
   dropout. Ask before touching it. tilt_deg, z_mm, cx and cy in ~/.config/tobii.json have
   never been measured. The 2026-09-02 ledger entry says what to measure and how to test it.

THE TOP DROPOUT, SETTLED AS FAR AS IT GOES. At about 550 mm looking top left the device
reported NO EYES for 496 consecutive frames (eye_present 0 on both, origins zero); the same
seat looking at the centre had both eyes present and gaze valid. So it is eye DETECTION that
fails, not the gaze estimate, which rules out the eye model and points at illumination and
glint geometry at a steep upward angle. Nothing in the vendored driver configures
illumination, exposure or mode. Read raw columns with
vendor/tobiifree/scripts/dump_gaze_columns.mjs (needs the wasm core built and the daemon
stopped).

WHAT THE HUMAN MUST DO, when asked. Room lights ON, sit at about 600 mm, no closer than 520.
Both are hard requirements found by measurement. They have run well over a dozen sweeps, so
ask for at most one thing at a time and say what it will decide.

LEARNED THE HARD WAY:
- Treat code quoted in a plan as a draft. Every task in the setup-view plan found a real
  defect in its own brief by testing: a threshold in the wrong unit, a key-polling bug, three
  impossible tests, and a window-mode guard that silently returned the wrong window.
- When a number matters, ask where it came from.
- Amending a patch needs `git diff HEAD`, not `git diff`. See CLAUDE.md.
- The refusals (partial sweep, isotropy, stale geometry, stale form, post-refit outlier) each
  blocked a plausible wrong answer. Do not relax them.
- A worktree agent is pinned to its worktree, so its commits must be cherry-picked onto the
  branch. Three landed that way this session with no conflicts.
```
