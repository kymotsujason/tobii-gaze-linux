# Tobii ET5 gaze overlay for OBS

Design, revision 2. Revision 1 was reviewed by four models and my own verification; every
change below traces to a finding that was confirmed against source. Claims are cited to
`file:line`. Where something is unverified it says so.

## 1. Scope and observer

Draw where the player is looking into the OBS output for osu! streams, with no measurable
effect on the game.

**The player never sees the overlay.** It exists only inside OBS. This matters for tuning:
the relevant observer is a stream viewer, for whom the indicator subtends roughly 4.4 degrees
in a windowed 1080p stream, not the 7.6 degrees it spans on DP-1-2. Revision 1 tuned against
a browser canvas at 2.1 degrees, which was accidental but closer to the viewing condition
than full-screen rendering would have been.

**Hardware and settings.** Tobii ET5, USB `2104:0313`, 133 Hz nominal, accuracy 0.5-1 degree.
DP-1-2: 2560x1440 at 360 Hz, 597 x 336 mm, X11 offset +4000+0, 45 px per degree at 60 cm.
osu! under Wine, borderless 2560x1440, `FrameSync = Unlimited`, 240 fps cap. OBS 32.1.2,
canvas 2560x1440, output 1920x1080 bicubic at 60 fps, x264 6000 kbps, NV12, BT.709 limited.

## 2. Components

```
Tobii ET5
  | libusb bulk, EP 0x83 IN / 0x05 OUT, exclusive claim of interface 0
  v
tobiifreed          vendored at a pinned commit PLUS a patch series (section 4)
  |                 systemd user unit, Restart=always
  +-- unix socket --> obs-tobii-gaze    OBS FILTER plugin, C          [stream]
  +-- unix socket --> gaze-cal          calibration, preview, blob re-apply [setup + boot]
```

`gaze-cal` runs twice: interactively during setup, and as `ExecStartPost` on the daemon unit
to re-apply the saved calibration blob. Revision 1 assigned a boot-time responsibility to a
setup-only tool, which could not work.

**Licensing.** tobiifree is GPL-3.0. Fine for a private build; relevant if ever published.

## 3. Setup order

The order is load-bearing. Calibration is computed in the frame the display area defines, and
a wrong display area cannot be corrected later without patch P4.

0. Install `assets/99-tobii.rules` to `/etc/udev/rules.d/`, reload, replug. Without this,
   `LibusbTransport.init` fails with `DeviceNotFound` or `ClaimInterface`.
1. **Prove the tracker streams gaze at all.** This resolves the open question of whether an
   ET5 needs one-time Windows provisioning. Nothing else can start until it does.
2. Measure DP-1-2 physically: width, height, the tracker-to-screen offset, depth, and tilt.
3. Force the display area (P4), then read it back with `get_display_area` (0x02) and assert
   it matches. **This readback is a blocking gate.** Do not proceed on a mismatch.
   Note the readback returns **nine f64 corner coordinates** (`tl`, `tr`, `bl` in tracker-space
   mm), not the `w_mm/h_mm/z_mm/tilt` the config accepts, so the assertion needs the
   conversion: `w = |tr_x - tl_x|`, `h = |tl_y - bl_y|`, with depth and tilt recovered from the
   z components. Compare all nine, within a tolerance.
4. Calibrate through `gaze-cal`. Save the blob returned by `finish_calibration`.
5. Replug the tracker and check whether calibration survived. This answers an open question
   and decides whether step 6 is mandatory or merely harmless.
6. Wire `gaze-cal --apply-saved` as `ExecStartPost` on the daemon unit, and make the unit
   **`Type=notify`** with `sd_notify("READY=1")` sent after `Server.init` binds the socket.
   With `Type=simple`, systemd runs `ExecStartPost` immediately after `execve`, while the
   daemon is still blocked in `LibusbTransport.init` and `Tracker.init`, so `gaze-cal` would
   hit `ENOENT` on a socket that does not exist yet and fail the unit.
   Note this covers daemon *restart* only; in-process USB recovery is P5's job.

## 4. Daemon patch series

Revision 1 said "vendored unmodified". That is not achievable. Six patches, kept as `.patch`
files so they rebase onto upstream, applied by the build before `zig build`.

| # | Fix | Why |
|---|---|---|
| P1 | Give **one thread exclusive ownership** of `transport`, `tracker` and all core globals. Queue every forwarded command to it; register the pending routing entry **before** sending | `usbThreadFn` loops `tracker.poll()` while the main thread reaches `drainReads` -> `recv` -> `core.feed_usb_in` (`tracker.zig:288,294`). Pausing only around the three state machines is not enough: `get_display_area`, both setters and `add_calibration_point` mutate the same globals (`main.zig:184-203`), and the pending entry is installed *after* the send (`:193-195`), so the USB thread can eat the response first. Streaming alone is unaffected. |
| P2+P3 | **One write path for every daemon socket write.** Maintain a bounded per-client output queue with a byte offset; loop on short counts; on `EAGAIN` buffer the tail and resume on `POLLOUT`; drop only a gaze frame that has had **zero** bytes written; never drop mid-frame; never drop a response | `server.zig:162` does `_ = std.posix.write(...) catch { removeClient }` on a non-blocking fd, so it both disconnects on `EAGAIN` and discards the short-write count. Treating these as two separate patches is wrong: a short write followed by `EAGAIN` leaves a frame prefix on the wire, which is exactly the desync P3 exists to prevent. Applies to `server.zig:162` and `main.zig:143,240,249`. |
| P4 | Add `--force-display-area` | `isReset()` is `w < 50 or h < 50` (`tracker.zig:41-45`) and the CLI accepts only `--init-config` and `--ws`. Any plausible stale geometry is kept forever. |
| P5 | Unplug recovery, with **explicit teardown sequencing and a full bootstrap replay**: signal the USB thread out of poll, join it, `transport.deinit`, capped backoff, re-init, re-handshake, force+readback display area, re-apply the saved calibration blob, publish a device-present transition, respawn the thread | `LIBUSB_ERROR_NO_DEVICE` returns immediately rather than consuming the 100 ms timeout, so `while (!quit) tracker.poll()` spins a core, and `main.zig:19-21` sets `.log_level = .debug`. Recovery must replay bootstrap because `ExecStartPost` does **not** re-run for an in-process reconnect, so without this the daemon comes back uncalibrated while reporting success. |
| P6 | Replace the 1 ms sleep loop with a real `poll()` on the notify pipe, socket fds and `POLLOUT` | `main.zig:469-482` has no `poll()` despite its comment, so the notify pipe is dead weight, every sample gains up to 1 ms, and the CPU wakes 1000 times a second. |
| P7 | **Enlarge the calibration buffers.** `session_out` to at least 4130 bytes, the client command buffer to at least 64 KB, `sendResult`'s response buffer likewise; reject over-size blobs before any copy; remove the `space == 0` buffer-wipe | `session_out` is `[512]u8` (`tobiifree_core.zig:1051`) while `build_cal_apply` writes envelope + header + 2 + `blob_len` through a raw pointer (`:566`), so any blob past ~478 bytes corrupts adjacent globals. `Client.buf` is `[4096]u8` (`server.zig:18`) and wipes itself when full. Calibration re-apply is a required deliverable and currently cannot work. |
| P8 | Add a **device status message** to the protocol, emitted on connect and on every transition | Section 11 requires the socket to carry device presence, but `daemon_protocol.zig:21-28` defines only `gaze`, `response`, `display_area`, `err`. Without it the plugin cannot distinguish unplugged hardware from a healthy daemon with stale gaze. |
| P9 | Purge pending entries on client disconnect; key them on a slot plus generation, not a bare fd; enqueue responses through the main writer rather than writing from the USB callback | `main.zig:89-143` stores only `client_fd` while `server.zig:168-174` closes the fd without purging, so a reused fd number can receive another client's response. |

P1, P4, P5 and P7 are correctness. P2+P3, P6, P8 and P9 are reliability. All are worth
upstreaming. **The patch series is the largest single cost in this design and the biggest
argument for reconsidering the browser-source route**, which needs none of P1, P7 or P9.

## 5. Toolchain

Build via `nix develop` / `nix build .#tobiifreed`. The repo already pins its exact toolchain
in `flake.lock` (nixpkgs rev `6201e203d095`), which beats guessing. Nix is not currently
installed.

Fallback if Nix is declined: **Zig 0.15.x**, fetched as a tarball. Not 0.14, because
`tobiifree_decode.zig:246` uses the unmanaged `std.ArrayList(u32) = .{}` form. Not 0.16,
because it removes `std.process.args`, `std.posix.getenv` and `std.fs.cwd`, all of which the
daemon uses (`main.zig:374`, `:256`, `:260`). The packaged Arch Zig is 0.16.

## 6. Wire protocol

**Source of truth is `driver/src/daemon_protocol.zig` at the pinned commit.**
`ARCHITECTURE.md` is wrong about four opcodes, invents a `cal_retrieve` at `0x23` that is
actually `cal_apply`, lists realm commands `0x10`-`0x13` that do not exist, and states the
gaze payload is 232 bytes.

- Framing: `[u8 msg_type][u32 LE payload_len][payload]`, header 5 bytes.
- Gaze payload: a raw `memcpy` of the `GazeSample` extern struct, **392 bytes**, not 232.
- **`response` (0x02) is framed differently.** Its payload begins with a one-byte `cmd_type`
  discriminator: `[0x02][u32 LE 1+N][u8 cmd_type][payload...]` (`daemon_protocol.zig`,
  `encodeResponse`). Parsing a response body from offset 5 rather than 6 misaligns every
  `f64` in it, so `get_display_area`'s nine doubles decode as garbage.
- Commands: `subscribe 0x01`, `get_display_area 0x02`, `set_display_area 0x03`,
  `set_display_area_corners 0x04`, `start_calibration 0x20`, `add_calibration_point 0x21`,
  `finish_calibration 0x22`, `cal_apply 0x23`, `disconnect 0xFF`.
- Server messages: `gaze 0x01`, `response 0x02`, `display_area 0x03`, `err 0xFF`.

**The client must send `subscribe` immediately on connect.** Clients are created with
`subscribed = false` (`server.zig:80`) and `broadcastGaze` skips them (`:161`). Omitting this
produces an open socket, no error, and zero samples, which is indistinguishable from a dead
tracker.

Reader requirements:

- Bound `payload_len` against a hard maximum of 8 KB **and** against the known size for the
  message type. A violation means the stream has desynced: force a reconnect, never resync.
- Skip unknown message types by `payload_len`. Both `gaze-cal` and the plugin are connected
  during recalibration and responses are routed through a fallible pending table, so the
  plugin will see traffic that is not for it.
- Watchdog: fd open but no valid gaze frame for 1 s forces a reconnect. A desync does not
  produce EOF, so the 2 s retry path would otherwise never fire.
- Validate every consumed field: `_Static_assert` on `sizeof`, `offsetof` assertions on each
  field, a received-length check, `isfinite` on all coordinates, and a range check.
  A total-size assert alone cannot detect reordered fields.
- Honour `present_mask`. `clearSample()` zeroes the struct and zero means valid, so a
  truncated frame otherwise decodes as two valid eyes at (0,0).
- **`validity == 0` means valid.** An easy inversion bug in C.

## 7. OBS integration

Register as a **filter** on the game capture source:

```c
.type         = OBS_SOURCE_TYPE_FILTER,              /* separate struct field */
.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,  /* separate struct field */
```

These are different fields (`obs-source.h:236,239`) and both `OBS_SOURCE_TYPE_FILTER` and
`OBS_SOURCE_VIDEO` equal 1, so OR-ing them is a mistake that inspection would not catch.

`video_render` calls `obs_source_skip_video_filter()`, which renders the parent directly with
no intermediate texture and no copy, and then draws our quad. The `process_filter_begin/end`
path would allocate a full-resolution texrender and run a full 2560x1440 fragment pass.

**`video_render` fires once per render target**, 1..N times per frame across program,
preview, projectors, multiview and Studio Mode. Therefore:

- All **presentation state** (opacity envelope, which taps are current, the sampled gaze
  point for this frame) advances in `video_tick`, exactly once per frame.
- `video_render` is **read-only** with respect to that state, and draws it once per target.

**Geometry is computed in the parent's source-local coordinate space, not per target.**
`video_render` receives only `(void *data, gs_effect_t *effect)` (`obs-source.h:351`); there is
no target-dimensions parameter, and `obs_source_get_width/height` returns *source* dimensions,
not the framebuffer's. OBS applies each scene item's transform around the render call, so
drawing in source-local pixels lets every view scale it correctly for free. Attempting to size
the annulus from a projector's framebuffer would either scale twice or vary per view.

**Thread ownership**, because sections 7 and 8 otherwise appear to assign "filter state" twice:

| State | Owner | Read by |
|---|---|---|
| Fusion, one euro, gap, outlier, offset EMA, sample ring | socket thread, sample rate | nobody directly |
| Immutable timestamped snapshot (filtered xy, stamps, validity, device flags) | published by socket thread under the mutex | `video_tick` |
| Opacity envelope, tap selection, quad geometry | `video_tick`, once per frame | `video_render` |
| Nothing | `video_render` | — it mutates nothing |

Other constraints, all verified:

- **Array uniforms are silently broken on the GL backend.** `shader-parser.c:493` records
  `array_count` but `gl-shaderparser.c` never emits `[N]`. Use eleven discrete `float4
  tap0..tap10`, packing `xy` = position, `z` = radius scale, `w` = weight.
- **Clamp the quad to `[0,W]x[0,H]`.** One scene render path clips via `gs_ortho`
  (`obs-scene.c:920-935`) and the direct path does not.
- **Never assume the incoming blend state.** `gs_blend_state_push()`, then set **both** the
  factors and the op explicitly — `gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA)` for
  premultiplied **and** `gs_blend_op(GS_BLEND_OP_ADD)` — then `gs_blend_state_pop()`. The op
  matters because OBS's own scene blend tables include `GS_BLEND_OP_MAX` paths
  (`obs-scene.c:116`), so a prior pass can leave MAX set.
- **sRGB.** Keep `OBS_SOURCE_SRGB` set, but not for the reason revision 2 first gave. It is
  **not** inert: `obs_source_main_render` reads it and forces `gs_set_linear_srgb(false)` for
  sources without it. So the flag preserves linear-sRGB source state, *and* our manual draw
  must still save/restore framebuffer sRGB via `gs_enable_framebuffer_srgb(gs_get_linear_srgb())`
  and upload Iris converted to linear. Note the parent, `xcomposite_input`, registers only
  `VIDEO | CUSTOM_DRAW | DO_NOT_DUPLICATE` and is **not** sRGB-aware, so the captured game is
  not a linear-light source. Appearance therefore cannot be predicted from a pure linear-light
  derivation alone, and peak opacity must be frozen only after a screenshot comparison on the
  real skip path.
- **Coordinate mapping is the identity only while osu is borderless at exactly 2560x1440.**
  Compare `obs_source_get_width/height(parent)` against that and surface a mismatch. Do the
  check in `video_render`, **not** `video_tick`: `obs_filter_get_parent` is only guaranteed
  valid inside `video_render`, the filter callbacks, and `filter_remove` (`obs.h:1131-1143`).
  Offset and scale properties default to identity.

**Lifecycle**, which revision 1 omitted entirely: `create` allocates, initialises the mutex,
and spawns the socket thread. Graphics objects are created under `obs_enter_graphics()` or on
first `video_render`. `destroy` sets a stop flag, wakes the socket thread through its eventfd,
joins it, closes the fd, then enters the graphics context to destroy `gs_` objects, then
destroys the mutex. Partial-construction failures unwind in the same order.

### The shader combine, normative

```
rgb  = max over contributing taps of the tap's UNPREMULTIPLIED linear colour
a    = 1 - product over taps of (1 - a_i)
a   *= exclusion(distance from the CURRENT gaze point)   // applied to TOTAL alpha
emit float4(rgb * a, a)                                  // premultiplied
```

The exclusion is applied **after** combining, to the total, and is centred on the current
gaze rather than on each tap. This is load-bearing. Applied per-tap it would do nothing about
the saccade case in section 9, where trailing rings centred at the *old* gaze point put their
brightest part on the *new* one.

The max is on unpremultiplied colour while alpha accumulates as an over. Maxing premultiplied
values instead yields a plausible-looking but dimmer image.

`GS_BLEND_OP_MAX` does exist (`graphics.h:127`) but is not used: there is no
`gs_blend_op_separate`, and doing this in one shader is cheaper and avoids the question.

The only surviving reason to avoid an offscreen buffer is that a `destination-out` pass on the
canvas would erase the captured game, because OBS clears the main texture to `(0,0,0,0)` and
it carries real alpha (`obs-video.c:181-184`). Revision 1 gave three reasons; the texrender
one was wrong (the `gs_texrender_reset`-in-tick idiom handles it) and the MAX one was
mathematically false.

## 8. Signal processing

Runs at sample rate on the socket thread.

- **Consume the per-eye fields** `gaze_point_2d_L_norm` and `gaze_point_2d_R_norm`, not
  `gaze_point_2d_norm`, which is already temporally filtered on-device and would cascade an
  unknown group delay under ours. Log the combined field alongside during bring-up so the
  device's own delay can be measured by cross-correlation.
- **Coordinate space is normalised `[0,1]`, stated explicitly.** In pixels, `beta = 1.5`
  yields alpha 0.876 at fixation, a passthrough. In normalised space it yields 0.043.
- **Normative unit system: degrees of visual angle**, converted on entry as
  `x_deg = x_norm * 2560 / 45`, `y_deg = y_norm * 1440 / 45`, filtered there, converted back
  on exit. This resolves two problems at once. Normalised space is anisotropic, since one x
  unit is 2560 px and one y unit is 1440, so a shared `hypot` cutoff would reintroduce the
  oblique bowing it exists to prevent. And `beta` is meaningless without its space: in pixels
  it yields alpha 0.876 at fixation (a passthrough), in normalised units 0.043.
- **`beta` must be refitted for degrees. The 1.5 figure belongs to normalised units and does
  not carry over.** Treat it as unset until the first hardware session.
- One euro: `minCutoff 0.8`, `dCutoff 4.0`. Measured in normalised units: raising `dCutoff`
  from 1.0 to 4.0 drops post-saccade alpha from 0.224 to 0.079, so the filter tightens when
  the fixation starts rather than 160 ms later. The direction of that result carries over to
  degrees; the magnitude does not.
- `dt` from `timestamp_us` deltas, clamped to [1, 50] ms, with fallback to host
  `CLOCK_MONOTONIC` if device and host deltas drift apart. Publish both stamps.
- **Outlier gate ahead of the filter**: reject displacements exceeding a physiological
  maximum for the elapsed `dt`. Real trackers emit valid-but-wrong samples around lid closure,
  which is the dominant real artefact and which a clean validity flag never signals.
- Eye fusion, with the signs stated because getting them backwards doubles the error rather
  than cancelling it. Let `off = EMA(L - R)`, tau ~2 s, updated only while both eyes are valid.
  Both valid: `(L + R) / 2`. Left only: `L - 0.5 * off`. Right only: `R + 0.5 * off`.
  Neither: hold position.
- Gaps: more than three consecutive invalid intervals (~25 ms) is a gap. On resume, reset the
  derivative state and snap position, hidden under the opacity ramp, and keep the reset active
  over a short window rather than only the first sample.
- Sustained loss beyond 250 ms fades out. Daemon disconnect uses the same path.

## 9. Visual design

**Colour: Iris, hue 257, `#8A5CFF`.** The one constant that survived review unchanged. The
skin occupies hues 24.8, 56.5, 85.7, 188.6 (the cursor), 325.7 and 345.8; the largest
unoccupied arc is 137 degrees wide centred at 257. Worst-case colour-blind separation is
deltaE 52.5 against any skin element under tritanopia. The halo is built from
`hsl(257,100%,62%)`, whose green channel of 61 keeps it chromatic under accumulation.

**Provenance: `tools/derive_visual_constants.py`.** That script is normative. It prints every
number below. If the script and this section disagree, the script wins.

**Form: an annulus plus a gaze-centred exclusion.** Ring centre 240 px, half-width 110 px, so
the ring spans 130 to 350 px with a smooth `(1 - t^2)^2` bump. Exclusion closed out to 163 px
(the 118 px hit circle plus one degree of tracking error), fully open by 203 px. Measured in
linear light over a real 118 px osu hit circle:

| | Blob 250 + hole 105 (rev 1) | Annulus (rev 2) |
|---|---|---|
| Mean alpha over the hit circle, fixation | 0.51 | **0.000** |
| Mean alpha over the hit circle, 240 px saccade | 0.51 | **0.000** |
| Clear centre radius | none, hole was 105 px | **168 px** |
| Halo peak | 0.40 | **0.77** |

**The exclusion is not optional.** Without it the annulus is *worse* than the blob during
saccades. Trailing rings whose radius happens to equal the jump distance land their brightest
part exactly on the new gaze point, giving 0.652 alpha at a 240 px jump, which is ordinary osu
spacing. The failure window is roughly 180 to 300 px. The exclusion drives it to 0.000 at every
jump distance while leaving the ring at its full 0.35, because the ring peaks at 240 px and the
exclusion is fully open by 203 px.

Revision 1's coverage figure of 0.33 was a single-pixel readout at the deepest point of the
hole; the honest mean was 0.51, and the hole was smaller than the object it protected.

**The tail tapers in weight only, never in ring radius.** Eleven taps over 240 ms with weights
`0.15 + 0.30 * k/10` and the current point at 1.0. Scaling the ring radius too was what filled
the centre.

**Ring width is an open decision, not a settled constant.** The annulus protects the note you
are looking at perfectly, but its maximum opacity lands at ring radius, which in osu is often
exactly where the neighbouring note is. Since players read ahead, that neighbour is frequently
the note the cursor is currently hitting, along with its judgment text. Measured mean alpha
over a 118 px note at various distances:

| Config | Gaze note | Note at 200 px | at 240 px | at 280 px | Peak | Outer |
|---|---|---|---|---|---|---|
| Blob 250 + hole 105 (rev 1) | 0.31 | 0.22 | 0.11 | 0.04 | 0.72 | ~250 px |
| Wide ring 240/110, peak 0.35 | 0.00 | 0.44 | 0.47 | 0.42 | 0.77 | 343 px |
| Wide ring 240/110, peak 0.22 | 0.00 | 0.33 | 0.35 | 0.31 | 0.59 | 341 px |
| **Thin ring 200/45, peak 0.55** | **0.00** | **0.23** | **0.20** | **0.15** | **0.92** | **243 px** |

The thin ring dominates the wide one on every axis: less than half the neighbour occlusion,
higher peak visibility, and a third smaller. **But it is a different aesthetic.** A narrow band
at 0.92 alpha reads as a reticle; the wide soft ring reads as a cloud, which is what was
originally chosen. Numbers cannot settle that.

**Decision required at bring-up, with both rendered through OBS**: thin bright ring versus wide
soft ring, and then peak opacity within whichever is chosen. Peak cannot be transferred
numerically from revision 1 because the colour space and the geometry both changed at once;
the attempt produced 0.05 with a poor fit and was discarded.

## 10. Temporal alignment

**Transport skew and tail asymmetry are different things and revision 1 conflated them.**

Transport: with `xcomposite_input` there is no readback queue, so a captured pixel is 0-7 ms
old while gaze is 15-80 ms old. **Gaze lags the pixels.** Encode, mux and network apply
equally to both and contribute zero skew. One signed offset knob: positive delays gaze in the
ring, negative instructs the operator to add an OBS Render Delay on the capture, with the gaze
filter ordered outermost. Default zero.

Tail: the ~82 ms figure from revision 1 is an *opacity* centroid, but taps combine with
`max()`, so the perceived centre is the current point. **Do not size a Render Delay from it**
or the game desyncs from the rest of the scene by ~80 ms. Tail asymmetry is a visual parameter
to be judged on a recorded saccade.

Measure transport skew in two parts, because they need different instruments and summing the
wrong one leaves most of the skew uncorrected.

- **Downstream only** (daemon, socket, plugin, OBS composite): inject a synthetic one-sample
  jump at monotonic time T while a separate client flashes a marker on screen at the same T,
  then count frames between them in the recording. This deliberately bypasses the tracker.
- **Tracker acquisition** (sensor exposure, on-device processing, USB), which is the larger
  and less knowable half: record the same saccade twice, once as 240 fps video of the eye and
  once as the daemon's timestamped output, then cross-correlate the onsets. Human reaction
  variability cancels because it is one physical event observed in two signals.

Sum the two signed results, then validate the final offset against recorded real saccades.
Also determine during bring-up whether `timestamp_us` is stamped at exposure or at transmit,
by comparing device deltas against host arrival deltas over a minute.

## 11. Failure handling

A frozen cloud looks identical to a working one, so every failure must be visible.

| State | Behaviour |
|---|---|
| Socket missing or EOF | Render nothing, retry every 2 s |
| Desync (bad `payload_len`, or no valid frame for 1 s) | Force reconnect |
| **Every reconnect** | Reopen, **send `subscribe` (0x01)**, reset reader state. Reopening alone reproduces the open-socket-zero-samples failure. |
| No sample for 250 ms | Fade out |
| Both eyes invalid under 250 ms | Dip to the opacity floor, hold position |
| One eye invalid | Keep drawing from the survivor, widen, apply the offset EMA |
| Daemon alive, device gone | P5 recovers and replays bootstrap. Distinguishable only via the P8 status message, which the protocol does not currently have; without P8 this state is indistinguishable from "connected but idle" |

**The operator cannot see the overlay**, so filter properties are not an adequate channel.
Provide an always-on indicator outside the program feed — a dock, tray item, or preview-only
corner HUD — showing connection state, last sample age, device presence, and calibration age.
Plus a kill hotkey and a recalibration hotkey.

## 12. Verification

**Primary latency test is input-to-photon**, by photodiode or 240 fps phone camera, across all
arms. The frametime protocol cannot see the thing that matters: `xcomposite_input` holds a
composite redirect that prevents KWin unredirecting osu, an estimated 3-8 ms, while osu still
renders at 240 fps. Test `pipewire` and `xshm` capture in the same rig, since if the redirect
cost is real the fix is changing capture method, and that is far cheaper to decide before the
plugin exists.

**Secondary CPU/GPU protocol.** Arms: A no OBS no daemon, B OBS only, C plus daemon, D plus
filter, plus Studio-Mode and projector variants of D. Randomised block order, not ABAB, since
there are more than two arms. Same osu replay every block.

- **GPU time is the primary number for arm D**, via `gs_timer` or `GL_TIME_ELAPSED` queries.
  `os_gettime_ns()` around `video_render` measures command submission, not execution, and GPU
  contention is the overlay's only plausible route to affecting osu.
- **Add a positive control**: the same shader at ~10x pixel area. The harness must resolve it
  as distinct from arm C before a null result in D means anything.
- Count deadline exceedances and report p99/p99.9/max, not 1% lows. Read
  `/proc/<pid>/task/<tid>/schedstat` per thread for osu's render thread, not the group leader.
- **Fault arms**: unplug the tracker mid-block, restart the daemon mid-block, `SIGSTOP` OBS
  for 5 s then continue. Pass criteria are section 11's, plus osu's distribution unchanged.

**Encode regression.** At the user's settings the cloud costs 257 kbps absolute, about 4% of
a 6000 kbps budget, and loses up to 33% chroma saturation on high-motion frames; banding is
mild. These numbers were produced with revision 1's compositing and **must be re-run** against
the annulus in linear light. Dither remains untested: the variant I ran was a no-op.

## 13. Open items

1. Whether the tracker streams gaze at all on Linux, and whether one-time Windows provisioning
   is required. Revision 1 argued against it from the absence of provisioning code in
   tobiifree, which is a null result and proves nothing.
2. Whether calibration survives a replug.
3. Peak opacity, and the tail weight curve, both pending a re-pick against a correct renderer.
4. Every signal-processing constant is still fitted to a simulation whose ground truth is a
   21 ms first-order glide with no fixations, no saccades, and perfectly periodic 120 ms
   blinks. Ranked by exposure: `beta` and `dCutoff` are worst and should be expected to refit
   from scratch; the gap threshold next; `minCutoff` and the fusion EMA tau are moderate; the
   Iris hue is unaffected because it derives from real skin data. The bring-up session's
   explicit deliverable is a recorded raw gaze trace to refit against.
