# Paste this into the next session

```
Resume the Tobii ET5 gaze overlay project in /home/jason/Documents/tobii-eye-tracker.

Read CLAUDE.md and docs/RESUME-phase1.md first. Between them they have the design, the
measured hardware facts, the patch workflow, and the current task state. docs/wip/phase1-ledger.md
has the execution record if you need detail.

We are executing docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md using the
superpowers:subagent-driven-development skill, on branch feat/phase1-bringup. Tasks 1 and 2
are complete. Continue to the end of Phase 1 (task 16).

Use Opus 5 for both implementer and reviewer subagents. Do not downgrade to a cheaper model
to work around a 529 error; retry, or verify it yourself and say so. Do not close a task
without a review verdict, even if the reviewer goes silent.

Two things are waiting, in this order:

1. Task 3 is REOPENED with a confirmed Critical. scripts/spike-first-sample.sh kills the
   daemon at 15s, but the "failed to connect" diagnostic needs ~20s to appear (200 handshake
   steps x 100ms recv timeout), so the Windows-provisioning case produces the least useful
   message in the script. The same 15s window also blinds the validity check, because sample
   #500 lands at 15.06s at 33 Hz. Fix: sleep 15 -> sleep 22, plus an explicit
   "handshake step present but handshake complete absent" branch, a trap to clean up the
   temp log, and a bounded wait instead of the current unbounded one. Full detail is in
   docs/RESUME-phase1.md.

2. Task 4 is done and APPROVED, with a fix round committed on top. The quarantine at
   docs/wip/task4-0006-calibration-buffers.UNVERIFIED.patch.txt was resolved: the patch
   now lives at patches/0000-calibration-buffers.patch, renumbered from 0006 so it applies
   before the five patches that get authored on top of it. Nothing to re-dispatch.

Task briefs for 4 through 9 are already extracted in
.superpowers/sdd/2026-07-26-phase1-bringup-and-daemon/. Generate more with the skill's
scripts/task-brief.

Tasks 4 through 12 need nothing from me. Tasks 13 and 15 do: task 13 is a nine-point
calibration plus a replug experiment that answers whether calibration persists, and task 15
is five minutes of real osu to record a gaze trace. Tell me when you reach those.

Start by confirming the environment still works: ./scripts/build.sh should succeed, and
./scripts/check-device.sh should report PASS. Then fix task 3 and carry on.
```

## Why this prompt is shaped that way

It names the two files to read rather than restating their content, so the next session
gets the full picture without you pasting a wall of text. It states the model policy and
the no-silent-close rule explicitly, because both were learned the hard way and neither is
recoverable from the code. It gives the two waiting items in dependency order with enough
detail to act without reading everything first. And it ends with a cheap environment check,
because a stale nix store or an unplugged tracker would otherwise be discovered three
subagent dispatches later.
