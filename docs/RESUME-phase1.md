# RESUME HERE: Tobii gaze overlay, Phase 1

Read this first, then `docs/wip/phase1-ledger.md` for the full execution record. Last
updated 2026-09-01 during session 4.

## What this project is

An eye-tracking gaze overlay for OBS, so osu! stream viewers can see where the player is
looking. Linux, X11, KDE. Tobii Eye Tracker 5 (`2104:0313`), which has no official Linux
driver. The driver is `tobiifree`, a third-party reverse-engineered Zig implementation
vendored as a pinned submodule.

- **Spec:** `docs/superpowers/specs/2026-07-26-tobii-obs-overlay-design.md`, 13 sections
- **Plan:** `docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md`, 16 tasks
- **Normative visual constants:** `tools/derive_visual_constants.py`. Run it. It prints
  every number in spec section 9, and if it disagrees with the spec the script wins
- **Ledger:** `docs/wip/phase1-ledger.md`. It is the recovery map. Trust it and `git log`
  over any recollection

Phase 1 is bring-up and the daemon. Phase 2 is the OBS filter plugin, Phase 3 is the
verification harness. Neither is planned yet, deliberately, because both depend on Phase
1's measurements.

## Published

The repository is public at https://github.com/kymotsujason/tobii-gaze-linux under
**GPL-3.0**. That license is not a free choice: `tobiifree` is GPL-3.0, and every file
under `patches/` embeds excerpts of its source, so the patches are derivative works.
`README.md` carries the change notice GPL-3.0 section 5(a) requires.

`skin/` and `traces/` are gitignored deliberately. The first is third-party osu! art and
audio that cannot be redistributed here, the second holds gaze traces, which are personal
biometric data.

## Working method

`superpowers:subagent-driven-development`. Fresh implementer subagent per task, Opus
review after each, ledger updated continuously. Branch `feat/phase1-bringup`.

**Opus 5 for implementers AND reviewers.** Do not downgrade to dodge a 529. Retry, or
verify yourself and say so.

**No task closes without a review verdict.** Verifying yourself first is worth doing and
catches different defects faster, but a silent reviewer is not a clean one. Two tasks were
closed early on self-verification alone and both times the review then found something
self-testing structurally could not reach.

**Reviewers must write their verdict to a file.** In session 3 the subagent reply channel
dropped three consecutive reviewer messages while implementer messages arrived normally.
Have each reviewer write to `<workspace>/task-N-review-verdict.md` and return one word.
A file survives a lost message.

## Task status

| Task | State |
|---|---|
| 1. Repo skeleton, pinned vendor, build script | complete (`be827e7..5b075cb`) |
| 2. udev scripts | complete (`c7d1cf5`, `1fdb7a4`) |
| 3. Bring-up spike | complete (`ae218b6..19e5a6f`) |
| 4. P7 calibration buffers | complete (`60b3ae3..8de095d`) |
| 5. P4 force the display area | complete (`b029663`) |
| 6. P1 single USB owner | complete (`5f20579`) |
| 7. P2+P3 one write path | complete (`ed3374a..7e64687`) |
| 8. P6+P5 poll loop, unplug recovery | complete (`25932e6..84fc228`) |
| 9. P8+P9 status message, pending lifetime | complete (`dfe0ae0..54539ff`) |
| 10. `gaze-cal` protocol layer | complete (`c8e6cd8..a955357`) |
| 11. `gaze-cal` client | complete (`7dfac2a..1acfd89`) |
| 12. Display area readback gate | complete (`d432926..e6dc84a`) |
| 14. systemd user unit | complete (`ebf87fb..91520cb`), done out of order |
| 13. Calibration + persistence experiment | ran on hardware; it REVEALED THE DEFECT below rather than completing. Spec section 13 item 2 is not answerable as posed |
| **13b. Host-side gaze correction** | NEW, not in the plan. Form S complete (`f3f61e2..513da1b`, reviewed clean after one fix round). **Needs one human fit** |
| **15. Trace recorder** | code complete (`854f455..75f433e`, reviewed clean after one fix round). **Needs the human fit, then five minutes of real osu** |
| 16. Refit filter constants | blocked behind 15's recording, which produces its input |

Every task carries a review verdict. Twelve of the sixteen closed needed a fix round first.

## The finding that reshaped Phase 1

A real nine-point calibration was run on hardware and **had no measurable effect**: the
calibrated, daemon-restarted, power-cycled and blob-reapplied conditions all sat in a
250-285 px band alongside an **uncalibrated baseline of 250 px**.

The investigation, in `.superpowers/sdd/.../cal-investigation.md`, excluded every
alternative against data rather than argument. The device's display area, ray-plane
intersection and normalisation are provably exact (7.5e-05 over 425 frames) and its 3D
eye-position sensing is sound (reported IPD 65.56 mm sd 0.19 against a human-measured 65).
**The error is in the gaze direction: an isotropic, distance-invariant gain of about 1.18
plus an additive vertical offset.** The onboard calibration is live but its entire effect
is a 4.6 mm eye-origin translation, which cannot express a gain, and no protocol command
exists to set one.

So the device cannot be told to fix this, and Phase 1 grew Task 13b, a host-side
correction. Measured on hardware:

| | median error |
|---|---|
| raw device output | 250-285 px (5.5 to 6.3 degrees) |
| form H at the fitted position | 37 px, WITHIN one degree |
| form H after a real 112 mm head move | 69 px, outside |
| **form S, shipped** | verify-normal **33 px**, lateral2 **53 px** held out |

Spec test 5.3 decided against the head-aware form: **the scale centre is not the eye.**

The form S review (session 4) added one refusal to the fit. After the single refit that
spec 2.3 allows, a surviving point still past 3x the new median refuses the sweep with
`GZ_FIT_ERR_REFIT`. It closes the case where a second outlier on the other axis was
absorbed and dragged `gy` by 5.7 percent while passing the isotropy guard. Re-scored over
the seven recorded nine-point sweeps it fires on none of them, and only `verify-normal#1`
reaches the rejection path at all, so that evidence rests on one sweep. A refused fit costs
one 30 second sweep, an absorbed gain error would poison the Task 15 trace, so the guard
stays. `tools/score_correction.py` mirrors it.

## What the human still has to do

1. **Re-fit, because nothing works until you do.** The `correction.conf` on disk holds
   form H parameters whose offsets are eye-relative, so form S REFUSES it: reading it as
   form S would be wrong by about 90 px and would look like a working calibration. The
   refusal was measured against the real file in the session 4 review. Run
   `./scripts/fit-correction.sh` where you actually sit, lights on, about 600 mm back. It
   runs the fit sweep and then the verify sweep without moving, expecting 35 to 50 px and
   WITHIN ONE DEGREE, and the third sweep is optional. Re-fit whenever the seat changes
   for good.
2. **Task 15's recording**, five minutes of real osu, unblocked once form S is verified.
   `gaze-cal record traces/osu-YYYYMMDD.csv` records 300 seconds by default, refuses to
   run without a usable form S fit (pass `--raw` only for a smoke test, never for Task 16),
   and writes a copy of the fit beside the trace as `<path>.correction.conf`. The CSV has
   28 columns: the brief's 16 raw fields, six corrected fields (`nan` where the input eye
   is invalid), and the six calibrated eye origins in tracker mm. Read the "samples
   missed" line at the end. A recv carrying two gaze frames loses one to the client's
   single `latest` slot, which measured 0 missed on an idle desktop but is untested under
   osu plus OBS. Then run the brief's Step 3 python: median dt near 30.3 ms, invalid
   fraction in the low single digits.

## Operating requirements found the hard way

- **Room lights must be ON.** With them off this tracker returns zero valid frames at the
  top-left point. Three runs were wasted before this was found, and it matters because osu
  is often played in the dark. Phase 2 must surface gaze loss rather than freezing.
- **Sit at about 600 mm, no closer than 520.** Nearer than that the top row of the screen
  leaves the tracker's cone entirely, whatever the lighting.
- The ET5 **loses its display geometry on a power cycle**, reading back 4x4 mm, which is
  its reset state. The daemon's replay branch is therefore the critical path, not dead code.

Briefs for every task are extracted in the workspace as `task-N-brief.md`. Regenerate with
`scripts/task-brief PLAN_FILE N` from the skill directory.

## Immediate state, read before touching anything

`git status` shows `vendor/tobiifree` dirty and that is expected. `./scripts/build.sh`
leaves the patch series applied and staged inside the submodule.

**Recovery is always safe:** `./scripts/build.sh` resets the submodule to the pinned
commit and reapplies every `patches/*.patch`.

The only patch so far is `patches/0000-calibration-buffers.patch`, renumbered from `0006`
so it applies before the five patches that will be authored on top of it. Its
`driver/src/tracker.zig` edit looks out of place for a buffer-sizing task and is
deliberate: it bounds a client-reachable overflow into `out_scratch` that raising
`Client.buf` to 65536 would otherwise have widened. Do not delete it.

## The environment broke in session 3, and the fix is in the build script

Today's system upgrade took glibc from 2.43 to 2.44, and `/usr/bin/nix` now segfaults
before `main`. The Arch nix package links `libmimalloc.so.3`, which interposes
`malloc`/`free` process-wide, and glibc 2.44 frees a pointer mimalloc never allocated.
`nix-daemon` died the same way.

- A systemd drop-in with `LD_PRELOAD=/usr/lib/libc.so.6` fixes `nix-daemon`, and it is
  installed.
- The same preload **cannot** fix the CLI usefully. `LD_PRELOAD` is inherited by children,
  and `nix develop` spawns a nix-store bash linked against store glibc 2.42, which needs
  `GLIBC_PRIVATE` symbols that 2.44 no longer exports. **Do not export `LD_PRELOAD`
  globally**, it would break every nix-store binary.
- `scripts/build.sh` therefore probes whether `nix develop` actually runs, rather than
  trusting `command -v nix`, and falls back to `$ZIG`, then a store zig 0.15.x, then a
  system zig. It re-enables nix automatically once the distro rebuilds the package.
- **Subtlety worth keeping:** a zig from `/nix/store` stamps the store's glibc as the
  output binary's `PT_INTERP`, and that loader does not search `/usr/lib`. Building
  against system libusb links fine and then dies at exec. `ldd` hides this because `ldd`
  uses the system loader. The fallback pins libusb to the store whenever zig is a store
  zig.

Warm builds got faster as a side effect, 0.2 s against roughly 8 s, because the fallback
skips flake evaluation.

## Measured facts that corrected the spec

All of these were assumptions that measurement disproved. Do not re-derive them.

| Assumption | Measured reality |
|---|---|
| 133 Hz sample rate | **33.2 Hz.** `frame_counter` advances by exactly 4 (331/331), dt 30.27 ms, p10 29.85, p90 30.55. The sensor counts at 133 Hz internally and ships every 4th frame. tobiifree's own `sdk/src/gusb.ts:99` says "~33Hz" |
| 133 Hz recoverable? | **No.** Setting the two `0x04` bytes in the 20-byte subscribe payload to `0x01` produced ZERO frames. Not a decimation divisor. Only `stream_id` at bytes 9..10 is understood |
| Per-eye offset up to 45 px | **67 x 40 px**, 78 px combined. Spec section 8's fusion EMA is more load-bearing than described |
| Calibration blob about 4 KB | **Exactly 4096 max.** `cal_finish_blob_ptr()` returns `&out_scratch`, `[4096]u8` at `tobiifree_core.zig:346`, and `scratch_size()` is exported at `:351`. Bounds-check against 4096, never against a buffer size |
| Windows provisioning maybe needed | **Not needed.** The handshake completes in 4 steps on a factory device, 92.8% both-eyes-valid over 20 s, gaze in `[0,1]`, and every field offset of the 392-byte struct confirmed |
| `GazeSample` is 232 bytes | **392 bytes.** `ARCHITECTURE.md` is wrong |
| Zig 0.14+ per the README | **0.15.x only.** 0.14 fails on unmanaged `ArrayList`, 0.16 removes `std.process.args`, `std.posix.getenv` and `std.fs.cwd`, all used |

## Environment facts

- udev rule installed, device writable without root. `scripts/check-device.sh` verifies
  and detects bootloader mode (`2104:0102`). Exit contract: 0 runtime, 1 not enumerated,
  2 bootloader, 3 not writable
- Daemon socket: `/run/user/1000/tobiifreed/gaze.sock`
- **A client MUST send `subscribe` (`0x01` plus u32 LE 0, 5 bytes) on connect**, and on
  every reconnect, or it receives zero samples with no error
- `response` (0x02) frames prepend a one-byte `cmd_type`, so their body starts at offset
  6, not 5
- `~/.config/tobii.json` **now exists**, written during Task 5, and the 1500x1000 mm
  placeholder is gone. The device holds geometry built from that file. **Only `w_mm` 597
  and `h_mm` 336 are measured**, being the DP-1-2 active area recorded in CLAUDE.md.
  `z_mm`, `tilt`, `cx` and `cy` are byte-identical copies of the daemon's own
  `--init-config` template and are **NOT measurements**
- **Task 13 must measure the mounting parameters before calibration is trusted.**
  Calibration is computed in the display-area frame, so calibrating against an unmeasured
  `z_mm` and `cy` produces a plausible-but-wrong frame, which is the exact failure Task 5
  exists to escape. `--force-display-area` is how you correct it afterwards
- Display area persists on-device across daemon restarts, confirmed by readback rather
  than inference: a no-flag restart reports `device display: TL=(-299,346,0)
  TR=(299,346,0) BL=(-299,10,0)`, which is 598 x 336 mm. Persistence across a device
  power cycle is NOT proven, and `isReset()` exists because it does not always hold. Those
  corners are 597 x 336 mm, printed rounded to whole millimetres from 298.5 per side. A
  `{d:.3}` readback during Task 5 gave 597.000 x 336.000 exactly

## The patch workflow, which is subtle and cost three fix rounds

`vendor/tobiifree` is never permanently edited. `scripts/build.sh`:

1. Resets the submodule to the **outer repo's gitlink** via `git rev-parse
   :vendor/tobiifree` then `checkout --force --detach`. Not `reset --hard HEAD`, which
   reads the submodule's own HEAD. Not `reset --hard $PIN`, which rewrites an attached
   branch, and this submodule has a local `main`
2. `clean -fd`. Not `-fdx`, which destroys the Zig cache: 7.5 s cold against 0.18 s warm
3. Applies every `patches/*.patch` in filename order
4. `git add -A` inside the submodule, so a later `git diff` yields only new work

To add a NEW patch: run `build.sh`, edit inside `vendor/`, extract with
`git -C vendor/tobiifree diff > patches/NNNN-name.patch`, run `build.sh` again to prove it
applies from the pin, then commit the `.patch` to the outer repo.

To AMEND an existing patch the command is different and getting it wrong silently
destroys the patch: extract with `git -C vendor/tobiifree diff HEAD`. Plain `git diff`
yields only the new edits, because step 4 already staged the existing patch.

**Never `git checkout -- .` inside the submodule between steps**, and never commit inside
`vendor/`.

Verified idempotent from six adversarial states, and a failed mid-series apply does not
compound.

## Deferred minors for the final review

Recorded in the ledger with `minor (deferred)`. Currently: `install-udev.sh` prompts for
sudo before checking the rules file exists; `check-device.sh` exits 127 with no message if
`lsusb` exists but fails; the spike's `"not found"` grep is unambiguous only because
`socket_source.zig` is absent from `tobiifreed`'s module graph; the spike cites
`main.zig:559` for `while (!quit)` when the loops are at `:479` and `:569`; a SIGINT
during the spike's sleep orphans the daemon, which then holds the USB interface; the
spike's new handshake branch depends on `handshake step` being a `.debug` line;
`.gitignore` lacks `.direnv/`; `clean -fd` leaves files matching the submodule's own
ignore rules; `ws_server.zig` still has a `[4096]u8` client buffer and a silent
`buf_len = 0` wipe, so a full calibration blob stalls on the `--ws` transport;
`MAX_RESPONSE_PAYLOAD` and the driver test's `DAEMON_RESP_BUF` duplicate 8192 across
modules; `Server.init` builds a roughly 1 MiB temporary by value.

## Still unknown, and only answerable on hardware

1. Whether calibration survives a replug. Task 13's experiment decides whether
   `gaze-cal --apply-saved` is mandatory or merely harmless
2. Thin ring against wide soft ring, and then peak opacity, judged through OBS rather than
   a browser. Spec section 9 records the measured trade: the thin ring dominates on every
   metric but reads as a reticle rather than a cloud, and the user chose "cloud"
3. Every signal-processing constant is still fitted to a simulation. `beta` is declared
   unset in spec section 8 because the unit system changed to degrees. Task 16 refits
   against a real recorded trace
