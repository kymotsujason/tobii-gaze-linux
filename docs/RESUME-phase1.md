# RESUME HERE — Tobii gaze overlay, Phase 1

Written 2026-07-26 at a deliberate stopping point (context limit). A fresh session should
read this file first, then `progress.md` in the same directory for the full ledger.

## What this project is

An eye-tracking gaze overlay for OBS, so osu! stream viewers can see where the player is
looking. Linux, X11, KDE. Tobii Eye Tracker 5 (`2104:0313`), which has no official Linux
driver — the driver is `tobiifree`, a third-party reverse-engineered Zig implementation
vendored as a pinned submodule.

- **Spec:** `docs/superpowers/specs/2026-07-26-tobii-obs-overlay-design.md` (13 sections)
- **Plan:** `docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md` (16 tasks)
- **Normative visual constants:** `tools/derive_visual_constants.py` (run it; it prints
  every number in spec section 9, and if it disagrees with the spec the script wins)
- **Ledger:** `progress.md` beside this file. It is the recovery map. Trust it and
  `git log` over any recollection.

Phase 1 is bring-up and the daemon. Phase 2 is the OBS filter plugin, Phase 3 is the
verification harness. Neither is planned yet, deliberately: they depend on Phase 1's
measurements.

## Working method

`superpowers:subagent-driven-development`. Fresh implementer subagent per task, Opus
review after each, ledger updated continuously. Branch `feat/phase1-bringup`.

**User's explicit model preference: Opus 5 for implementers AND reviewers.** Do not
downgrade to Sonnet or Haiku to dodge a 529 — retry, or verify yourself and say so.
Many Opus agents hit `529 Overloaded` in the previous session.

**Hard rule learned the hard way: no task closes without a review verdict.** I closed
Task 2 and Task 3 early on my own verification and both times the review then found
something my testing structurally could not reach (a USB PID I never enumerated, a
timeout I never waited for). Verify yourself first — it catches different defects faster
— but never treat a silent reviewer as a clean one.

## Task status

| Task | State |
|---|---|
| 1. Repo skeleton, pinned vendor, build script | **complete** (`be827e7..5b075cb`) |
| 2. udev scripts | **complete** (`c7d1cf5`, `1fdb7a4`) |
| 3. Bring-up spike | **REOPENED — has a Critical, fix specified below** |
| 4. P7 calibration buffers | **IN FLIGHT, INCOMPLETE — see below** |
| 5-9. Remaining daemon patches | not started, briefs staged |
| 10-12. `gaze-cal` protocol, client, display gate | not started |
| 13-16. Calibration, systemd, trace, refit | not started, need the human |

Briefs for Tasks 4-9 are already extracted in this directory as `task-N-brief.md`.
Generate more with `scripts/task-brief PLAN_FILE N` from the skill directory.

## IMMEDIATE STATE — read before touching anything

`git status` shows `vendor/tobiifree` dirty and that is expected: `./scripts/build.sh`
leaves the patch series applied and staged inside the submodule.

Task 4 is **done, reviewed, APPROVED, and a fix round is committed**. Its patch is
`patches/0000-calibration-buffers.patch`, renumbered from `0006` so that it applies before
the five patches that will be authored on top of it. The `driver/src/tracker.zig` edit in
it is deliberate: it bounds a client-reachable overflow into `out_scratch` that raising
`Client.buf` to 65536 would otherwise have widened. Do not delete or re-dispatch it.

**Recovery is safe:** `./scripts/build.sh` resets the submodule to the pinned commit and
reapplies every `patches/*.patch`.

## Task 3's Critical, fully specified

`scripts/spike-first-sample.sh` kills the daemon at 15 s. The `failed to connect`
diagnostic branch is **unreachable** in that window:

- `tracker.zig:250` loops 200 steps. A silent device returns `.recv` every step.
- Each `.recv` costs one `drainReads`, whose first read is `recvTimeout(buf, 100)`
  (`libusb_transport.zig:86`).
- 200 x 100 ms = **20 s** to reach the timeout. The 15 s kill stops at ~step 150.

So the log never contains `failed to connect`, and the operator gets "none of the known
failure patterns matched". **That is the Windows-provisioning case — spec section 13
item 1, the project's single open question — producing the least useful message in the
script.**

Same 15 s window also blinds the validity check: `main.zig:68` logs samples `<=3` then
every 500th, and at 33.2 Hz sample #500 lands at 15.06 s, just past the kill. So only
samples #1-#3 are ever inspected, covering ~90 ms. The committed baseline's own `gaze #1`
is `vL=4 vR=4` (warm-up), so a correctly seated user can be told to check their seating.

**Fix, one line kills both:** `sleep 15` -> `sleep 22`. Also add an explicit branch that
diagnoses correctly rather than waiting for the daemon's own timeout:

```bash
if grep -q "handshake step" "$LOG" && ! grep -q "handshake complete" "$LOG"; then
    echo "FAIL: TTP handshake started but never completed (device silent; possible Windows provisioning requirement)"
    MATCHED=1
fi
```

Two more Important items on the same script: the temp log leaks on every path (`mktemp`
sits above the binary check, so even the "run build.sh first" exit leaks — needs
`trap 'rm -f "$LOG"' EXIT` and `mktemp` moved down), and `wait "$PID"` is unbounded
(`quit` is a non-atomic `bool` at `main.zig:33` read in `while (!quit)` at `main.zig:559`
under `ReleaseSafe`, where hoisting is legal; bound it with `kill -0` polling then
`kill -9`).

## Measured facts that corrected the spec

All of these were assumptions that measurement disproved. Do not re-derive them.

| Assumption | Measured reality |
|---|---|
| 133 Hz sample rate | **33.2 Hz.** `frame_counter` advances by exactly 4 (331/331); dt 30.27 ms, p10 29.85, p90 30.55. Sensor counts at 133 Hz internally, ships every 4th frame. tobiifree's own `sdk/src/gusb.ts:99` says "~33Hz" |
| 133 Hz recoverable? | **No.** Setting the two `0x04` bytes in the 20-byte subscribe payload to `0x01` produced ZERO frames. Not a decimation divisor. Only `stream_id` at bytes 9..10 is understood |
| Per-eye offset up to 45 px | **67 x 40 px** (78 px combined). Spec section 8's fusion EMA is more load-bearing than described |
| Calibration blob ~4 KB | **Exactly 4096 max.** `cal_finish_blob_ptr()` returns `&out_scratch`, `[4096]u8` at `tobiifree_core.zig:346`. Bounds-check against 4096, not buffer size. Note `sendResult` currently sizes `HEADER_SIZE+1+8192`, twice what the core can produce |
| Windows provisioning maybe needed | **Not needed.** Handshake completes in 4 steps on a factory device, 92.8% both-eyes-valid over 20 s, gaze in `[0,1]`, 392-byte struct layout and every field offset confirmed |
| `GazeSample` is 232 bytes | **392 bytes.** `ARCHITECTURE.md` is wrong |
| Zig 0.14+ per the README | **0.15.x only.** 0.14 fails on unmanaged `ArrayList`; 0.16 removes `std.process.args`, `std.posix.getenv`, `std.fs.cwd`, all used. `nix develop` pins the author's exact toolchain and yields 0.15.2 |

## Environment facts

- **nix 2.35.1 installed and working**, flakes enabled. Store initialised via
  `sudo nix-store --init` (Arch's package does not create it). No group needed — the
  daemon socket is `srw-rw-rw-`.
- **udev rule installed**, device writable without root. `scripts/check-device.sh`
  verifies; it now also detects bootloader mode (`2104:0102`) and has a documented
  exit contract (0 runtime, 1 not enumerated, 2 bootloader, 3 not writable).
- Daemon socket: `/run/user/1000/tobiifreed/gaze.sock`
- **A client MUST send `subscribe` (`0x01` + u32 LE 0, 5 bytes) on connect** or it
  receives zero samples with no error. Every reconnect too.
- `response` (0x02) frames prepend a one-byte `cmd_type`, so their body starts at
  offset 6, not 5.
- `~/.config/tobii.json` does not exist; the daemon wrote a **1500x1000 mm placeholder**
  geometry to the device. Task 5's `--force-display-area` exists to correct this, and
  correct geometry must precede calibration.
- The device currently reports its stored area as preserved, so display area persists
  on-device across sessions.

## The patch workflow, which is subtle and cost three fix rounds

`vendor/tobiifree` is never permanently edited. `scripts/build.sh`:
1. Resets the submodule to the **outer repo's gitlink** via
   `git rev-parse :vendor/tobiifree` then `checkout --force --detach` (NOT
   `reset --hard HEAD`, which reads the submodule's own HEAD, and NOT
   `reset --hard $PIN`, which rewrites an attached branch — `vendor/tobiifree` has a
   local `main`)
2. `clean -fd` (NOT `-fdx`, which destroys the Zig cache: 7.5 s cold vs 0.18 s warm)
3. Applies every `patches/*.patch` in filename order
4. `git add -A` inside the submodule, so a later `git diff` yields only new work

To add a patch: run `build.sh`, edit inside `vendor/`, `git -C vendor/tobiifree diff >
patches/NNNN-name.patch`, run `build.sh` again to prove it applies from the pin, commit
the `.patch` to the outer repo. **Never `git checkout -- .` inside the submodule between
steps** and never commit inside `vendor/`.

Verified idempotent from six adversarial states, and a failed mid-series apply does not
compound.

## Deferred minors for the final review

Listed in `progress.md` with `minor (deferred)`. Currently: `install-udev.sh` prompts for
sudo before checking the rules file exists; `check-device.sh` exits 127 with no message
if `lsusb` exists but fails; the spike leaks its temp log; the spike's `"not found"` grep
is unambiguous only because `socket_source.zig` is absent from `tobiifreed`'s module
graph; `.gitignore` lacks `.direnv/`; `clean -fd` leaves files matching the submodule's
own ignore rules.

## Still unknown, and only answerable on hardware

1. Whether calibration survives a replug (Task 13's experiment decides whether
   `gaze-cal --apply-saved` is mandatory or merely harmless)
2. Thin ring versus wide soft ring, and then peak opacity, judged through OBS rather
   than a browser. Spec section 9 records the measured trade: the thin ring dominates on
   every metric but reads as a reticle rather than a cloud, and the user chose "cloud"
3. Every signal-processing constant is still fitted to a simulation. `beta` is declared
   unset in spec section 8 because the unit system changed to degrees. Task 16 refits
   against a real recorded trace
