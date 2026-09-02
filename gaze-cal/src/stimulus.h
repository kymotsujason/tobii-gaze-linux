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
 * allocating. Opening again in the SAME mode returns the open window, and
 * opening in the other mode returns NULL after printing why, since a plain
 * handle cannot draw or take keys and a second owner of the input window would
 * ungrab the keyboard out from under it. Close the one you have first. */
struct gz_stimulus;

struct gz_stimulus *gz_stimulus_open(const char *output);
const struct gz_screen *gz_stimulus_screen(const struct gz_stimulus *s);

/* Draws the dot and waits for the server to have processed it, so the caller's
 * settle timer starts when the dot is up rather than when the request was
 * queued. Returns 0, or -1. */
int gz_stimulus_show(struct gz_stimulus *s, double nx, double ny);
int gz_stimulus_blank(struct gz_stimulus *s);
void gz_stimulus_close(struct gz_stimulus *s);

#define GZ_KEY_NONE   0
#define GZ_KEY_ENTER  1
#define GZ_KEY_ESCAPE 2
#define GZ_KEY_OTHER  3

/* Like gz_stimulus_open, plus a back buffer, key events and a keyboard grab.
 * The window is override_redirect, so KWin never focuses it: without the grab
 * it receives no key press at all. gz_stimulus_close releases the grab, and so
 * must every exit path, because a held grab locks the keyboard for the whole
 * session. Returns NULL after printing why, including when a plain window is
 * already open. */
struct gz_stimulus *gz_stimulus_open_input(const char *output);

/* The next key press, or GZ_KEY_NONE. Never blocks. */
int gz_stimulus_key(struct gz_stimulus *s);

/* Back buffer drawing, window coordinates, colours as 0xRRGGBB. Nothing shows
 * until gz_stimulus_present copies the buffer to the window. The existing
 * gz_stimulus_show draws straight to the window and is unchanged. */
void gz_stimulus_clear(struct gz_stimulus *s);
void gz_stimulus_rect(struct gz_stimulus *s, int x, int y, int w, int h,
                      unsigned long rgb, int filled);
void gz_stimulus_disc(struct gz_stimulus *s, int cx, int cy, int r, unsigned long rgb);
/* An arc of `degrees` out of 360, starting at the top and running clockwise,
 * `width` pixels thick. 360 is a full ring. */
void gz_stimulus_ring(struct gz_stimulus *s, int cx, int cy, int r, int width,
                      unsigned long rgb, int degrees);
/* Left-aligned at (x, y) with y the baseline. Returns the width drawn in px.
 * The width already accounts for the upscale described in stimulus.c, so a
 * caller lays text out with this and gz_stimulus_text_height and never needs
 * to know the scale. */
int  gz_stimulus_text(struct gz_stimulus *s, int x, int y, const char *text,
                      unsigned long rgb);
int  gz_stimulus_text_height(const struct gz_stimulus *s);
/* What gz_stimulus_text would return, without drawing anything. Font metrics
 * are client side, so this costs no round trip, where measuring by drawing off
 * screen costs an XGetImage per call and the setup view centres text on every
 * frame. */
int  gz_stimulus_text_width(const struct gz_stimulus *s, const char *text);
void gz_stimulus_present(struct gz_stimulus *s);

#ifdef __cplusplus
}
#endif

#endif
