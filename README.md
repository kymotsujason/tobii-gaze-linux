# Tobii ET5 gaze overlay for OBS

Draws where the player is looking into the OBS output for osu! streams, with no
measurable effect on the game itself. The player never sees the overlay: it exists
only inside OBS.

This is Plan 1 of 3. Phase 1 gets calibrated gaze coordinates streaming reliably from
a Tobii ET5 on Linux, and records a real gaze trace used later to refit the overlay's
signal-processing constants. Plan 2 (the OBS filter plugin) and Plan 3 (the
verification harness) depend on what this phase produces.

See `docs/superpowers/specs/2026-07-26-tobii-obs-overlay-design.md` for the full
design and `docs/superpowers/plans/2026-07-26-phase1-bringup-and-daemon.md` for the
task-by-task implementation plan.

## Layout

- `vendor/tobiifree/` - pinned upstream Zig driver, a git submodule, never edited in
  place.
- `patches/` - numbered `.patch` files applied to the vendor checkout by
  `scripts/build.sh`.
- `scripts/build.sh` - applies the patch series and builds `tobiifreed`.
- `gaze-cal/` - calibration, display area setup, live preview, and trace recording.
- `traces/` - recorded gaze traces (gitignored except this README).
- `tools/` - `derive_visual_constants.py`, normative for the overlay's visual constants.

## Building

```
./scripts/build.sh
```

Clone with submodules, or run `git submodule update --init --recursive` first. The build
resets `vendor/tobiifree` to the pinned commit, applies every patch under `patches/` in
filename order, and then builds `tobiifreed`.

The toolchain is Zig 0.15.x, which the build normally takes from `nix develop`. Zig 0.14
and 0.16 both fail to compile this code, so the version is not negotiable. If `nix` is
installed but not working, the build falls back to a Zig 0.15.x found in `/nix/store`, in
`$ZIG`, or on `PATH`, and says which one it chose. The resulting binary lands at
`vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed`.

## Licensing

This project is licensed under the **GNU General Public License v3.0**. The full text is
in [LICENSE](LICENSE).

The upstream driver, [tobiifree](https://github.com/Aetherall/tobiifree), is also
GPL-3.0. It is vendored as a pinned git submodule and is never edited in place. Because
every file under `patches/` contains excerpts of that GPL-3.0 source, both the changed
lines and their surrounding context, those patches are derivative works and carry the
same license. Licensing the whole repository GPL-3.0 keeps that boundary simple.

### Changes made to tobiifree

GPL-3.0 section 5(a) asks that modified files carry notice of the modification. The
patch series is that notice. Each file below is applied to the pinned upstream checkout
at build time and changes nothing in the upstream repository itself.

| Patch | Upstream files touched | What it changes |
|---|---|---|
| `0000-calibration-buffers.patch` | `driver/src/tobiifree_core.zig`, `driver/src/tracker.zig`, `applications/tobiifreed/src/main.zig`, `applications/tobiifreed/src/server.zig` | Enlarges the calibration buffers and bounds the calibration blob against the 4096-byte scratch buffer. `session_out` was 512 bytes, which overflowed on any blob past roughly 478 bytes. Also bounds the two daemon response buffers against what can actually reach them. |

The series grows as Phase 1 proceeds, and the numbering is a valid application order
rather than the order the work was done.

### Not included here

- `skin/` is third-party osu! art and audio. It is gitignored deliberately and is not
  redistributable through this repository.
- `traces/` holds recorded gaze traces, which are personal biometric data. Everything
  except the README is gitignored.
