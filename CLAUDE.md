# Tobii ET5 gaze overlay for OBS

An eye-tracking overlay that draws where the player is looking into an OBS scene, so osu!
stream viewers can see their gaze. Linux, X11, KDE, CachyOS.

**The player never sees the overlay.** It exists only inside OBS. This matters for visual
tuning: the relevant observer is a stream viewer, for whom the indicator subtends roughly
4.4 degrees in a windowed 1080p stream, not the 7.6 degrees it spans on the native panel.

## Read these before working

| File | What it is |
|---|---|
| `docs/superpowers/specs/2026-07-26-tobii-obs-overlay-design.md` | The design, 13 sections. Section numbers are referenced everywhere |
| `docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md` | Phase 1 plan, 16 tasks |
| `docs/RESUME-phase1.md` | Current state, what is in flight, what is blocked |
| `docs/wip/phase1-ledger.md` | Blow-by-blow execution record |
| `tools/derive_visual_constants.py` | **Normative** for spec section 9. Run it. If it disagrees with the spec, the script wins |

Phase 1 is bring-up and the daemon. Phase 2 is the OBS filter plugin, Phase 3 is
verification. Phases 2 and 3 are deliberately unplanned: their constants depend on Phase 1
measurements.

## Hardware and environment

- Tobii Eye Tracker 5, USB `2104:0313` (runtime) / `2104:0102` (bootloader). No official
  Linux driver.
- Gameplay monitor DP-1-2: 2560x1440 at 360 Hz, 597 x 336 mm, X11 offset +4000+0,
  45 px per degree at 60 cm.
- osu! under Wine, borderless 2560x1440, `FrameSync = Unlimited`, 240 fps cap.
- OBS 32.1.2. Canvas 2560x1440, output 1920x1080 bicubic, 60 fps, x264 6000 kbps, NV12,
  BT.709 limited. Capture is `xcomposite_input`.
- nix 2.35.1 with flakes. The store needed `sudo nix-store --init` because Arch's package
  does not create it. No group membership needed, the daemon socket is `srw-rw-rw-`.

## Measured facts. Do not re-derive these, and do not trust documents over them

Every row below was an assumption that measurement disproved.

| Claim | Reality |
|---|---|
| 133 Hz sample rate (Tobii marketing) | **33.2 Hz.** `frame_counter` advances by exactly 4; dt 30.27 ms. The sensor counts at 133 Hz internally and ships every fourth frame. tobiifree's own `sdk/src/gusb.ts:99` says "~33Hz" |
| The other 3 frames are recoverable | **No.** Setting the two `0x04` bytes in the 20-byte subscribe payload to `0x01` produced zero frames. Only `stream_id` at bytes 9..10 is understood |
| `GazeSample` is 232 bytes (`ARCHITECTURE.md`) | **392 bytes** |
| Calibration blobs are "about 4 KB" | **Exactly 4096 max.** `cal_finish_blob_ptr()` returns `&out_scratch`, `[4096]u8` at `driver/src/tobiifree_core.zig:346`. Bounds-check against 4096, never against a buffer size |
| Zig 0.14+ (tobiifree README) | **0.15.x only.** 0.14 fails on unmanaged `ArrayList`, 0.16 removes `std.process.args`, `std.posix.getenv`, `std.fs.cwd` |
| Per-eye offset up to 45 px | **67 x 40 px** (78 combined) |
| ET5 needs Windows setup first | **It does not.** Handshake completes in 4 steps on a factory device |

`ARCHITECTURE.md` in the vendored driver is also wrong about four opcodes and invents a
`cal_retrieve` at `0x23` that is actually `cal_apply`. **`driver/src/daemon_protocol.zig`
at the pinned commit is the only source of truth for the wire protocol.**

## Protocol essentials

- Framing: `[u8 msg_type][u32 LE payload_len][payload]`, header 5 bytes.
- **`response` (0x02) prepends a one-byte `cmd_type`**, so its body starts at offset 6.
  Parsing from 5 misaligns every `f64`.
- **A client MUST send `subscribe` (`0x01` + u32 LE 0) on connect, and on every
  reconnect**, or it receives zero samples with no error at all.
- `validity == 0` means VALID. Inverting this is an easy C bug.
- Commands: `subscribe 0x01`, `get_display_area 0x02`, `set_display_area 0x03`,
  `set_display_area_corners 0x04`, `start_calibration 0x20`, `add_calibration_point 0x21`,
  `finish_calibration 0x22`, `cal_apply 0x23`, `disconnect 0xFF`.
- Socket: `/run/user/1000/tobiifreed/gaze.sock`

## The patch workflow. This is subtle and cost three fix rounds

`vendor/tobiifree` is a pinned submodule that is **never permanently edited**. Every change
is a numbered `.patch` under `patches/`, applied by `scripts/build.sh`, which:

1. Resets to the **outer repo's gitlink** (`git rev-parse :vendor/tobiifree`) via
   `checkout --force --detach`. Not `reset --hard HEAD`, which reads the submodule's own
   HEAD. Not `reset --hard $PIN`, which rewrites an attached branch, and this submodule
   has a local `main`.
2. `clean -fd`. Not `-fdx`, which destroys the Zig cache: 7.5 s cold versus 0.18 s warm.
3. Applies `patches/*.patch` in filename order.
4. Runs `git add -A` inside the submodule so a later `git diff` yields only new work.

To add a patch: run `build.sh`, edit inside `vendor/`, extract with
`git -C vendor/tobiifree diff > patches/NNNN-name.patch`, run `build.sh` again to prove it
applies from the pin, then commit the `.patch` to the outer repo.

**Never `git checkout -- .` inside the submodule between steps** (it restores from the
index, which `git add -A` has dirtied) and **never commit inside `vendor/`**.

Build: `./scripts/build.sh`. Zig comes from `nix develop` and is not on the system PATH,
so run Zig commands as
`nix develop /home/jason/Documents/tobii-eye-tracker/vendor/tobiifree --command bash -c "..."`.

## Working conventions

- Branch `feat/phase1-bringup`. Conventional commit prefixes: `feat:`, `fix:`, `test:`,
  `chore:`.
- `set -euo pipefail` is house style for shell, except where diagnostic greps are expected
  to fail, in which case omit `-e` deliberately and say why.
- **Opus for both implementers and reviewers.** Do not downgrade to dodge a 529, retry or
  verify yourself and say so.
- **No task closes without a review verdict.** Verifying yourself first is worth doing and
  catches different defects faster, but a silent reviewer is not a clean one. This rule
  exists because closing early twice let a Critical and an Important slip through.
- When a number matters, ask where it came from. Almost every defect found in this project
  so far has been a provenance failure rather than a logic error.

## Things that bite

- `skin/` is 140 files of third-party osu! art and audio, deliberately untracked and
  gitignored. It was only ever reference material for the hue palette.
- The device stores its display area and keeps it across sessions. It currently holds a
  **1500x1000 mm placeholder**, and correct geometry must be set before calibration
  because calibration is computed in that frame. `isReset()` only fires below 50 mm, so a
  wrong-but-plausible geometry is otherwise unfixable, which is what `--force-display-area`
  is for.
- `.superpowers/` is gitignored, so anything written there is not in git. Durable notes
  belong in `docs/`.
