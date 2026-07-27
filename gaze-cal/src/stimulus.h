/* gaze-cal/src/stimulus.h - the fullscreen calibration stimulus.
 *
 * Xlib is confined to stimulus.c. calibrate.c drives the stimulus through
 * gz_stim_ops and never links X, which is what lets the whole calibration
 * sequence be tested without a display. The pure half of the geometry,
 * struct gz_screen and gz_screen_point_px, lives in calibrate.h for the same
 * reason.
 *
 * The output geometry is READ FROM X AT RUNTIME, never hardcoded. The brief
 * for this task, the plan and CLAUDE.md all named a monitor that does not
 * exist on this machine ("DP-1-2" at +4000+0) while the real gameplay panel is
 * DP-2 at +4000+1440. Taking any of them literally would have drawn every
 * calibration dot 1440 px above where the eye was looking, and nothing
 * downstream can tell a wrong stimulus position from wrong gaze.
 */
#ifndef GZ_STIMULUS_H
#define GZ_STIMULUS_H

#include "calibrate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 20 px per the brief. At 4.34 px/mm on DP-2 and 600 mm of eye relief that is
 * 0.44 degrees, comfortably inside a fixation. The inner dot gives the eye
 * something precise to land on: a bare 20 px disc is wider than the fovea and
 * invites the gaze to wander inside it. */
#define GZ_STIM_DOT_PX   20
#define GZ_STIM_INNER_PX 4

/* The RandR output called `name`, or the X primary when name is NULL.
 * Returns 0, or -1 after printing why. Fails loudly rather than falling back
 * to the root window, which spans all three monitors here. */
int gz_screen_find(const char *name, struct gz_screen *out);

/* One window per process, so this hands back file-static state rather than
 * allocating. */
struct gz_stimulus;

struct gz_stimulus *gz_stimulus_open(const char *output);
const struct gz_screen *gz_stimulus_screen(const struct gz_stimulus *s);

/* Draws the dot and waits for the server to have processed it, so the caller's
 * settle timer starts when the dot is up rather than when the request was
 * queued. Returns 0, or -1. */
int gz_stimulus_show(struct gz_stimulus *s, double nx, double ny);
int gz_stimulus_blank(struct gz_stimulus *s);
void gz_stimulus_close(struct gz_stimulus *s);

#ifdef __cplusplus
}
#endif

#endif
