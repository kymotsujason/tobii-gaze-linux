# Paste this into the next session

```
Resume the Tobii ET5 gaze overlay project in /home/jason/Documents/tobii-eye-tracker.

Read CLAUDE.md and docs/RESUME-phase1.md first. Between them they have the design, the
measured hardware facts, the patch workflow, and the current state.
docs/wip/phase1-ledger.md is the full execution record; skim the entries dated 2026-07-27.

We are executing docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md using the
superpowers:subagent-driven-development skill, on branch feat/phase1-bringup. Opus 5 for
implementers and reviewers. Do not close a task without a review verdict, and have
reviewers WRITE their verdict to a file rather than only returning it, because the reply
channel drops subagent messages.

WHERE THINGS STAND. Tasks 1-12 and 14 are complete and reviewed. Task 13 ran a real
calibration on hardware and revealed that the ET5's own calibration cannot fix an ~18
percent isotropic gain in its gaze direction, so Phase 1 grew Task 13b, a host-side
correction that is not in the plan. Form H of that correction is shipped and reviewed
clean, measuring 37 px at the fitted position, inside the one-degree target, against
250-285 px raw.

THE ONE THING IN FLIGHT. An agent was implementing FORM S of the correction when the last
session ended. Spec test 5.3 decided against the head-aware form: with a real 112 mm
lateral head move, form H measured 69 px and form S 52 px, so the scale centre is not the
eye. Check these before doing anything, since the work may already be done:
  .superpowers/sdd/2026-07-26-phase1-bringup-and-daemon/task-13b-formS-summary.md
  .superpowers/sdd/2026-07-26-phase1-bringup-and-daemon/task-13b-report.md
  git log, for any commit after bc32a1f
If form S landed it needs a scoped re-review and then ONE human verify sweep. If it did
not, re-dispatch from correction-spec.md section 5.6 and the ledger entry headed
"SPEC TEST 5.3 DECIDED".

THEN. Task 15 records five minutes of real osu and needs the human. It must be recorded
through a CORRECTED stream. Task 16 refits the overlay's filter constants against it.

WHAT THE HUMAN MUST DO. Room lights ON, and sit at about 600 mm, no closer than 520. Both
are hard requirements found by measurement, not preferences. A verify sweep is nine dots
and about 30 seconds. They have already run well over a dozen sweeps, a tape session and an
IPD measurement, so ask for at most one thing at a time and say what it will decide.

LEARNED THE HARD WAY, all of it earned:
- Treat code quoted in the plan as a draft. Seven tasks found defects in plan-mandated code
  and every one was caught by testing rather than reading.
- When a number matters, ask where it came from. The display area was wrong for most of a
  session because 597 x 336 was the plan's own example value carrying a MEASURE YOURS
  comment, and it named a real but adjacent monitor.
- Amending a patch needs `git diff HEAD`, not `git diff`. See CLAUDE.md.
- Three refusals blocked plausible wrong answers: a partial-sweep fit, an anisotropic fit
  that looked good at 31 px, and a stale-geometry log. Do not relax them.
```
