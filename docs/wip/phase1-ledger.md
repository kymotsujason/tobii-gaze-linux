# SDD ledger, plan: docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md

Task 1: complete pending review (commit be827e7, branch feat/phase1-bringup)
Task 1: review found CRITICAL, build.sh `git checkout -- .` restores from INDEX,
  and the script's own `git add -A` makes the index diverge from HEAD after the first
  patch. Reproduced by controller: re-apply then fails. Fix: `git reset --hard HEAD`.
  Would have first surfaced at Task 5. Root cause: introduced by the plan self-review's
  own fix for the patch-extraction bug.
Task 1: minor (deferred): .gitignore lacks .direnv/ and OS cruft patterns.
Task 1: ruling, docs/ and tools/ in the initial commit are CORRECT (controller's
  out-of-band instruction; they predate Task 1). Not scope creep.
Task 1: ruling, check-device.sh finding DISMISSED; controller's review brief wrongly
  named a Task 2 file. Not an implementer defect.
Task 1: fix round 1/5 (commits bfb929d, 90130f2), reset --hard HEAD replaces
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
Task 1: re-review (Opus), all 5 round-1 findings ADDRESSED and independently
  reproduced (bug and fix both). Evidence judged sound: the throwaway-patch test does
  exercise the staged-index failure path.
Task 1: fix round 2/5 dispatched, IMPORTANT: reset --hard HEAD uses the submodule's
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
--- SESSION 3 START 2026-07-26 16:20 ---
ENVIRONMENT BREAKAGE (not a plan task, controller-fixed, commit ae218b6):
  ./scripts/build.sh segfaulted. Root cause found from the core dump, NOT ours:
  today's 13:50 glibc upgrade (2.43+r37 -> 2.44+r3) made /usr/bin/nix die with
  SIGSEGV in ELF constructors, before main. Stack: mimalloc _ZdaPv/free <-
  __newlocale <- _S_create_c_locale <- ios_base::Init <- ld-linux. nix links
  libmimalloc.so.3 (installed 02:41 with nix), which interposes malloc/free
  process-wide; glibc 2.44 frees a pointer mimalloc never allocated.
  PROOF: LD_PRELOAD=/usr/lib/libc.so.6 nix --version -> exit 0. Preloading
  libstdc++ instead -> still 139, so it is the C allocator, not operator delete.
  nix-daemon.service is also dead (systemctl: failed), so `nix develop` fails even
  with the CLI preload -> full repair needs a systemd drop-in (sudo, user's call).
  WORKAROUND SHIPPED: build.sh now probes `nix develop --command true` instead of
  trusting `command -v nix`, and falls back to $ZIG / store zig 0.15.x / system zig.
  SUBTLETY, cost one failed binary: a /nix/store zig stamps the STORE glibc as the
  output's PT_INTERP, and that loader does not search /usr/lib. Building against
  system libusb linked fine and then died at exec with "libusb-1.0.so.0: cannot
  open shared object file". ldd hid it (ldd uses the system loader). So the
  fallback pins PKG_CONFIG_PATH to the store libusb whenever zig is a store zig.
  Also: `bash -c 'cmd'` execs in place, so redirecting could not hide bash's
  "Segmentation fault" notice until `; exit $?` forced a fork.
  VERIFIED: build.sh exit 0, warm rebuild, idempotent across runs, submodule ends
  at d303e47 + 0006 staged; daemon binary runs and reaches handshake step 3;
  check-device.sh PASS exit 0.
  FLAGGED: build.sh is a Task 1 artifact changed outside the task loop. The final
  whole-branch review must cover this diff.
STALE BRIEFING CORRECTED: the handoff said Task 4's patch was quarantined at
  docs/wip/task4-0006-...UNVERIFIED.patch.txt. It is not: commit 60b3ae3 renamed it
  back to patches/0006-calibration-buffers.patch with a stronger bound (asserts
  against core.scratch_size(), not buf.len). So Task 4 is IMPLEMENTED AND COMMITTED
  but has NO REVIEW VERDICT. It needs review, not re-dispatch.
Task 4: REVIEWED, APPROVED (task-4-review-verdict.md), then a four-change fix round.
  RENUMBERED: patches/0006-calibration-buffers.patch is now
  patches/0000-calibration-buffers.patch. Every 0006 reference above is historical; the
  file has not existed under that name since this entry. Reason: 0006 was authored
  against the bare pin, but all five remaining patches (0001-0005) will be authored on
  top of it and would have been APPLIED before it. Five context inversions, removed by
  a rename.
  onResponse (main.zig) got the guard sendResult already had. Its payload_len is the
    device's TTP plen, bounded only by ACC_CAP = 2 MiB, and encodeResponse memcpys with
    no length check, so a frame declaring plen=100000 smashed an 8198-byte stack frame.
    Bound is MAX_RESPONSE_PAYLOAD = 8192, with the buffer defined in terms of it.
  sendResult's buffer shrank 70000 -> 4096 payload bytes. Measured in the ReleaseSafe
    binary: frame 0x111b8 (70072) -> 0x1048 (4168), and the per-call 0xaa undefined-fill
    memset shrank with it. The 70000 came from mirroring the 65536 inbound Client.buf,
    but inbound is capped by what a client may send and outbound by out_scratch at 4096.
    Plan text corrected so Tasks 5-8 do not inherit the conflation.
  Both rejection paths now send an error frame instead of returning silently, so a client
    that trips a guard fails fast rather than waiting on the Task 11 watchdog.
  25/25 driver tests (was 24). The new one feeds a 100000-byte response frame through
    feed_usb_in and asserts on_response receives it intact, proving the guard's
    precondition is real rather than assumed.
Task 5: complete (commit b029663, patches/0003-force-display-area.patch, review APPROVED
  after the one blocking finding was resolved). Spec compliance FULLY MET, all five steps,
  verbatim where the brief was verbatim.
  DEVIATION, reviewer-approved and reproduced rather than argued: the argument loop was
  restructured to push back the --ws lookahead. The pinned code did `continue` on a
  lookahead starting with '-', and its own comment admitted that was only safe while
  --init-config returned early. Without the fix, `tobiifreed --ws --force-display-area`
  silently leaves the flag unset, which would have shipped a no-op flag on a daemon whose
  entire purpose is preventing silent geometry errors. The reviewer lifted both loops into
  standalone programs and ran them under zig 0.15.2.
  EXACTLY TWO invocations changed meaning, both previously nonsense:
    --ws --init-config: pinned dropped --init-config and ran the daemon; now it runs
      initConfig and exits, which is plainly the intended reading.
    --ws --ws 1234: pinned kept port 7081; now sets 1234.
  No documented invocation changed, and the usage comment never listed --ws.
  BEHAVIOUR CHANGE the brief intended but did not spell out: a failed geometry write is
  now fatal on the GENUINE RESET path too, not only under the flag. Correct under spec
  section 11, since a device reporting under 50 mm is definitively wrong, but it changes
  the no-flag invocation.
  CONTROLLER-CONFIRMED ON HARDWARE: device readback after a full restart is
  TL=(-299,346,0) TR=(299,346,0) BL=(-299,10,0), so 598 x 336 mm really is stored on the
  device. Stronger than the config-load line, which only proves the file parsed.
Task 5: BLOCKING FINDING I-1, resolved by controller, no code change. docs/RESUME-phase1.md
  still said the config did not exist and the device held the 1500x1000 placeholder. Task 5
  made both false, and the accurate record lived only under .superpowers/, which is
  gitignored. A later session could have read RESUME, believed the mounting parameters were
  still unset, and run Task 13's calibration against unmeasured z_mm and cy. Fixed: RESUME
  now records what is measured (w_mm 597, h_mm 336) versus what is a template default
  (z_mm, tilt, cx, cy), and states that Task 13 must measure before calibration is trusted.
Task 5: PLAN DEFECT found by the reviewer and fixed by controller. The plan's Task 3 Step 1
  snippet proposed "cx": "center" and "cy": "bottom". evalAnchorExpr reads exactly ONE
  anchor character then an optional signed offset, so "center" hits 'e' and returns null,
  and ox_mm falls back silently to the DisplayArea default of -750. Both corrected to "c"
  and "b - 10", with the grammar documented beside the snippet so it cannot recur.
Task 5: minor (deferred): std.process.exit(1) on the new fatal path skips
  `defer transport.deinit()`, which sends vendor control 0x42 session-close
  (libusb_transport.zig:106-111). Only exit in main that abandons rather than closes the
  session. Probably harmless since the kernel releases the interface, but untested.
Task 5: minor (deferred to Task 12, which already edits this block): the genuine reset path
  now logs neither "forcing display area from config" nor "preserved from previous
  session", so it is identifiable only by the absence of both. The brief's verbatim Step 2
  code dropped the old "device display area looks reset, applying config" line.
Task 5: minor (noted): patch header index blob fd9e7e4 is the post-0000 blob, not the
  pinned blob. Harmless, git apply consults the hash only when context fails to match, and
  the reviewer proved context matches both alone and in the stack.
Task 5: doc re-review verdict = ALL REQUIRED ITEMS ADDRESSED. It verified the anchor
  grammar independently and reproduced the arithmetic: "c" gives ox_mm -298.5, and
  "b - 10" gives -h/2 - 10 = -178 so oy_mm = 10, which is EXACTLY the BL=(-299,10,0) the
  device reports. It also confirmed "b - 10" is the literal shipped template value at
  main.zig:548, so the plan snippet now matches what --init-config writes.
  It found three further issues, all fixed by controller:
  - RESUME called the readback "598 x 336" beside a bullet asserting 597 is measured,
    reading as a 1 mm discrepancy in the one document meant to settle geometry. It is
    {d:.0} rounding of 298.5 per side. Now states 597 x 336 printed rounded, and records
    the {d:.3} readback of 597.000 x 336.000.
  - RESUME's status table still said tasks 5-9 not started, contradicting a bullet 70
    lines below. Table now lists Task 5 complete separately.
  - MOST VALUABLE, and it went beyond what it was asked: my plan to "raise the unmeasured
    mounting parameters at Task 13" DOES NOT WORK. task-13-brief is extracted from the
    plan's Task 13 section, which never mentions the display area, z_mm, or measuring
    anything. And Task 12's gz_display_verify compares the device against the config's
    OWN want values, so it passes cleanly with an unmeasured z_mm of 0. The obligation was
    invisible exactly where it mattered. FIXED: the plan's Task 13 now opens with an
    explicit PRECONDITION block naming what to measure, saying to apply it with
    --force-display-area, and stating that gz_display_verify cannot catch this.
Task 6: complete (commit 5f20579, patches/0001-single-usb-owner.patch, review APPROVED).
  Spec compliance FULLY COMPLIANT, five of five steps, one necessary deviation.
  PLAN DEFECT, the significant finding of this task: the plan's own handshake code is a
  REGRESSION, not a weaker fix. usbResume cleared only the request flag, so a command
  issued inside the USB thread's 0.2 ms parking sleep read a leftover usb_paused of true,
  took that ack, and re-parked the thread before it ever reached tracker.poll(). That
  thread is the only reader of the IN endpoint, so a pipelining client starved it and no
  response was ever consumed. MEASURED on hardware, three log files:
    pre-burst.log  unpatched binary        4000 of 4000 served
    post.log       plan's handshake        4199 forwarded, 200 routed, 3935 table full
    accept.log     shipped fix             1232 forwarded, 1232 routed, 0 warnings/errors
  The plan's own Step 4 test at 10 ms spacing cannot see it, because the main thread idles
  for 10 ms of every 10.03 ms and the USB thread always gets a turn.
  FIX: a poll progress gate. usbPause waits until usb_polls has moved past the value
  captured at the last resume, which proves the reader took a real turn instead of merely
  observing a flag transition. The reviewer established this as a happens-before edge, not
  a narrowed window: reaching fetchAdd requires first executing usb_paused.store(false),
  and usbPause's acquire load synchronises with that release, so usb_paused can only
  become true again through a fresh entry into the parking branch. Instrumentation over
  5160 recorded pauses: stale_ack true ZERO times, spins==0 ZERO times. The implementer
  reported 400 pauses and understated its own evidence by an order of magnitude.
  Plan text corrected so nobody ships the original.
  Reviewer confirmed independently: all seven forwardable commands are inside the paused
  region and subscribe/disconnect are filtered ahead of it; defer usbResume cannot be
  skipped since every post-pause return is a plain return with no process.exit on the
  path; addPending now precedes transport.send with takePending on send failure, and
  request_id comes from the core's monotonic takeReqId which never returns 0, so the
  failure branch cannot evict another client's entry; every store is .release and every
  load .acquire, including the reverse edge that publishes the main thread's command-time
  writes before polling resumes; deadlock is structurally impossible because nothing the
  USB thread does waits on the main thread.
  RACE NOT REPRODUCED, and the implementer said so plainly. Reviewer accepted the fix
  anyway on two grounds: the calibration half is not a narrow window at all, since
  driveStateMachine runs drainReads on the main thread for the whole command while the USB
  thread polls, both feeding one accumulator; and the request/response half is
  unsynchronised access to two tables from two threads, which is undefined behaviour
  whether or not the window is hit on this hardware.
Task 6: IMPORTANT I1, deferred to TASK 7 by the reviewer's own scoping. A failed pause is
  advisory: when either budget is exhausted usbPause logs and returns, and the caller
  proceeds as if it owned the endpoint. Harmless for get_display_area, but for
  start_calibration/finish_calibration/cal_apply it means a calibration blob assembled
  from an interleaved accumulator, and Task 13 writes those blobs to the device. Fix is to
  have usbPause return a bool and let the three state-machine branches refuse.
Task 6: IMPORTANT I2, MUST land in TASK 8's brief. Per-command latency is now ~30 ms, one
  gaze interframe, because the pause waits for a real poll boundary. readCommands
  dispatches a whole buffer fill before returning, so drainGaze does not run for the whole
  batch. The gaze ring is 64 entries, about 2 s at 33.2 Hz, so a batch over ~70 commands
  wraps the ring and drainGaze then reads slots the USB thread already overwrote. The
  flush that follows writes the backlog to every subscriber at once, and broadcastGaze
  calls removeClient on ANY write error including error.WouldBlock, disconnecting a
  healthy client. Reproduced three times. Before this patch a 500-command batch took
  100 ms and could not overflow a 2 s ring, so this patch made it reachable.
  NOTE: the EAGAIN half is Task 7's Produces line; the ring overflow is Task 8.
Task 6: minor M1 (fold into Task 7 with I1): usbResume captures the poll counter BEFORE
  clearing the request flag, which is exact only when the preceding pause was acked. After
  a budget exhaustion the thread is still running and can bump usb_polls between those two
  lines. Precondition is an already-logged failure, so no new harm. One line: skip the
  capture, or use usb_polls -% 1, when the pause was not acked.
Task 6: minor M2 (fold into Task 7): the usbPause doc comment says a worst-case poll is
  ~101 ms. The drain loop runs up to seven tryRecv calls, so the true bound is ~107 ms.
  Conclusion unaffected, the budget is still 9x the bound, measured max was 121 of 5000.
Task 6: minor M3 (noted only): usb_polls is a u32 compared for equality, wrapping after
  about four years at 33.2 Hz.
Task 6: CARRY TO TASK 9: the core's own pending table silently evicts slot 0 on overflow
  (tobiifree_core.zig:1056-1066, MAX_PENDING = 32, no log line). The progress gate keeps at
  most one request outstanding so forwardCommand cannot reach it today, but Task 9's
  pending-entry lifetime work needs to know.
Task 6: UNEXPLAINED, by implementer and reviewer both: two clients pipelining 500 commands
  each is not reliably serviced, one client sometimes getting only its first 32 commands
  read. One run completed 1000 of 1000 in 30.6 s, so it is a servicing delay, not a hang.
  Pause and routing logic is NOT implicated: forwarded equalled routed in every run,
  cumulative 1232 of 1232, zero table overflows. Task 8 should look.
