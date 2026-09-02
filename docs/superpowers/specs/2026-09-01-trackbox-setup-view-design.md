# Track box setup view for gaze-cal

A fullscreen screen on the gameplay monitor that shows what the tracker sees (the two eyes
inside the device's own track box, the distance, and the gaze point raw and corrected) and
runs the fit and verify sweeps from that screen. It replaces `scripts/fit-correction.sh` as
the way a new user gets a working correction, while the script stays for anyone who prefers
a terminal.

## Why this exists

The project's two proven failure modes are invisible from the chair. With the room lights
off the tracker returns zero valid frames at the top left point, and closer than about 520
mm the top row of the screen leaves the tracker's cone. Together they cost four sweeps, and
each was found by reading `eye_mm` out of the log after the sweep had already failed. A
screen that shows both eyes and the distance before the first dot is drawn would have caught
every one of them.

The same screen also shows where the device thinks the user is looking, raw and corrected,
which is the quickest way to see the 18 percent gain and what form S does to it, and to see
the corrected dot drift as the head leaves the fitted seat.

## What the device provides, measured 2026-09-01

Each gaze sample carries `trackbox_eye_pos_L` and `trackbox_eye_pos_R`, which the driver
decodes from fields 0x03 and 0x09 and describes at `tobiifree_core.zig:897` as the eye's
normalised position inside the track box. Nothing in this project had read them before.
Sampled live for 32 seconds (1060 samples, 873 with both eyes valid) with the user moving
gently in the chair:

| Quantity | Range seen |
|---|---|
| Left eye box x | 0.519 to 0.603 |
| Right eye box x | 0.370 to 0.452 |
| Box y, both eyes | 0.439 to 0.483 |
| Box z, both eyes | 0.287 to 0.403 |
| Eye z in mm | 544 to 600 |

The values stay in 0 to 1, sit near 0.5 on the tracker's centre line, drop to exactly zero
whenever that eye is invalid, and move with the head. Pairing box z against eye z puts box z
of 0 near 410 mm and box z of 1 near 880 mm, and x and y come out near 440 mm per unit at
that distance. The two eyes sit 0.15 apart, which is the user's 65 mm IPD at that scale.
Those edge figures are extrapolated from a 56 mm range of movement and are rough until
someone walks to an edge. The design doesn't depend on them: the fields are drawn as they
come, in a unit box, and the device decides where its edges are.

The display space variant of the field (0x25 and 0x27), which the driver documents as
always zero, was live in the same run and tracked the first field in x and y with a
different z. It isn't used.

## Scope

In scope:

- A new command, `gaze-cal setup [--output NAME] [--config PATH]`.
- The track box view, the gaze dots, the dwell target, and the fit and verify flow.
- Splitting the fit and accuracy commands into a core that returns a verdict struct and a
  wrapper that prints it, so the view can show the verdict without a terminal.
- Drawing primitives, a back buffer, key input and a keyboard grab in `stimulus.c`.

Out of scope:

- Any change to what the fit computes, to the refusals, or to the correction file.
- An always on widget while playing. The view is a check before a sweep or a recording.
- Showing the full terminal output in the window. The window shows a short verdict, and
  the full report goes to the terminal and `gaze-cal.log` exactly as today.
- The device's own calibration (`gaze-cal calibrate`). It has no measurable effect and
  isn't part of the flow.

## The screen

Layout A from the brainstorm. The window is the same fullscreen `override_redirect` window
`stimulus.c` already opens on the gameplay output, black background.

- The track box sits in the bottom left, one third of the screen tall (480 px on the 2560 x
  1440 panel) and 4:3 wide, drawn as a grey outline with faint centre lines. The two eye
  dots are filled discs at the box coordinates from the track box fields, x across and y
  down, green when that eye is valid. An invalid eye is drawn as a hollow red disc at its
  last valid position, and after one second with neither eye valid the readout line says
  "no eyes seen, check the room lights and sit about 600 mm back".
- A vertical distance bar to the right of the box, the same height, maps box z from 0 at
  the bottom to 1 at the top. The mean eye z in mm is drawn as a marker on it, and the 520
  mm floor is a red line at the box z that corresponds to it. Since the mapping from box z
  to mm is only extrapolated, the floor is placed from the live pair (eye z in mm against
  box z) using a running linear fit over the session, seeded with the measured pair above,
  so the line is right where the user actually is.
- A readout line under the box: distance in mm, `L ok` or `L lost`, `R ok` or `R lost`,
  the sample rate over the last second, and which correction is loaded (`fit: form S
  2026-09-01 14:35` from the file's `fit_utc`, or `no fit on disk`).
- The raw gaze point as an orange ring anywhere on the screen, from `gaze_point_2d_norm`.
  The corrected point as a green ring, from `gz_gaze_correct`, only when a usable
  correction loaded. Both use the calibration dot size from `stimulus.h`.
- A start target at normalised (0.6, 0.85), a 300 px grey disc with the word for the next
  action in it ("fit", "verify", "try again", "fit again"). A ring around it fills over
  1.5 seconds while the gaze dwells inside the acceptance region and resets when it leaves.
  Enter triggers the same action from the keyboard. Escape closes the view in every state.

The target's position and acceptance region come from the raw error, since a new user has
no fit yet. Under the fit#9 parameters the fixed point of form S in x is 0.61, and in y it
is 1.32, off the bottom of the screen, so the raw error is smallest low on the screen. At
(0.6, 0.85) the raw point lands about 4 px left and 127 px above the true one, against 222
px above at the screen centre. The acceptance region is therefore a 250 px radius around the
target centre, larger than the drawn disc, so the raw dot of an unfitted user still dwells
inside it. A different seat changes the offset `b`, so the region is generous on purpose.

## States

```
view ----(Enter or dwell on "fit")----> fit sweep ----> fit verdict
  ^                                                        |
  |                                    success: target reads "verify"
  |                                    refusal: target reads "try again" (back to fit sweep)
  |                                                        v
  |                              verify sweep ----> verify verdict, target reads "fit again"
  |                                                        |
  +----------------------(Escape from any state)-----------+
```

- view: everything above is drawn. The target reads "fit".
- fit sweep: the box, bar, readout, gaze rings and target are hidden. The nine dots are
  drawn by the existing fit core through `gz_stim_ops`, on the same window. Escape aborts
  through the interface's existing abort return (`show` returns -1).
- fit verdict: the view returns, plus a verdict block above the target: median px, worst
  px, "within one degree" or "outside one degree", and the fitted gains. On a refusal the
  block shows the reason in plain words and what to do. The target reads "verify" after a
  success and "try again" after a refusal.
- verify sweep and verify verdict: the same with the accuracy core, label `verify-setup`.
  The verdict block shows the corrected median and worst against the 35 to 50 px band, and
  how far the head sat from the fitted seat. The target reads "fit again".

The verdict block stays until the next sweep starts or Escape closes the view.

## Components

`gaze-cal/src/view.c` and `view.h`, new. Owns the state machine, the layout, the dwell
detector and the frame loop. Everything that decides where things go or what text to show
is a pure function over a small state struct:

- `gz_view_layout(const struct gz_screen *, struct gz_view_layout *)` computes the box
  rectangle, the bar rectangle, the readout baseline and the target centre and radii.
- `gz_view_eye_px(layout, box_xy)` maps a track box value to a pixel.
- `gz_view_bar_px(layout, box_z)` maps box z to a pixel on the bar.
- `gz_view_floor(fit, floor_mm)` returns the box z for the 520 mm floor from the running
  linear fit of box z against eye z.
- `gz_dwell_feed(struct gz_dwell *, inside, now_ns)` returns 1 exactly once when the gaze
  has stayed inside for `GZ_DWELL_NS` (1.5 s), and resets whenever `inside` is 0.
- `gz_view_step(struct gz_view *, event)` advances the state machine on Enter, Escape, a
  dwell fire, or a sweep result, and says which sweep to run next, if any.
- `gz_view_verdict_text(const struct gz_sweep_verdict *, char *, size_t)` renders the
  verdict block.

`gz_cmd_setup(sock, cfg, output)` in `view.c` is the only function that touches the client
and the stimulus, and it is the frame loop: poll, update, draw, sleep to the next frame.

`gaze-cal/src/stimulus.c`, extended:

- A Pixmap back buffer the size of the window. Every frame is drawn into it and copied to
  the window with one `XCopyArea`, since clearing and redrawing the window directly
  flickers.
- Primitives: `gz_stimulus_rect`, `gz_stimulus_disc`, `gz_stimulus_ring`,
  `gz_stimulus_text` (server default font, sized by `XLoadQueryFont` of a fixed name with
  a fallback to the default), `gz_stimulus_clear`, `gz_stimulus_present`.
- `gz_stimulus_key(s)` returns the next key press (Enter, Escape, other) or none, without
  blocking, from `XPending` and `XNextEvent`.
- `XGrabKeyboard` on open when input is requested, `XUngrabKeyboard` on close. The window
  is `override_redirect`, so KWin never focuses it and it would receive no key presses
  otherwise. The grab is released on every exit path, including a signal, because a held
  grab locks the keyboard for the whole session.

`gz_stimulus_show` keeps its behaviour for the existing commands, drawing straight to the
window, so nothing about `fit`, `accuracy`, `probe` or `calibrate` changes.

`gaze-cal/src/calibrate.c`, refactored: `gz_cmd_fit` and `gz_cmd_accuracy` split into
`gz_fit_core` and `gz_accuracy_core`, which fill a `struct gz_sweep_verdict` and return the
same return codes as today, and the existing `gz_cmd_*` wrappers, which call the core and
print exactly what they print now. The struct carries the return code, points used and
rejected, median and worst residual in px, whether it is within one degree, the fitted
gains and offsets, the head distance from the fitted seat in mm for verify, and a short
reason string on refusal. The reason strings reuse the text the cores already print.

`gaze-cal/src/main.c`: the `setup` command and its usage line. Exit codes match the other
stimulus commands: 0 when the user closed the view, 1 when the device disagrees with the
config, 3 when the geometry couldn't be read, 2 on a usage error.

## Data flow

The frame loop polls the client with a 10 ms timeout and takes one sample per frame, the
latest. From it, the track box fields drive the eye dots gated on per eye validity
(validity 0 means valid), the mean of the two eye z values drives the distance and the bar,
`gaze_point_2d_norm` drives the raw ring, and `gz_gaze_correct` drives the green ring when a
correction loaded. The dwell detector consumes the corrected point when there is one and the
raw point otherwise. The sample rate on the readout is frames per second over the last
second of host time.

The correction is loaded once when the view opens, through `gz_correction_load` against the
device's display area from the same readback gate the sweeps use, and reloaded after every
fit sweep, so the green ring appears the moment a fit succeeds. A stale form H file is
refused by the loader as today, and the readout says `no fit on disk (stale file refused)`.

Before a sweep starts the view closes its client, and it reconnects when the sweep returns.
The fit and accuracy cores open their own connection, and a second client that stops
reading for the 40 seconds of a sweep hits the daemon's backpressure timeout
(`BACKPRESSURE_TIMEOUT_MS`, 10 s, from patch 0008), which frees the slot and leaves the
view holding a dead socket. Closing first avoids that.

The view logs one line per second to `gaze-cal.log` while it is open: eye z, box position
for each eye, validity, and the raw and corrected gaze points. That makes a session in
front of it data rather than a picture, and it costs nothing.

## Error handling

- Daemon not running: the window opens, says so, and exits 1 on any key. Matching the
  other commands, nothing is drawn beyond the message.
- Link lost during the view: the readout says `reconnecting`, the eye dots go hollow, and
  the client reconnects the way `monitor` does, resubscribing on every reconnect.
- Geometry gate failure: the sweep refuses before drawing a dot and the verdict block shows
  the gate's message and `fix with tobiifreed --force-display-area`. The view stays open.
- Sweep refusals map to plain words from what the core already knows: the missing points
  and whether proximity or lights is the likelier cause (`gz_missing_cause`), the isotropy
  refusal, the post-refit refusal naming its point, and the two outlier refusal. Each ends
  with what to do next.
- Neither eye for one second: the readout hint above. Nothing else changes, and the target
  stays active, since a user who can't be seen will get a refusal with the reason.
- Escape during a sweep aborts it. The fit core's existing abort path writes nothing.
- Any exit path releases the keyboard grab, including SIGINT and SIGTERM, through a flag
  the frame loop checks, never from inside the handler.

## Testing

`gaze-cal/tests/test_view.c`, wired into `test` and `test-asan`, covers the pure half:

- The layout for 2560 x 1440 and for one other size: the box is a third of the height and
  4:3, the bar is beside it, the target centre is at (0.6, 0.85).
- Track box value to pixel: 0 and 1 land on the box edges, 0.5 on the centre lines.
- Bar mapping and the floor: with the measured pairs, 520 mm lands near box z 0.23.
- The dwell detector with an injected clock: enter, hold below the threshold, fire exactly
  once at the threshold, no second fire while still inside, reset on leave, and reset on a
  gap in samples.
- The state machine: every transition in the diagram, Escape from every state, refusal
  routing to "try again", and that no sweep is requested twice for one trigger.
- Verdict text for a success, a within one degree verify, an outside one degree verify, and
  each refusal reason.

`tests/test_calibrate.c` gains one test per split: the wrapper prints the numbers the core
returned. Existing sweep tests are unchanged and prove the split didn't move the fit.

`tests/mutate.sh` gains mutations for the dwell threshold, the reset on leave, each state
transition, and the client close before a sweep. `make check` must stay at 0 unexpected
survivors.

The live check needs the human: lean until an eye dot goes red, lean in until the bar
crosses the floor, block one eye with a hand, then run a fit and a verify from the screen
without touching the terminal. The verdict in the window must match the terminal's numbers.

## Facts this design rests on

- Room lights must be on and the seat must be about 600 mm back, no closer than 520.
  Measured in Task 13 and recorded in `CLAUDE.md`.
- Validity 0 means valid. `present_mask` is `0x003fffff` in every frame, including frames
  with no eyes, so nothing here gates on it.
- The device delivers 33.2 Hz, so a frame loop at that rate draws every sample.
- The raw error under form S's fit#9 parameters is smallest low on the screen, which is
  where the target goes. Computed above.
- The daemon evicts a client that stops reading for 10 s, which is why the view closes its
  client around a sweep.
