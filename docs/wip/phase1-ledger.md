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
Task 7: review APPROVED (commit ed3374a, patches/0002-write-path.patch). Spec FULLY
  COMPLIANT, one necessary deviation. Reviewer independently extracted the pin to a scratch
  tree, applied all four patches in filename order, and confirmed every resulting file is
  byte identical to the vendor tree, with no fuzz. Exactly one posix.write on a client fd
  remains, inside flush.
  SECOND PLAN-MANDATED CODE DEFECT, derived independently by the reviewer and by me. The
  brief's enqueue tests capacity as (out_len - out_off + msg.len) but memcpys at out_len.
  Those differ by exactly out_off. Reviewer's concrete reachable state: out.len 131072,
  out_off 60000, out_len 130950, msg.len 170 gives 71120 <= 131072 so the test passes, the
  reset branch does not fire because out_off != out_len, and the copy writes 48 bytes past
  the end. Reachable via one stalled client that keeps sending commands: sendToClient
  enqueues responses unconditionally so out_len climbs, and flush writing tens of KB does
  short-write on AF_UNIX. In ReleaseSafe a remote client can panic the daemon; in
  ReleaseFast it corrupts the next Client's fd or buf_len. Shipping the brief verbatim
  would have been a CRITICAL. The shipped compaction version was proven overflow-free on
  every path.
Task 7: MY INSTRUCTION WAS WRONG, adjudicated in the implementer's favour. I told it to use
  usb_polls -% 1 on the unacked path. Under an equality gate ANY baseline other than the
  counter's true current value satisfies the exit condition immediately, so -% 1 deletes
  the wait instead of lengthening it, and +% 1 fails identically. Reading the counter after
  the store is the only correct fix, and it is what shipped.
Task 7: IMPORTANT I1, MEASURED not argued, fix round dispatched. main.main reserves
  0x6020b8 = 6,299,832 bytes = 6.008 MiB of an 8 MiB stack. sizeof(Server) is 0x3004a8 =
  3.001 MiB, and the residual after subtracting TWO Server-sized objects is 5,992 bytes in
  BOTH this build and the Task 6 baseline, which is only consistent with the frame holding
  two. Confirmed in the disassembly: back-to-back memcpy calls with $0x3004a8 in %edx at
  main.main+0x2cdf and +0x2cf3, the second targeting main.server. So the error-union return
  materialises Server three times at startup. Headroom fell 6.0 MiB -> 1.99 MiB in one
  patch, and MAX_CLIENTS = 32 would need ~12 MiB and fail to start with a stack-probe
  SIGSEGV before logging anything.
  FIX CHOSEN: give Server.init an out-parameter so the .bss global is built in place. This
  needs no ruling from the user because the plan never specified how init returns, unlike
  the 131072 buffer size, which is plan-mandated and stays.
Task 7: IMPORTANT I2, recorded in the plan rather than fixed. ws_server.zig still writes
  bare from the USB thread with no queue and no lock and still removeClients on
  error.WouldBlock, so after this task onResponse has one locked branch and one unlocked
  branch, both on the USB thread, and --ws silently opts into a data race plus the very
  EAGAIN disconnect this task removes. Off by default and unused by this project's client,
  so Phase 1 is not blocked. Plan's Task 7 section now carries the dependency.
Task 7: reviewer named the single line preventing a real deadlock: server.zig:156 releases
  the lock before forwardCommand. Without it the USB thread blocks in onResponse on a lock
  the main thread holds while the main thread waits for that same thread to acknowledge a
  pause. Because both pause loops are budgeted, reintroducing the bug would present as 2 s
  per command and a refusal rather than a hard hang. A comment was requested at that site.
Task 7: the brief's Step 4 stall test is a COIN FLIP, not a weak test. The disconnect
  threshold is 66299 bytes, exactly 167 x 397 with remainder 0, so the socket buffer holds
  precisely 167 frames, which at 33.2 Hz take 5.030 s. The brief's 5 s stall misses it by
  about one frame, 30 ms. The pre-patch binary passes it. The 20 s run is the only
  load-bearing evidence: old disconnects at 66299 bytes, patched keeps the client and
  drains 66696 bytes, remainder 0 mod 397, 168 whole frames.
Task 7: minor M4 (noted): 0002 was authored against a tree with 0003 applied, so its hunk
  headers are wrong for the position it applies at and it lands at offsets -1 and -13.
  Verified to resolve from the pin by exact context with no fuzz, and the context line
  server.acceptClients() is unique in the file, so risk today is nil.
Task 7: minor M5 (deferred): a wedged-but-live client now holds one of 16 slots forever and
  drops every gaze sample, with no idle or backpressure timeout. Intended consequence of
  the fix and the right trade, but a new resource-exhaustion shape nothing bounds.
Task 7: minor M6 (CARRY TO TASK 9): response routing is still keyed on a raw fd. This task
  narrows the damage from any fd to any live client, but a queue-full disconnect inside a
  command is a NEW way for the fd to close mid-flight, so the recycled-fd window is
  slightly wider than before.
Task 7: fix round 1/5 (4 addressed, 0 open; commits af11327..7e64687). Re-review verdict
  ALL FIXES ADDRESSED, no new breakage.
  I1 RESOLVED BY MEASUREMENT: main.main's frame fell from 0x6020b8 = 6,299,832 bytes to
  0x2778 = 10,104, and zero 0x3004a8 memcpys remain anywhere in the binary. Controller
  verified both numbers independently by objdump. Headroom 1.99 MiB -> 8.38 MiB. Fix was
  Server.init(self: *Server, ...) !void filling the .bss global in place, with fields
  assigned individually and the client array cleared by a slot loop, so neither a stack
  temporary nor a 3.1 MB rodata constant is emitted. out stayed at the plan-mandated
  131072.
  FIELD AUDIT, the risk of dropping a struct literal: all 8 Server fields confirmed
  written, and every `try` in init precedes the first field write, so a failing init leaves
  the global wholly untouched and deinit is registered only after the catch. No
  partial-construction window.
  THE 10,104 FIGURE EXPLAINED PROPERLY, and the implementer's own account was wrong. It
  is not init's locals (path_buf + sockaddr.un is only 622 bytes). It is exactly 4096 + 16:
  STACK COLORING. loadDisplayArea's short-lived buf: [4096]u8 used to overlap the two dead
  3.1 MB Server temporaries, whose lifetimes were disjoint from it. Remove the temporaries
  and it needs its own slot. The old 5,992 residual was 4,096 + 1,896 scalars. Both builds
  already inlined Server.init and loadDisplayArea into main.
  Also confirmed: .bss unchanged at 5,355,760 and .rodata FELL 64 bytes, so no 3.1 MB
  constant was traded for the removed copies.
Task 7: minor M1 fixed with proto.Err in daemon_protocol.zig (failed = 1, usb_busy = 2).
  Wire value 1 preserved for every pre-existing failure: the only encodeError call site in
  the tree is main.zig:292 via sendResult's !ok path, and ws_server.zig emits no error
  frames at all.
Task 7: the patch now touches a THIRD file, daemon_protocol.zig. Re-reviewer judged no
  ordering risk: 0000 touches main/server/core/tracker, 0001 and 0003 touch main.zig only,
  and nothing else in the series touches daemon_protocol.zig, so 0002's third file lands on
  pristine pin content. Err also belongs there, beside encodeError and Srv.err.
Task 7: CARRY TO TASK 11 (client): proto.Err is enum(u32) so the width matches the wire,
  but nothing exports it, no extern and no generated header, so the C client must duplicate
  1 and 2 by hand with nothing keeping them in sync. Also, Err is EXHAUSTIVE, so a Zig-side
  @enumFromInt on a code the daemon adds later panics in safe modes; a trailing `_` would
  make it a safe decode target.
Task 7: note: driver/build.zig roots its tests at tobiifree_core.zig and
  tobiifree_decode.zig, so daemon_protocol.zig is NOT compiled by the 25 driver tests. It
  compiles only via the daemon build.
Task 7: complete (commits e93ae62..7e64687, review APPROVED, 1 Important fixed, 1 Important
  recorded as a plan dependency, 6 minors deferred)
Task 8: review CHANGES REQUIRED, one CRITICAL. Commits 25932e6 (patches 0004, 0005) and
  694f406 (test harness). Spec MET except Step 5, a physical replug, which needs a human.
  TEN deviations, all justified, FOUR confirmed as real defects in the brief's own code:
    D3 NULL dereference: LibusbTransport.deinit nulls usb_handle, and Zig does not assign
      on the error path of `transport = init() catch continue`, so transport keeps the
      nulled value. The brief never calls tracker.deinit(), so tracker.connected stays true
      and poll() does not bail, reaching libusb_bulk_transfer(NULL, ...). Confirmed a
      segfault. Verified fixed: 10 consecutive failed reopens, no crash.
    D4 gaze callback never re-registered: Tracker.init sets .gaze_cb = null and still logs
      "connected, streaming gaze", so a replug would report success and deliver ZERO
      samples forever.
    D5 response hook never re-installed: core.set_hooks treats null as leave-unchanged, so
      queryDisplayArea's captureResponse replaces only the response hook and nobody
      restores it. onResponse is the sole caller of takePending, so every routed response
      would be dropped and pending would fill to 64.
    D6 unconditional geometry replay would overwrite good device geometry from a possibly
      stale config on every replug.
  CRITICAL C1, the one nobody could have hit yet: reconnect() installs onResponse BEFORE
    the geometry replay, and the replay calls setDisplayArea -> queryDisplayArea ->
    set_hooks(captureResponse), which is never restored. So the replay branch UNDOES the
    D5 fix. main() has these in the opposite order. Under --force-display-area the branch
    condition is always true, so after any replug the daemon logs "USB reconnected", gaze
    streams perfectly, and every command silently gets no reply until "pending table full".
    Task 13's calibration is entirely add_calibration_point, so it would hang with no error
    the client can see. One line moved. Fix round dispatched.
Task 8: PATCH SPLIT PROVEN COMPLETE by content rather than line count. The reviewer git
  archived the pin d303e47 to a scratch tree, applied all six patches in filename order,
  and diff -r against vendor/tobiifree excluding .git, zig-out and caches: EXIT 0, BYTE
  IDENTICAL. Independently, I confirmed the modified-file set and the covered-file set are
  the same six files. tracker.zig genuinely needed no change.
Task 8: CORRECTION to my own earlier note: the shipped gaze ring is 256 entries, about
  7.7 s, not 64 and 2 s. And the ring overrun is GENUINELY fixed, not merely slowed. Three
  separate changes doing different jobs: the per-command drain prevents the measured cause,
  256 entries only widens the margin, and the detector changes the failure MODE so a lapped
  reader jumps to w-(N-1) and counts the loss instead of serving stale slots as fresh. The
  arithmetic checks out both ways: the old -252 backwards jumps are 63 x 4, exactly one lap
  of 64, and the scratch build's 245 reported drops match the client's +984 forward jump,
  which is 246 x 4.
Task 8: LOG VOLUME defect found by the reviewer, fix dispatched. At the 2 s backoff cap the
  daemon emits 1800 ERROR lines per hour, forever, while unplugged. That is the flood the
  brief wanted avoided, merely slowed down.
Task 8: FOUR MORE UNTESTED GAPS beyond the three the implementer named. (1) Bootloader-mode
  re-enumeration: LibusbTransport.init only looks for 2104:0313, but CLAUDE.md records
  2104:0102 as the bootloader PID, so a tracker returning in FBL mode retries forever at
  the cap and says nothing actionable. (2) Unplug during a calibration command, which is
  exactly what Task 13 does, and where a fatal lands in drainReads on the MAIN thread under
  an acknowledged pause rather than in the USB thread's poll(). (3) Unplug with a WS client
  attached, so failPending's ws.sendToClient path has never run. (4) Hours rather than
  seconds; the longest measured absence is 15.5 s.
Task 8: 4x4 mm ANOMALY, daemon code paths RULED OUT by the reviewer. decode_display_area
  hands the payload to tlv.Reader.readPoint3d, which demands the exact prolog tag 0x031f41
  then three Q42 reads, so garbage, truncation, a stale accumulator or a mis-routed
  response would all fail with WrongTag and log "decode failed", not yield a clean
  symmetric rectangle. The orelse fallback would have logged all-zero corners. So the bytes
  were a WELL-FORMED DEVICE RESPONSE, and the anomaly is device-side. It first appeared
  after a session containing eight reset_tobii runs. Instrumentation dispatched: hex-log
  the raw payload, length and request id whenever decoded corners trip isReset().
  Separately, a real latent hazard was found that does NOT explain this one but is being
  closed anyway: captureResponse stores a POINTER into the core accumulator whose lifetime
  ends at the next feed_usb_in, drainReads keeps feeding, and feed_usb_in compacts acc_buf.
  It works today only because try_recv_fn's 1 ms timeout means the response is the last
  feed before the next 30 ms gaze frame.
Task 8: PHYSICAL TEST CHECKLIST produced for the user, 10 items, in section 8 of the
  verdict file. Item 4 is load-bearing: whether a replug logs "preserved across reconnect"
  or "replaying display area after reconnect" decides whether the ET5 keeps geometry across
  a power cycle, and therefore whether the replay branch is dead code or the critical path.
  C1 must be fixed BEFORE that test or item 7 fails for an unrelated reason.
Task 8: fix round 1/5 (6 addressed, 0 open; commits 0c151a5..84fc228). Re-review verdict
  ALL FIXES ADDRESSED, no new breakage.
  C1 fixed and PROVED THROUGH THE EXACT BRANCH, which is what makes this convincing: the
  round-1 binary answered 0/70 commands after a reconnect with 6 "pending table full", and
  the fixed build answers 70/70 twice with 0 overflows. set_hooks now sits after the whole
  block containing BOTH queryDisplayArea call sites (tracker.zig:106 in Tracker.init and
  :138 in setDisplayArea), and the re-reviewer confirmed nothing after it reaches either.
  I1 fixed: a replay failure now tears down, keeps device_present false and retries, so
  device_present.store(true) is unreachable except through failure == null. No spin,
  because sleepUntilQuit(backoff_ms) is the first statement of the loop.
  Log volume: 4 failure lines per 120 s outage and a hard ceiling of 60/hour, against 1800
  before. Past 60 s the cap is 30 s and the log is gated by a timer rather than the attempt
  count, so a long absence cannot regress to a flood.
  Fix 5 chose COPYING the response into a 4096-byte buffer over stopping the drain, which
  holds regardless of how many feed_usb_in calls follow. The over-length guard returns
  without setting captured_len, so an oversized reply degrades to "no response" rather than
  a partial decode. That matters because plen is device-controlled up to ACC_CAP = 2 MiB.
  captured_payload is gone from the tree entirely.
  Instrumentation is bounded: fires only on corners.isReset() using the identical @abs
  formula as isReset itself, prints at most 128 bytes into a 256-byte buffer, and is
  reachable at most twice per connect, never in the retry loop.
Task 8: PATCH COMPLETENESS verified two independent ways after the implementer disclosed
  that its first extraction produced 142 lines instead of 382. Controller: git archive of
  the pin, apply all six patches, diff -r against vendor/tobiifree = BYTE IDENTICAL.
  Re-reviewer, by content rather than line count: 0004 still carries round-1's RecvResult
  union, bytesOf and the fatal field, and 0005 still carries GAZE_RING_SIZE = 256 and
  fn pollWait, none of which appear in this round's delta. No cross-leak between the two
  extraction paths.
  ROOT CAUSE PROPAGATED TO CLAUDE.md: build.sh ends with `git add -A`, so a plain
  `git diff` on a SECOND round against the same patch yields only the new delta. Amend with
  `git diff HEAD`, or `git diff HEAD -- <paths>` when a task splits patches by path. This
  trap has now nearly shipped broken patches twice, at 14 lines instead of 24 and 142
  instead of 382.
Task 8: physical checklist corrected by the re-review and written to
  docs/physical-test-checklist.md, which is TRACKED, unlike the verdict file it came from.
  Log strings changed with Fix 3, so items 2, 3, 4, 7 and 8 were reworded, and the item 3
  threshold dropped from "above about 40 lines" to "above about 5".
Task 8: CARRY TO TASK 9, from the re-review: when applySavedCalibration is filled in, note
  it runs AFTER set_hooks. caResponseHook save/restores hook_response
  (tobiifree_core.zig:2026,2067) so cal_apply itself is safe, but any NEW helper that calls
  set_hooks directly is not.
Task 8: still open, recorded not fixed: bootloader PID 2104:0102 re-enumeration is
  unhandled, and reconnect() has no cross-event rate limit so a flapping cable emits about
  5 lines per flap.
Task 8: complete (commits 7e64687..84fc228, review APPROVED after one Critical fixed,
  1 Critical + 1 Important resolved, physical replug outstanding on a hardware dependency)
Task 9: review CHANGES REQUIRED, no Critical, two Important. Commit dfe0ae0, patches
  0007 and 0008. Spec compliance MET on all five steps, all four carried items genuinely
  done. Controller verified byte-identical reconstruction from the pin with all eight
  patches before dispatching the review.
  P9 RACE DEMONSTRATED, NOT ARGUED: 21 misrouted replies in 2000 before, 0 in 2000 after.
  Slot-plus-generation keying judged SOUND rather than window-moving: the discriminator is
  Client.gen drawn from Server.slot_gen[i], which lives OUTSIDE Client and outlives its
  occupants, every access is under Server.lock, the counter skips 0 on wrap, and aliasing
  would need 2^32 accepts on one slot while a stale ref survives. refForFd still looks up
  by fd but runs on the main thread inside dispatchCmd, and the main thread is the only
  thread that accepts, so no new client can inherit the fd during that window.
  Server.sendToClient was DELETED outright, so no socket reply can be addressed by fd
  number any more, which is stronger than a check.
  The implementer disclosed that its default test mode never exercises the generation
  check, because the hold counter keeps the slot occupied so B lands elsewhere, and added
  a `sub` mode where a gaze broadcast to A's closed socket forces removeClient and frees
  the slot. The guard then fired 7 times, always gen N vs N+1 on the same slot. The
  reviewer called that out approvingly: the mechanism, not the aggregate.
  HEAD-OF-LINE fix confirmed correct where the rejected design was not: dispatch is
  independent of the read, a failed read only sets eof and the buffer drains on its own
  terms, so the strand case cannot occur BY CONSTRUCTION. The zero-timeout condition
  exists (pendingWorkLocked consulted under the lock in pollWait) and cannot livelock
  because progress is guaranteed each pass. 15.12 s -> 0.50 s for the second client's
  first reply, 500/500 both ways, strand 200/200.
  BACKPRESSURE threshold derivation confirmed: 131072 / 397 = 330 frames / 33.2 Hz =
  9.94 s. Wedged subscriber dropped at 10021 ms, idle unsubscribed survived 45 s.
  WIRE FORMAT JUDGED SAFE TO FREEZE for the C client: [u8 0x04][u32 LE 3][u8 present]
  [u8 cal][u8 version] is three uint8_t fields, no padding on any ABI, no endianness
  question inside the payload, status is always the first frame on a fresh connection, and
  it is delivered to unsubscribed clients too. ONE RULE FOR TASK 10: require
  payload_len >= 3 and read the version at payload offset 2, which pins the only field a
  future shape change must not move.
Task 9: IMPORTANT 1, fix dispatched, and worse than the implementer's own concern said. A
  hold that never resolves does not merely pin a slot, it SPINS THE MAIN LOOP AT 100% OF A
  CORE. server.zig:268 refuses to reap an eof client while holds > 0, and pollWait
  registers POLL.IN for every client including eof ones, so a closed socket is permanently
  readable-at-EOF and poll returns immediately every iteration. Normally that window is one
  USB round trip, about 30 ms. If the reply never arrives it is PERMANENT: readCommands
  skips the read because eof is set, nothing dispatches, nothing reaps, and only failPending
  or a restart clears it. Trigger: a short-lived CLI client sends add_calibration_point,
  or any command the device declines to answer, then exits. This project exists to not
  perturb a machine running osu! at 360 Hz, so a permanently hot core is the one cost it
  cannot pay, and it is reachable from exactly the Task 12 and 13 tooling.
Task 9: IMPORTANT 2, fix dispatched, and a tripwire working as designed. This task
  introduced a CROSS-THREAD READ of `subscribed`, the one Client field deliberately left
  outside the lock. flush now reads it at server.zig:342 and flush is reached from the USB
  thread via sendToRef and setStatus, while dispatchCmd still writes it lock-free under the
  comment I had Task 7 add, which asserts both reader and writer are the main thread. That
  comment is now FALSE. The practical harm is one byte with no tearing on x86-64, but the
  stale invariant is how the next lifetime bug gets written. The Task 7 tripwire is
  precisely what made this detectable.
Task 9: minor, and it is a NUMBER so it matters here: the implementer's "holds are bounded
  at one per client by the progress gate" is NOT established. The gate proves the USB
  thread completed a poll since the last resume, not that the previous request was
  ANSWERED. A device that never answers accumulates one pending entry and one hold per
  command up to MAX_PENDING. The core's own 32-entry eviction is a second generator of
  permanently unresolved entries, so the two COMPOUND rather than being independent.
Task 9: minor: stall_since_ms uses wall clock, so an NTP step can disconnect a healthy
  subscriber early or postpone the timeout indefinitely.
Task 9: minor: PATCH NAMES NOW MISDESCRIBE CONTENTS. 0008-pending-entry-lifetime carries
  P8's emission, head-of-line blocking and backpressure, because path-based splitting could
  not express the brief's split when both concerns cross both daemon files. The split
  itself is sound: the dependency runs ONE WAY only (0008 needs STATUS_SIZE, encodeStatus
  and pending_evictions from 0007, never the reverse), which matches filename order, so the
  stack applies cleanly and 0008 is independently revertible while 0007 alone is not.
  Rename dispatched, because "revert the status patch" would otherwise pick the wrong file.
Task 9: minor for TASK 13: saveCalibration sets calibration_applied = true even on the
  branch where the blob was too large to save for replay, so a later reconnect would
  advertise cal=1 with nothing replayed. Unreachable today because calApply already refuses
  anything past scratch_size() = 4096 = saved_cal.len, but Task 13 writes the real blobs.
Task 9: MAX_CMDS_PER_PASS = 8 judged defensible and less arbitrary than the implementer
  admitted. It does NOT protect the gaze ring, since forwardCommand drains the ring after
  every single command, so ring overrun is bounded independently. What 8 buys is a fairness
  bound of about 8 x 30 ms x (clients - 1) before another client's first reply, which
  matches the measured 0.50 s for two clients almost exactly. No cliff on either side.
  Honest framing: bounded by measurement, not derived from a rate.
Task 9 fix round: patches renamed to describe their contents.
  0007-device-status-message.patch  -> 0007-protocol-status-and-eviction-counter.patch
    driver/src/daemon_protocol.zig, driver/src/tobiifree_core.zig.
    Srv.status 0x04, encodeStatus, PROTOCOL_VERSION, STATUS_SIZE, non-exhaustive Err,
    and the core's pending_evictions counter.
  0008-pending-entry-lifetime.patch -> 0008-daemon-routing-fairness-and-backpressure.patch
    applications/tobiifreed/src/main.zig, applications/tobiifreed/src/server.zig.
    P9 slot+generation routing, the slot hold and its deadline, P8's emission,
    MAX_CMDS_PER_PASS, and the backpressure timeout.
  Dependency still runs one way only, 0008 on 0007, matching filename order.
Task 9 fix round: the eof spin was real and measured. One half-closed client holding an
  unanswered reply took the daemon from 0.0% to 92.5% of a core, permanently. Fixed by
  dropping eof fds from the poll set (0.4%) plus a 5s hold deadline that reaps the slot,
  measured at 5.02s. Needed a throwaway instrumented binary, since the DEVICE ANSWERS
  EVERY forwarded command it was given, including add_calibration_point outside a
  calibration session, which replied in 0.02s. That is worth knowing for Task 13.
Task 9: fix round 1/5 (7 addressed, 0 open; commits 9153da6..54539ff). Re-review verdict
  ALL FIXES ADDRESSED, no new breakage. Method was strong: the re-reviewer reconstructed
  the pin TWICE, once with the patch set before the fix and once after, and diffed the
  results, so it judged exactly what this round changed rather than the cumulative patch.
  Fix 1 both halves confirmed. Dropping POLL.IN for eof fds loses nothing an eof peer can
  still send, and the buffered-command case is not stranded because pendingWorkLocked
  forces timeout = 0. Losing POLLOUT costs at most one 50 ms tick of flush latency.
  Fix 2 confirmed NOT a deadlock: the subscribe branch returns before forward_fn, so the
  new lock scope is three lines and never spans forwardCommand.
  Fix 4 confirmed properly monotonic: std.time.Instant.now() is CLOCK.BOOTTIME on Linux,
  which no NTP step can move, and it was applied to BOTH the stall clock and the eof
  deadline.
  Fix 6 came out stronger than asked: Target became a union(enum), so a socket target
  carries no fd field at all and the compiler enforces it, rather than a comment.
  Fix 5 rename verified by direction: 0008 uses proto.STATUS_SIZE, proto.encodeStatus and
  core.pending_evictions; 0007 references no daemon symbol at all and is a pure rename with
  ZERO content lines changed.
Task 9: the "device answered every command" finding STRENGTHENS Fix 1 rather than weakening
  it, and the re-reviewer's reasoning is worth keeping. Device silence was never the only
  generator of an unresolved hold: the core's 32-entry seq table discards an evicted
  request's response BEFORE pendingTake, so the hold is never released even though the
  device answered; USB loss short of a full reconnect does the same; a reply slower than
  the deadline is the third. More decisively, the SPIN half needs no unanswered reply at
  all, since any eof client free-ran the loop while it lingered, which is the ordinary
  "send a batch then close" path. That half measured 92.5% -> 0.4%, against 0.4% -> 0.3%
  for the deadline. So the finding lowers the priority of the deadline, not the
  justification. And "the device answered every command in this session" is not "the device
  answers every command".
Task 9: NO INSTRUMENTATION LEAKED, checked beyond my own TOBII_TEST grep: no getenv,
  std.process, _TEST, instr, tobiifreed-instr, no_reply or drop_reply anywhere in either
  patch or the working tree. The only getenv in the daemon is two pre-existing HOME lookups.
Task 9: CARRY TO TASK 11, important for the client design: the daemon's guarantee is now
  "a reply within 5 s of half-close, or never", so the client needs a READ TIMEOUT, not a
  blocking read.
Task 9: 5 s deadline kept. The drop is safe rather than merely unlikely: sendToRef
  re-resolves the slot under the lock and compares gen, and acceptClients bumps slot_gen[i]
  BEFORE writing the new Client literal, so no two occupants of a slot ever share a
  generation. 5000 ms against 8 x 30 ms x 15 = 3600 ms is the right order. One term is
  understated: forwardCommand calls usbPause, whose budget is 1 s, so one command hitting
  that ceiling blows past 5 s, but that only happens when the USB thread has stopped
  polling, which is the reconnect path, and reconnect runs failPending and answers every
  entry anyway, so the gap closes itself.
Task 9: WIRE FORMAT STILL FROZEN-SAFE after the fix round. daemon_protocol.zig is
  byte-identical across the fix diff. The only behavioural deltas touch the VALUE of the
  existing cal byte, never a message type, field order, width or endianness, and both make
  cal=0 where the daemon cannot back a cal=1 claim.
Task 9: complete (commits 7b090b7..54539ff, review APPROVED after one fix round,
  2 Importants resolved, 5 minors resolved, 3 recorded)
ALL DAEMON PATCH TASKS COMPLETE. patches/ holds eight patches, 0000 through 0005 plus 0007
  and 0008, and they reconstruct the vendor tree byte-identically from the pin d303e47.
  Tasks 10 onward build the C client.
Task 10: implemented, commit c8e6cd8, gaze-cal/src/proto.{c,h} plus tests and Makefile.
  DONE_WITH_CONCERNS. 41 tests on gcc 16.1.1 with -Wall -Wextra -Werror, again under ASan
  and UBSan, again on clang 22.1.8 with -Wconversion -Wsign-conversion -Wshadow
  -Wcast-align -Wpedantic. `make mutate` breaks the implementation 25 ways and the suite
  catches all 25 with 0 unexpected survivors. Verified live against the real daemon and
  tracker: 265 gaze frames read in 37-byte chunks, 2922 partial parses, 0 desyncs, every
  frame_counter delta exactly +4, 33.11 Hz. nm shows memcpy as proto.o's ONLY undefined
  symbol, so no allocation, no I/O, no CLI dependency.
Task 10: FOURTH SET OF PLAN DEFECTS, three in one task.
  (1) LOAD-BEARING FOR TASK 12: the brief said display_area frames are 9 doubles. THE
      DAEMON NEVER EMITS TYPE 0x03 AT ALL. A get_display_area reply comes back as a
      RESPONSE (0x02) with cmd_type 0x02 and a 164-byte RAW TTP body. Confirmed live, not
      inferred. proto now treats 0x03 as a known type with no defined shape: bounded,
      consumed, handed to the caller to ignore rather than treated as desync. Task 12 is
      built entirely on reading the display area back, so it must know this.
  (2) gz_encode_cmd as quoted had a LIVE integer overflow: `cap < 5 + payload_len` wraps
      for payload_len near SIZE_MAX and admits a SIZE_MAX memcpy. Reproduced under ASan as
      negative-size-param: (size=-1). Fixed by bounding against GZ_MAX_PAYLOAD before any
      arithmetic.
  (3) The brief's Makefile did not build: SRC listed five files that do not exist until
      Task 11. Now $(wildcard src/*.c) with .DEFAULT_GOAL := test, since there is no main()
      to link yet. TASK 11 SHOULD FLIP the default goal back to $(BUILD)/gaze-cal.
Task 10: status uses len >= 3 rather than the brief's len == 3, following the brief's prose
  over its code. == 3 would turn an appended status field into a desync a client cannot
  distinguish from corruption, at exactly the moment PROTOCOL_VERSION exists to prevent it.
Task 10: CARRY TO TASK 11: GZ_MAX_PAYLOAD is 65536 as specified though the daemon's real
  ceilings are 8193 and 4097, so Task 11 needs a 64 KB read buffer to guarantee forward
  progress.
Task 10: CLAUDE.md corrected by controller on two counts the implementer flagged: the
  1500x1000 placeholder is gone and the device holds the real 597x336, and the
  display_area-frame myth is now recorded as a myth.
Task 10: review APPROVED, no Critical, two Important, fix round dispatched.
  All four deviations INDEPENDENTLY CONFIRMED. The display_area one was proven statically,
  not just live: daemon_protocol.zig has exactly four server encoders
  (encodeGaze/Status/Response/Error) and Srv.display_area reaches encodeHeader NOWHERE in
  driver/ or applications/tobiifreed/. So the brief was wrong twice over, since
  `len == 9*sizeof(double)` would reject nothing today and reject every real frame if 0x03
  ever shipped.
  The reviewer also recomputed all 23 GazeSample offsets BY HAND from the Zig extern struct
  (total 392, unfiltered at 376) and diffed all 22 GZ_BIT_* against tobiifree_core.zig:
  1093-1114 and every opcode against Cmd/Srv: exact. No test covers the bit values, so that
  hand-check is currently the only evidence for them.
  Offset-6 and validity are genuinely load-bearing: the offset-5 mutant fails at
  test_proto.c:117 and the inverted-validity mutant at test_proto.c:456.
Task 10: IMPORTANT 1: proto.h does not compile as C++. g++ rejects _Static_assert outright
  and there are no extern "C" guards, so Plan 2's OBS plugin, the named consumer of this
  exact file, cannot include it, and patching only the assert would leave the declarations
  mangled and unlinkable against a C-built proto.o.
Task 10: IMPORTANT 2: GZ_MAX_PAYLOAD is exported without a GZ_MAX_FRAME, so a frame the
  parser calls well-formed can need 65541 accumulator bytes while Task 11's gz_client_feed
  returns RECONNECT on overflow. A 64 KiB client would reconnect-loop forever on a frame
  proto told it was valid.
Task 10: honesty correction: "the suite catches all 25" counts an ALLOW_SURVIVE mutant as
  caught. Truth is 24 caught, 1 documented survivor, 0 unexpected. Four further mutants
  survive that the set does not contain, so the set is meaningful but incomplete.
Task 10: CARRY TO TASK 11, from the reviewer: 0 means incomplete and -1 means desync, so
  `if (r <= 0)` is a BUG; f.body points into the caller's buffer and DANGLES across
  compaction; size the input buffer >= 65541 or fix Important 2; gz_frames_dropped cannot
  tell counter rollover (about a year at 33.2 Hz) from a counter RESET on reconnect, which
  would report about 1e9 dropped, so reset the state on reconnect; and restore
  .DEFAULT_GOAL := $(BUILD)/gaze-cal.
Task 10: PLAN CORRECTED for Task 12, which was built on a false premise. Its brief assumes
  nine f64 arrive on the wire. They do not. The readback is a response 0x02 with cmd_type
  0x02 and a ~164-byte RAW TTP TLV body, so before gz_corners_to_rect has any input at all
  Task 12 must port decode_display_area (tobiifree_core.zig:445: skip a 2-byte prolog, then
  three readPoint3d reads of Q42 fixed point) into C. That means a small TLV READER THE
  PLAN NEVER BUDGETED FOR. Plan text updated.
