# Paste this into the next session

```
Resume the Tobii ET5 gaze overlay project in /home/jason/Documents/tobii-eye-tracker.

Read CLAUDE.md and docs/RESUME-phase1.md first. Between them they have the design, the
measured hardware facts, the patch workflow, and the current state.
docs/wip/phase1-ledger.md is the full execution record; read the 2026-09-01 entry.

We are executing docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md using the
superpowers:subagent-driven-development skill, on branch feat/phase1-bringup. Opus for
implementers and reviewers. Do not close a task without a review verdict, and have
reviewers WRITE their verdict to a file rather than only returning it, because the reply
channel drops subagent messages.

WHERE THINGS STAND. Tasks 1-12 and 14 are complete and reviewed. Task 13 revealed that the
ET5's own calibration cannot fix an ~18 percent isotropic gain, so Phase 1 grew Task 13b, a
host-side correction. Form S (a static affine per axis) is complete and reviewed clean at
513da1b, after a fix round that added a post-refit refusal (GZ_FIT_ERR_REFIT) closing the
opposite-axis outlier hole; it fires on none of the seven recorded sweeps. Task 15's
recorder, gaze-cal record, is code complete and reviewed clean at 75f433e. make check at
75f433e exits 0 with 201 killed, 7 documented, 0 unexpected, verified by the controller.

FIRST THING TO DO. Nothing runs until the human re-fits. The correction.conf on disk is
still a form H file and form S refuses it (measured against the real file). Ask for ONE
thing: run ./scripts/fit-correction.sh with room lights ON, seated where they play, about
600 mm back and no closer than 520. It runs the fit sweep, then the verify sweep without
moving; the third sweep is optional. Verify inside 35 to 50 px and within one degree means
form S holds on the live device. Above 80 px falsifies it and the next step is an
investigation, not a recording.

THEN. Task 15 step 2: gaze-cal record traces/osu-YYYYMMDD.csv while they play five minutes
of real osu, easy and hard maps. It refuses without a usable form S fit; never use --raw for
this. Read the "samples missed" line, then run the brief's Step 3 python (median dt near
30.3 ms, invalid fraction in the low single digits). Then Task 16, tools/fit_filter.py,
refits the one euro constants in degrees against that trace. After Task 16, the final
whole-branch review; the deferred minors are listed in the workspace ledger
.superpowers/sdd/2026-07-26-phase1-bringup-and-daemon/progress.md.

WHAT THE HUMAN MUST DO. Room lights ON, and sit at about 600 mm, no closer than 520. Both
are hard requirements found by measurement. They have run well over a dozen sweeps, a tape
session and an IPD measurement, so ask for at most one thing at a time and say what it
will decide.

LEARNED THE HARD WAY, all of it earned:
- Treat code quoted in the plan as a draft. Eight tasks found defects in plan-mandated code
  and every one was caught by testing rather than reading.
- When a number matters, ask where it came from.
- Amending a patch needs `git diff HEAD`, not `git diff`. See CLAUDE.md.
- The refusals (partial sweep, isotropy, stale geometry, stale form, post-refit outlier)
  each blocked a plausible wrong answer. Do not relax them.
- Agent worktrees: the harness pins a worktree agent to its worktree, so its commits must be
  cherry-picked onto the branch. Both Task 15 commits applied clean that way.
```
