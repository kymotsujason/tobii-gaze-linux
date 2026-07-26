# SDD ledger — plan: docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md

Task 1: complete pending review (commit be827e7, branch feat/phase1-bringup)
Task 1: review found CRITICAL — build.sh `git checkout -- .` restores from INDEX,
  and the script's own `git add -A` makes the index diverge from HEAD after the first
  patch. Reproduced by controller: re-apply then fails. Fix: `git reset --hard HEAD`.
  Would have first surfaced at Task 5. Root cause: introduced by the plan self-review's
  own fix for the patch-extraction bug.
Task 1: minor (deferred): .gitignore lacks .direnv/ and OS cruft patterns.
Task 1: ruling — docs/ and tools/ in the initial commit are CORRECT (controller's
  out-of-band instruction; they predate Task 1). Not scope creep.
Task 1: ruling — check-device.sh finding DISMISSED; controller's review brief wrongly
  named a Task 2 file. Not an implementer defect.
Task 1: fix round 1/5 (commits bfb929d, 90130f2) — reset --hard HEAD replaces
  index-relative checkout; clean -fd not -fdx (controller correction mid-round,
  -fdx destroyed the zig cache every run); submodule-init guard; skin/ gitignored.
SPIKE RESULT (Task 3 question, answered early): ET5 streams gaze on Linux with NO
  Windows provisioning. Handshake completes in 4 steps. Eye detection works:
  92.8% both-eyes-valid over 20s, gaze in [0,1], struct offsets confirmed correct.
SPEC DEFECT FOUND: sample rate is 33.2 Hz, NOT the 133 Hz in spec section 1.
  frame_counter advances by exactly 4 on every sample (331/331), dt 30.27ms
  (p10 29.85, p90 30.55). Device counts at 133 Hz internally, delivers every 4th.
  tobiifree's own sdk/src/gusb.ts:99 says "~33Hz"; the 133 figure came from Tobii
  marketing via web search. Affects spec sections 1, 8, 9, 10, 12.
MEASURED: per-eye L-R offset 67 x 40 px (78 px combined). Spec section 8 estimated
  "up to 45 px". The fusion EMA is more load-bearing than described.
EXPERIMENT (controller, reverted): subscribe payload bytes 6 and 15 are both 0x04 and
  frame_counter advances by 4, so hypothesised as a decimation divisor. Set both to
  0x01, rebuilt, measured: ZERO frames. Not a simple divisor; the edit breaks the
  subscription. CONCLUSION: 33 Hz is the delivered rate. Unlocking 133 Hz would need
  the 20-byte payload decoded properly (only stream_id at bytes 9..10 is understood).
  Out of scope for Phase 1; record as possible future upside.
Task 1: re-review (Opus) — all 5 round-1 findings ADDRESSED and independently
  reproduced (bug and fix both). Evidence judged sound: the throwaway-patch test does
  exercise the staged-index failure path.
Task 1: fix round 2/5 dispatched — IMPORTANT: reset --hard HEAD uses the submodule's
  own HEAD, not the outer gitlink. Controller reproduced silent wrong-base build.
  Opus found the likely trigger: build.sh's own `git add -A` leaves the submodule index
  staged, so one stray commit inside vendor/ bakes patches into HEAD, and build.sh's
  comment tells people to run git there. Fix: reset to `git rev-parse :vendor/tobiifree`
  with a loud warning on divergence. Plus enforce vendor/PINNED_COMMIT, anchor /skin/.
Task 1: minor (deferred): dropping clean -x means files matching the submodule's own
  .gitignore (build/, node_modules/, *.o, /result, .direnv) survive; a future patch
  creating such a path would fail re-apply with "already exists". Unlikely for Zig
  source patches. Recorded, not fixed.
Task 1: minor (deferred): implementer's round-2 timing claim elapsed_warm=0s is not
  credible; cache preservation verified independently via `git clean -ndx` instead.
Task 1: fix round 2 committed 6ddfc38. Controller verified the CODE directly: it is the
  corrected version (checkout --force --detach, cat-file -e precheck, PINNED_COMMIT
  warns not exits) even though the implementer's summary message described the
  pre-correction plan. Artifact right, report stale.
Task 1: timing re-measured properly: cold 7.469s, warm 0.176s (42x). Justifies clean -fd.
Task 1: FOUR defects total in one 3-line block; TWO were in controller-authored fixes.
  checkout -- . reads index; clean -fdx kills the build cache; reset --hard HEAD reads
  the submodule HEAD not the gitlink; reset --hard $PIN rewrites an attached branch.
  All four silent. Root cause: every git verb's default is wrong for "immutable base".
Task 1: controller verification (Opus close-out never delivered; agent idled twice):
  - all 4 round-2 findings confirmed ADDRESSED in committed d3d3459
  - idempotent from 6 adversarial states (clean / staged deletion / untracked file /
    unstaged modification / twice in a row / PINNED_COMMIT absent) -- submodule ends
    clean at d303e47 with 0 dirty entries in every case
  - staged DELETIONS are cleared by checkout --force --detach, not just additions
  - partial mid-series apply does NOT compound: run 2 resets and fails identically,
    one marker not two
  - no fifth defect found on any testable axis
Task 1: complete (commits be827e7..5b075cb, 5 commits, controller-verified)
Task 2: complete (commit c7d1cf5, controller-verified; Opus reviewer idled without a
  verdict, 529s widespread this session).
  Verified: check-device.sh set -e + failing-grep handled via || guard (tested with a
  bogus VID -> correct message, exit 1). install-udev.sh tested with a stubbed sudo:
  missing rules file exits 1 safely; happy path is cp/reload/trigger then the replug
  instruction naming check-device.sh. Ordering correct.
Task 2: minor (deferred): install-udev.sh prompts for sudo before checking the source
  rules file exists, so a missing file costs a password prompt first.
Task 2: minor (deferred): check-device.sh conflates a missing lsusb binary with a
  missing device; both report "tracker not enumerated". Disclosed by implementer.
Task 2: minor (deferred): check-device.sh matches only PID 0313, not the 0102 bootloader
  PID that the udev rule also covers. Fine for normal use.
Task 2: REOPENED. Controller closed it prematurely; the Opus review landed afterwards
  with a real IMPORTANT finding. Fix round queued behind Task 3 (no concurrent
  implementers on one branch).
  IMPORTANT: check-device.sh greps only 2104:0313, but the udev rule it verifies also
    covers 2104:0102 (bootloader). A tracker in FBL mode is accessible yet reports
    "tracker not enumerated" -- wrong diagnosis in the one case that matters, and this
    project can reach FBL deliberately (assets/flash_firmware.c, extract_firmware.c).
    Fix: grep -iE '2104:(0313|0102)' and echo which PID matched.
  MINOR (bundle with the above, all one-liners in the same 2 files):
    - install-udev.sh: preflight [ -f "$RULES" ] with an actionable message before sudo
    - check-device.sh: `| head -n1` after the grep; two matching devices currently make
      sed emit "001\n003" and produce a nonsense path (fails closed, baffling message)
    - install-udev.sh: scope the trigger (--subsystem-match=usb
      --attr-match=idVendor=2104) and add `udevadm settle` to avoid a race with
      check-device.sh
  Reviewer confirmed correct: [ -w ] honors the uaccess ACL not just MODE=0666, so the
    check survives a future tighten to 0660; zero-padding matches lsusb output; reload
    before trigger before replug is the right order.
Task 3: complete pending review (commit f973ecc, spike PASSES). Implementer added the
  validity distinction (streaming-with-eyes vs streaming-blind) and a 4th diagnostic
  branch beyond the brief; verified all 7 branches via synthetic logs built from log
  strings read out of the daemon's real source. Skipped brief Step 1 (tobii.json) per
  controller instruction; device currently holds the 1500x1000mm placeholder geometry,
  which Task 5 exists to correct.
Task 3: minor (deferred): the "not found" grep is unambiguous only because tobiifreed
  does not link usb_source.zig/socket_source.zig (they belong to tobiifree-overlay).
  A future patch could break that silently.
FACT for Task 4, now proven not estimated: the calibration blob is hard-capped at 4096
  bytes. cal_finish_blob_ptr() returns &out_scratch, declared [4096]u8 at
  tobiifree_core.zig:346. So Task 4's session_out=8192 (needs 4130) and Client.buf=65536
  (needs 4101) are both correct with headroom. Codex's "~4KB" estimate was right.
Task 3: controller verification while review outstanding:
  - all 5 grep strings confirmed present in the daemon's real source
  - "not found" ambiguity CLEARED more strictly than the implementer claimed: only 2
    occurrences exist (socket_source.zig:44, libusb_transport.zig:43), and
    socket_source.zig is absent from tobiifreed's build.zig module list, which wires
    only core/tracker/libusb_transport/daemon_protocol/server/ws_server/main
  - cadence trap avoided correctly: validity check greps whatever samples were logged
    rather than requiring #500, so it is not flaky at 33 Hz
  - set -uo pipefail deliberately WITHOUT -e is correct here, since diagnostic greps
    are expected to fail
  - diagnostics are independent checks with a MATCHED fallback, not an if/elif chain
Task 3: minor (deferred): temp log leaked -- LOG=$(mktemp) with no trap on EXIT.
  11 stray /tmp/tmp.* files present.
Task 3: open question for the reviewer: exit 0 for both "streaming with eyes" and
  "streaming but blind". Defensible (the gate is about streaming) but arguably blind
  streaming deserves a distinct status.
Task 2: fix round 1 (commit 1fdb7a4) EXCEEDS the brief. Controller-verified all 6 paths
  with a stubbed lsusb: runtime->0, bootloader->2 (correct FBL diagnosis, the Important
  finding), both present->0 with runtime taking precedence, neither->1, lsusb-lists-it-
  but-node-absent->1, real hardware->0. Solved the multi-device case better than the
  specified `head -n1`: uses ${match%%$'\n'*} AND announces the truncation. Also fixed
  the deferred lsusb-missing conflation, anchored the sed (kills the greedy .* concern),
  and added a documented exit-code contract.
Task 2: minor (deferred): a BROKEN lsusb (present but failing) exits 127 with no
  message, violating the script's own 0/1/2/3 contract and failing silently.
  Fix: usb=$(lsusb) || { echo "FAIL: lsusb failed"; exit 1; }
Task 2: complete (commits 5b075cb..1fdb7a4, controller-verified; Opus reviewer delivered
  the finding list, fix implementer's report never arrived but the code is verified)
Task 3: complete (commit f973ecc, controller-verified; Opus reviewer idled without a
  verdict). 1 minor deferred (temp log leak, no trap on EXIT).
Task 3: REOPENED (second premature close by controller; review arrived after closure).
  CRITICAL, controller-confirmed by arithmetic: the "failed to connect" branch is
    UNREACHABLE at 15s. tracker.zig:250 loops 200 steps; a silent device returns .recv
    every step; each .recv costs one drainReads whose first read is recvTimeout(buf,100)
    (libusb_transport.zig:86). 200 x 100ms = 20s, so the spike's 15s kill stops at
    ~step 150 and the log never contains "failed to connect". That is the
    Windows-provisioning case -- spec section 13 item 1, the project's one open
    question -- and it currently yields "none of the known failure patterns matched".
  IMPORTANT: validity check is blind at 33 Hz. main.zig:68 logs samples <=3 then every
    500th; #500 lands at 15.06s, just past the kill. So the only samples the validity
    grep ever sees are #1-#3, covering ~90ms. The baseline's own gaze #1 is vL=4 vR=4
    (warm-up), so a correctly seated user can be told to check their seating.
  IMPORTANT: temp log leaks on every path; mktemp is above the binary check so even the
    "run build.sh first" exit leaks. Need trap 'rm -f "$LOG"' EXIT and mktemp moved down.
  IMPORTANT: wait "$PID" is unbounded. quit is a non-atomic bool (main.zig:33) read in
    while (!quit) (main.zig:559) under ReleaseSafe where hoisting is legal; if it fires,
    wait blocks forever and cat "$LOG" never runs -> zero output. Bound with kill -0
    polling then kill -9.
  FIX THAT KILLS TWO AT ONCE: sleep 15 -> sleep 22. Plus an explicit
    "handshake step present but handshake complete absent" branch, which diagnoses
    correctly rather than relying on the daemon reaching its own timeout first.
  Reviewer confirmed sound: PASS-as-FAIL and FAIL-as-PASS both impossible (~70x margin);
    SIGTERM handlers make kill/wait reap cleanly; server.zig:42 unlinks a stale socket so
    no AddressInUse false-FAIL; exit 0 for blind streaming is RIGHT (vL=4 with nobody
    seated is correct behaviour, and non-zero would break headless use).
  Reviewer corrected the implementer's report: usb_source.zig does NOT contain "not
    found" text, only socket_source.zig does -- matches what I found independently.
  Fix queued behind Task 4 (no concurrent implementers).
CARRIED TO TASK 4 (sent to implementer mid-flight): sendResult sizes HEADER_SIZE+1+8192,
  twice the 4096 the core can produce. Bounds check must assert <=4096, not buffer size.
--- SESSION STOP 2026-07-26 (context limit). See RESUME.md beside this file. ---
Task 4: IN FLIGHT, INCOMPLETE. patches/0006-calibration-buffers.patch written but
  UNVERIFIED and UNCOMMITTED. vendor/ has uncommitted edits including tracker.zig, which
  is surprising for a buffer task and should be checked. Recovery: ./scripts/build.sh
  resets vendor to the pin and reapplies patches/, so either verify 0006 and finish, or
  delete it and re-dispatch Task 4 from its brief. Do not trust it unreviewed.
Task 3: still REOPENED. Critical fully specified in RESUME.md. Fix is sleep 15 -> 22
  plus an explicit handshake-incomplete branch, a trap for the temp log, and a bounded
  wait. Was queued behind Task 4.
