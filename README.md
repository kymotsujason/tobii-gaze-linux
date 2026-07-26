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

Applies every patch under `patches/` to `vendor/tobiifree`, then runs `zig build` for
`tobiifreed` inside the pinned `nix develop` shell (Zig 0.15.x). The resulting binary
lands at `vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed`.

## Licensing

`tobiifree` is GPL-3.0. Fine for a private build, relevant if this is ever published.
