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

FIRST THING TO DO. Form S of the correction landed at commit f3f61e2 and is UNREVIEWED.
Spec test 5.3 decided against the head-aware form: with a real 112 mm lateral head move,
form H measured 69 px and form S 52 px, so the scale centre is not the eye. Form S was then
refitted and re-scored from the recorded sweeps: verify-normal 33 px, and lateral2 53 px
held out rather than fitted. Hold-out over 12 ordered pairs gives form S 48 px against form
H's 52 px. make check exits 0 with 179 killed, 7 documented, 0 unexpected, verified by the
controller. Read task-13b-formS-summary.md, then dispatch a SCOPED RE-REVIEW of f3f61e2.
Two things for that reviewer. The existing correction.conf on disk is a form H fit and is
correctly REFUSED, since reading it as form S would be wrong by about 90 px and would look
like a working calibration; check the version-then-form locks and GZ_CORR_PARSE_STALE hold.
And there is one disclosed concern worth judging: a second outlier on the opposite axis can
be absorbed, dragging gy by 5.7 percent while passing both the outlier rule and isotropy. A
per-axis rule was tried and measured, and it would have refused the two sweeps that work, so
the rule was left as specified, pinned by a test, with damage bounded at 20 px median.

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
