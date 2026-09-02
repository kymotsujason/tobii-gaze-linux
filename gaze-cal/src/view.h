/* gaze-cal/src/view.h - the fullscreen track box setup view.
 *
 * Everything declared here is pure. The frame loop that owns the window and
 * the socket is the only part of view.c that isn't, which is what lets
 * tests/test_view.c drive the layout, the dwell, the state machine and the
 * sweep sequencing with no display and no tracker anywhere.
 *
 * The sweeps are run through gz_view_io rather than called directly for one
 * reason that matters: a fit or a verify opens its own client, so this view's
 * client has to be CLOSED before the sweep starts and reconnected after. Both
 * clients live on the same socket, and the daemon serves one command at a
 * time. Putting that order behind callbacks is what makes it testable.
 */
#ifndef GZ_VIEW_H
#define GZ_VIEW_H

#include <stdint.h>
#include <stddef.h>

#include "calibrate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The floor line drawn across the track box. Below GZ_FIT_TOO_CLOSE_MM the top
 * row of the nine points leaves the box, so a seat under this distance is what
 * the view is there to prevent. */
#define GZ_VIEW_FLOOR_MM      520.0
#define GZ_VIEW_TARGET_NX     0.6
#define GZ_VIEW_TARGET_NY     0.85
#define GZ_VIEW_TARGET_R_PX   150
#define GZ_VIEW_ACCEPT_R_PX   250
#define GZ_DWELL_NS           1500000000ULL
#define GZ_DWELL_GAP_NS        250000000ULL
#define GZ_VIEW_NO_EYES_NS    1000000000ULL

struct gz_view_layout {
    int box_x, box_y, box_w, box_h;         /* window coordinates */
    int bar_x, bar_y, bar_w, bar_h;
    int readout_x, readout_y;               /* baseline of the readout line */
    int verdict_x, verdict_y;               /* baseline of the first verdict line */
    int target_cx, target_cy, target_r, accept_r;
    int eye_r;                              /* eye dot radius */
};
void gz_view_layout(const struct gz_screen *scr, struct gz_view_layout *out);

/* Track box value (x across, y down, both 0 to 1) to a pixel inside the box.
 * Clamped to the box, so a value the device reports slightly past 1 stays
 * visible on the edge rather than off the box. */
void gz_view_eye_px(const struct gz_view_layout *l, double box_x, double box_y,
                    int *px, int *py);
/* Box z (0 near, 1 far) to a y pixel on the bar, 0 at the bottom. */
int gz_view_bar_py(const struct gz_view_layout *l, double box_z);

/* Running least squares of eye z (mm) against box z. Seeded with the pairs
 * measured on 2026-09-01 so the floor line is in a sensible place before the
 * first live sample, and refined by every valid sample after. */
struct gz_zfit { double sx, sy, sxx, sxy; unsigned n; };
void   gz_zfit_init(struct gz_zfit *f);
void   gz_zfit_add(struct gz_zfit *f, double box_z, double z_mm);
double gz_zfit_box_z(const struct gz_zfit *f, double z_mm);   /* inverse map */

/* Fires exactly once after GZ_DWELL_NS continuously inside. Leaving, or a gap
 * longer than GZ_DWELL_GAP_NS between feeds, resets it. */
struct gz_dwell { uint64_t since_ns, last_ns; int fired; };
void gz_dwell_init(struct gz_dwell *d);
int  gz_dwell_feed(struct gz_dwell *d, int inside, uint64_t now_ns);
/* 0 to 360, how much of the ring to draw. */
int  gz_dwell_degrees(const struct gz_dwell *d, uint64_t now_ns);

enum gz_view_state {
    GZ_VIEW_IDLE, GZ_VIEW_FIT_SWEEP, GZ_VIEW_FIT_VERDICT,
    GZ_VIEW_VERIFY_SWEEP, GZ_VIEW_VERIFY_VERDICT, GZ_VIEW_CLOSED
};
enum gz_view_event  { GZ_EV_NONE, GZ_EV_TRIGGER, GZ_EV_ESCAPE, GZ_EV_SWEEP_DONE };
enum gz_view_action { GZ_ACT_NONE, GZ_ACT_RUN_FIT, GZ_ACT_RUN_VERIFY, GZ_ACT_CLOSE };

struct gz_view {
    enum gz_view_state state;
    struct gz_sweep_verdict verdict;
    int have_verdict;
};
void gz_view_init(struct gz_view *v);
/* `result` is read only for GZ_EV_SWEEP_DONE. */
enum gz_view_action gz_view_step(struct gz_view *v, enum gz_view_event ev,
                                 const struct gz_sweep_verdict *result);
/* "fit", "verify", "try again", "fit again", or "" while a sweep runs. */
const char *gz_view_target_word(const struct gz_view *v);
int gz_view_in_sweep(const struct gz_view *v);

/* The verdict block, up to four lines separated by '\n'. Returns the length. */
size_t gz_view_verdict_text(const struct gz_sweep_verdict *v, char *buf, size_t cap);

/* The readout line. `fit_stamp` is the correction's fit_utc or NULL. */
size_t gz_view_readout_text(double z_mm, int l_valid, int r_valid, double hz,
                            const char *fit_stamp, int stale_file, int no_eyes,
                            int reconnecting, char *buf, size_t cap);

/* Sweep sequencing, with I/O behind callbacks so the order is testable: close
 * the client, run the sweep, reload the correction after a successful fit,
 * reconnect, then feed the result to the state machine. Returns the action
 * the state machine wants next, normally GZ_ACT_NONE. */
struct gz_view_io {
    void *ctx;
    void (*close_client)(void *ctx);
    int  (*reconnect_client)(void *ctx);
    int  (*run_fit)(void *ctx, struct gz_sweep_verdict *out);
    int  (*run_verify)(void *ctx, struct gz_sweep_verdict *out);
    void (*reload_correction)(void *ctx);
};
enum gz_view_action gz_view_run_action(struct gz_view *v, enum gz_view_action act,
                                       const struct gz_view_io *io);

/* Reads the fit_utc line out of a correction file into `buf`, "" when the
 * file or the key is absent. Returns 1 when found. */
int gz_view_fit_stamp(const char *path, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
