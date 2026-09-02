/* gaze-cal/src/calibrate.h - the nine-point calibration, the blob on disk, and
 * the measurements the persistence experiment is built from.
 *
 * Nothing here links X. The stimulus is injected as gz_stim_ops, so the whole
 * command sequence, including the retries and the bounds, is driven by
 * tests/test_calibrate.c against a socketpair with no display anywhere.
 *
 * Three contracts from earlier tasks are load-bearing in here and are repeated
 * where they bite:
 *   - one command at a time, because the client has a single response slot;
 *   - on GZ_CLIENT_TIMEOUT, reconnect, because a late err frame carries no
 *     cmd_type and would be charged to the next command;
 *   - a reconnect must re-run the status gate itself, because
 *     gz_client_reconnect goes through gz_client_init and clears have_status.
 */
#ifndef GZ_CALIBRATE_H
#define GZ_CALIBRATE_H

#include <stddef.h>
#include <stdint.h>

#include "client.h"
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- the screen the stimulus is drawn on ----------------
 *
 * Pure, and deliberately not in stimulus.h: the mapping from a normalised
 * calibration point to a pixel is a calibration concern, and keeping it out of
 * the translation unit that includes Xlib is what lets it be tested. */
struct gz_screen {
    char name[32];
    int x, y, w, h;        /* X11 root coordinates */
};

/* Rounds to the pixel whose centre is nearest nx*w, then clamps into the
 * output. Half-open, so 0.1 and 0.9 land symmetrically about the middle. */
void gz_screen_point_px(const struct gz_screen *s, double nx, double ny,
                        int *px, int *py);

/* ---------------- the nine points ---------------- */
#define GZ_CAL_POINTS 9
extern const double GZ_CAL_PTS[GZ_CAL_POINTS][2];

/* One degree is 45 px on DP-2 at 600 mm, which is the plan's yardstick. The
 * degrees a sweep prints are computed from the measured eye distance instead
 * whenever the frame carried one, so a session at a different distance is not
 * judged against someone else's geometry. Here rather than in calibrate.c
 * because the verdict fills and the setup view are both judged against it. */
#define GZ_ACC_TARGET_PX 45.0

/* ---------------- the blob on disk ----------------
 *
 * Nothing in the daemon persists a calibration. Its saved_cal is a
 * process-local replay buffer that a restart empties, so surviving a daemon
 * restart, or a reboot, is this file's job.
 *
 * Layout: [u32 magic][u32 version][u32 len][u32 crc][len bytes]. The magic and
 * the version are here because a bare len+crc file cannot tell a format change
 * from corruption, and because the CRC of a truncated file is simply the CRC
 * of a different, valid-looking blob. */
#define GZ_BLOB_RELDIR  ".local/share/tobii-gaze"
#define GZ_BLOB_RELNAME "calibration.bin"
#define GZ_BLOB_MAGIC   0x4c414354u    /* "TCAL" read little-endian */
#define GZ_BLOB_VERSION 1u
#define GZ_BLOB_HDR     16

int gz_blob_path(char *buf, size_t cap);

/* Reflected CRC-32, polynomial 0xEDB88320. The brief called this crc32c; it is
 * not, CRC-32C is the Castagnoli polynomial. The name here says what it is. */
uint32_t gz_crc32(const unsigned char *p, size_t n);

/* Writes through a temporary file and renames, so an interrupted save leaves
 * the previous calibration intact rather than a half-written one. Returns 0,
 * or -1 after printing why. Refuses a blob longer than GZ_CAL_BLOB_MAX. */
int gz_blob_save_to(const char *path, const unsigned char *blob, size_t n);

/* Returns the blob length, or -1. Refuses anything it cannot fully account
 * for: bad magic, unknown version, a length past GZ_CAL_BLOB_MAX or past cap,
 * a file whose size does not match the declared length, or a CRC mismatch.
 * A corrupt blob applied to the device is worse than no calibration, because
 * it is wrong rather than absent and nothing downstream can detect it. */
int gz_blob_load_from(const char *path, unsigned char *out, size_t cap);

int gz_blob_save(const unsigned char *blob, size_t n);
int gz_blob_load(unsigned char *out, size_t cap);

/* ---------------- sampling ----------------
 *
 * 512 entries is 15 s at the measured 33.2 Hz, well past any window here. */
#define GZ_SAMPLE_CAP 512

struct gz_samples {
    /* n_total is every frame the client parsed in the window. n_seen is the
     * subset this loop actually got to inspect, which is smaller whenever one
     * poll consumed several frames: the client keeps only the latest. The
     * validity fraction is therefore computed over n_seen, never over n_total,
     * or a burst of four arriving together would read as 25% eyes-open and
     * abort a perfectly good calibration. */
    unsigned n_total;
    unsigned n_seen;
    unsigned n_any;        /* of n_seen, at least one eye valid */
    unsigned n_both;       /* of n_seen, both eyes valid */
    unsigned n;            /* recorded gaze points, at least one eye valid */
    double x[GZ_SAMPLE_CAP], y[GZ_SAMPLE_CAP];
    unsigned nz;
    double z[GZ_SAMPLE_CAP];   /* eye origin z, tracker space, mm */
    unsigned nex, ney;
    double ex[GZ_SAMPLE_CAP], ey[GZ_SAMPLE_CAP];

    /* Paired accumulator for the correction fit. Only frames that carried BOTH
     * a gaze point and a reconstructible eye midpoint contribute, so the two
     * means are over the same frames. Fitting a mean gaze against a mean eye
     * taken over different frame sets would mix two head positions into one
     * observation. Sums rather than arrays: the fit needs the means and the
     * outlier rule works per point, not per frame. */
    unsigned nfit;
    double fit_gaze_sum[2];    /* gaze_point_2d_norm */
    double fit_eye_sum[3];     /* eye midpoint, tracker mm */
};

/* Polls for ms, recording one sample per new frame_counter. Returns 0, or a
 * GZ_CLIENT_* negative. Draining matters even when the samples are thrown
 * away: the receive accumulator holds about 41 gaze frames, so 1.2 s of not
 * reading backs the socket up and eventually gets the client dropped.
 *
 * `e` carries the eye-midpoint reconstruction across calls and may be NULL,
 * which scopes it to this one window. A caller that wants the first frames of
 * a window to be usable when only one eye is tracked must pass a state that has
 * already seen both eyes. gz_collect is the NULL case, kept because most
 * callers throw the samples away. */
int gz_collect_eyes(struct gz_client *c, unsigned ms, struct gz_samples *s,
                    struct gz_eye_state *e);
int gz_collect(struct gz_client *c, unsigned ms, struct gz_samples *s);

/* The share of inspected frames that had BOTH eyes, or 0 when too few frames
 * were inspected for that to be a measurement. Split out of gz_calibrate so
 * the denominator is testable: dividing by n_total instead would read a burst
 * of four frames arriving in one read as 25% eyes-open and abort a
 * calibration that was fine. */
double gz_valid_frac(const struct gz_samples *s);

struct gz_stat { double median, p05, p95; };

/* Sorts v in place. n == 0 yields all zeros. */
struct gz_stat gz_stat_of(double *v, unsigned n);

/* ---------------- the calibration ---------------- */

struct gz_stim_ops {
    void *ctx;
    /* Returns 0, or -1 to abort the run. */
    int (*show)(void *ctx, double nx, double ny);
};

struct gz_cal_opts {
    const char *sock_path;      /* reconnect target after a timeout, or NULL */
    unsigned settle_ms;         /* dot up, saccade and land */
    unsigned sample_ms;         /* validity window ending when the point is sent */
    double min_valid_frac;      /* both-eyes-valid share required; 0 disables */
    unsigned point_retries;
    /* Parameters rather than constants so the tests can drive the timeout path
     * without spending twenty seconds per case. GZ_CAL_DEFAULTS carries the
     * measured values below. */
    int start_timeout_ms, point_timeout_ms, finish_timeout_ms;
    /* How long to wait for gaze to resume after a command before calling the
     * stall a floor rather than a measurement. */
    int gap_wait_ms;
};
extern const struct gz_cal_opts GZ_CAL_DEFAULTS;

/* Below this many inspected frames the validity fraction is not a measurement,
 * so it counts as a failure and the point is retried. At 33.2 Hz a 300 ms
 * window carries about ten. */
#define GZ_CAL_MIN_SEEN 3

/* start_calibration and cal_apply drive a multi-step device conversation, and
 * finish_calibration additionally makes the device compute. None of them is
 * the ~30 ms single request the default timeout was measured against. */
#define GZ_CAL_START_TIMEOUT_MS  10000
#define GZ_CAL_POINT_TIMEOUT_MS   3000
#define GZ_CAL_FINISH_TIMEOUT_MS 20000
#define GZ_CAL_APPLY_TIMEOUT_MS  20000

/* How long to wait for gaze to resume after a command before calling the
 * stream stalled. Above GZ_WATCHDOG_NS on purpose: a stall this long is the
 * thing being measured, so the measurement must not stop short of it. */
#define GZ_CAL_GAP_WAIT_MS 1500

struct gz_cal_step {
    uint8_t cmd;
    double req_ms;         /* send to reply */
    double gap_ms;         /* the gaze inter-arrival gap straddling the reply */
};

struct gz_cal_result {
    struct gz_cal_step step[GZ_CAL_POINTS + 2];
    int nsteps;
    double max_gap_ms;
    double valid_frac[GZ_CAL_POINTS];
    unsigned retries;
    size_t blob_len;
    unsigned char blob[GZ_CAL_BLOB_MAX];
};

/* start_calibration, nine add_calibration_point, finish_calibration. Leaves
 * the blob in r->blob and does NOT apply it: the caller saves it first, so a
 * failure in cal_apply cannot lose a calibration the device already computed.
 *
 * finish_calibration is compute_and_apply then retrieve then close_realm
 * (tobiifree_core.zig calFinishPollInner), so the DEVICE is already calibrated
 * when this returns. The cal_apply that follows is what makes the DAEMON
 * remember the blob for its own reconnect replay and advertise
 * calibration_applied.
 *
 * Returns 0, or a negative. A failure leaves the device's calibration realm
 * open; the next start_calibration or cal_apply unlocks it again, so the
 * remedy is to re-run rather than anything special. */
int gz_calibrate(struct gz_client *c, const struct gz_stim_ops *stim,
                 const struct gz_cal_opts *o, struct gz_cal_result *r);

/* One cal_apply, waited for. Returns 0, or a negative. */
int gz_apply_blob(struct gz_client *c, const char *sock_path,
                  const unsigned char *blob, size_t n);

/* ---------------- the host-side correction ----------------
 *
 * The transform itself is in proto.c, because Plan 2's OBS plugin applies it
 * too. Only the CLI ever FITS one, so the fit and the file live here.
 *
 * Kept out of calibration.bin deliberately. That blob is the device's own
 * calibration; it has a different owner and a different lifetime, and merging
 * the two is exactly the class of conflation that has already cost this project
 * time. */
#define GZ_CORR_RELNAME "correction.conf"

/* One calibration point's worth of evidence: where the dot was, what the device
 * reported, and where the eye was over the SAME frames. */
struct gz_fit_input {
    double target[2];        /* normalised display coordinates */
    double reported[2];      /* mean gaze_point_2d_norm over the window */
    double eye_mm[3];        /* mean eye midpoint, tracker mm, same frames */
};

#define GZ_FIT_OK          0
#define GZ_FIT_ERR_POINTS  (-1)   /* fewer than four usable points */
#define GZ_FIT_ERR_FLAT    (-2)   /* the targets do not span an axis */
#define GZ_FIT_ERR_OUTLIER (-3)   /* more than one point past 3x the median */
#define GZ_FIT_ERR_BOUNDS  (-4)   /* gain or isotropy outside GZ_CORR_* */
#define GZ_FIT_ERR_REFIT   (-5)   /* a point still past 3x the median after the refit */

struct gz_fit_report {
    int    n_in, n_used;
    int    rejected[GZ_CAL_POINTS];   /* 1 where a point was dropped as an outlier */
    int    n_rejected;
    /* Index of the point that survived the rejection and still sat past 3x the
     * refit median, or -1. Set only on the GZ_FIT_ERR_REFIT path, and it is
     * what the refusal message names. */
    int    refit_outlier;
    /* Corrected-minus-target, in mm on the panel, for every input point,
     * including rejected ones. This is the acceptance quantity, not the
     * regression residual: they differ by the factor g. */
    double resid_mm[GZ_CAL_POINTS];
    double median_resid_mm, worst_resid_mm;
    double eye_z_mm;                  /* mean over the used points, provenance */
    /* Where the head was, in normalised display coordinates. Form S is scoped
     * per seating position, so this identifies which position the fit belongs
     * to, and a later sweep's distance from it predicts the degradation at
     * GZ_CORR_DEGRADE_PX_PER_MM. */
    double eye_proj[2];
};

/* ---------------- one nine-point sweep ----------------
 *
 * In the header rather than in calibrate.c because gz_corrected_stats reads a
 * sweep and tests/test_calibrate.c builds one by hand to check it. Filled by
 * the sweep runner, which is the only writer.
 *
 * `gaze_med` is the per-axis median the human table prints and `gaze_mean` is
 * the mean over the frames that carried both a gaze point and an eye
 * midpoint, which is what the fit and every corrected figure use. `n_fit` is
 * how many of those paired frames there were, so `n_fit == 0` means the point
 * carried no usable pair at all. */
struct gz_sweep_point {
    double target[2];        /* normalised display coordinates */
    double gaze_med[2];      /* per-axis median, what the human table prints */
    double gaze_mean[2];     /* mean over the paired frames, what the fit uses */
    double eye_mm[3];        /* mean eye midpoint over those same frames */
    unsigned frames, n_gaze, n_fit;
    double err_px;           /* from the median, or -1 when no gaze landed */
};

struct gz_sweep {
    struct gz_sweep_point pt[GZ_CAL_POINTS];
    int n_gaze_ok, n_fit_ok;
    double zbuf[GZ_SAMPLE_CAP];
    unsigned nz;
};

/* ---------------- what one sweep decided ---------------- */

#define GZ_VERDICT_FIT      1
#define GZ_VERDICT_ACCURACY 2

/* What one sweep decided, in the numbers the setup view draws. The cores fill
 * it and keep printing everything they print today, so this is the same result
 * in a form a window can show without a terminal. */
struct gz_sweep_verdict {
    int    kind;                /* GZ_VERDICT_FIT or GZ_VERDICT_ACCURACY */
    int    rc;                  /* the command's exit code, 0 on success */
    int    refused;             /* 1 when nothing usable came out of the sweep */
    int    n_used, n_rejected;
    double median_px, worst_px; /* corrected when a correction applied, else raw */
    int    within_one_degree;   /* median_px <= GZ_ACC_TARGET_PX */
    int    corrected;           /* accuracy: 1 when the figures are corrected ones */
    double gx, gy, bx, by;      /* fit only, zero otherwise */
    double moved_mm;            /* accuracy: head distance from the fitted seat, -1 unknown */
    char   reason[200];         /* plain words on refusal, "" otherwise */
    char   next[120];           /* what to do next, always set */
};

/* Corrected error statistics over one sweep, the numbers report_corrected
 * prints. Returns the number of points that carried both a gaze sample and an
 * eye position, 0 when none did (out is zeroed then). */
struct gz_corr_stats {
    unsigned n;
    double   median_px, worst_px, raw_median_px;
    double   moved_mm;          /* -1 when the fit recorded no seat */
};
unsigned gz_corrected_stats(const struct gz_sweep *sw, const struct gz_screen *scr,
                            const struct gz_correction *corr, struct gz_corr_stats *out);

/* Pure fills, so the words in the window come from the same place as the
 * words on the terminal. */
void gz_fit_verdict_fill(int fit_rc, const struct gz_fit_report *rep,
                         const struct gz_correction *corr, double mm_per_px,
                         struct gz_sweep_verdict *out);
void gz_missing_verdict_fill(const struct gz_sweep *sw, struct gz_sweep_verdict *out);
void gz_accuracy_verdict_fill(int have_corr, const struct gz_corr_stats *cs,
                              double raw_median_px, double raw_worst_px,
                              struct gz_sweep_verdict *out);
const char *gz_fit_err_text(int rc);

/* Two univariate ordinary least squares of reported on target, one per axis.
 * Form S: no eye term, because spec test 5.3 measured the eye-relative form
 * losing at a displaced seat. Each point's eye position is still read, for the
 * seating position the report and the file record.
 *
 * Rejects a point whose residual exceeds 3x the median and refits once. More
 * than one such point fails the whole sweep, because at that rate the sweep is
 * not measuring a fixation. After the refit, a survivor still past 3x the new
 * median fails it too, as GZ_FIT_ERR_REFIT: that is the second bad point its
 * own axis absorbed, which no per-axis rule and no isotropy bound can see.
 * Pure and fixed-size, so it can move into proto.c if Plan 2 ever needs to
 * fit. Returns GZ_FIT_OK or a GZ_FIT_ERR_*. */
int gz_correction_fit(const struct gz_fit_input *in, int n, struct gz_rect area,
                      struct gz_correction *out, struct gz_fit_report *rep);

/* Which cause to name when a sweep loses points, kept pure and separate from
 * the printing so it can be tested against the sweep that got it wrong.
 *
 * Blaming the room lights for a proximity failure costs a human a five-minute
 * round trip, and it has already done so: the run at 07:01 on 2026-07-27 lost
 * the whole top row with the eye at 486 mm, and the message sent the user to
 * the light switch. Below GZ_FIT_TOO_CLOSE_MM the top row leaves the trackbox,
 * and the sweep that resolved all nine points sat at 586 mm. */
#define GZ_FIT_TOO_CLOSE_MM 520.0

#define GZ_MISS_LIGHTS    0    /* nothing in the data points at proximity */
#define GZ_MISS_TOO_CLOSE 1    /* the top row alone is gone, and the head is close */
#define GZ_MISS_CLOSE     2    /* the head is close, but that is not the pattern */

/* `paired[i]` is nonzero where point i produced both a gaze sample and an eye
 * position, and `eye_z[i]` is that point's eye distance in mm, read only where
 * paired is nonzero. Writes the median distance over the paired points, or 0
 * when there are none. The first three points are the top row, because
 * GZ_CAL_PTS is row-major from the top left. */
int gz_missing_cause(const int *paired, const double *eye_z, int n, double *out_med_z);

/* Connect, gate the daemon's status, and prove the device holds the geometry
 * the config names, writing that rect to `out_want` when it does. Returns a
 * GZ_GATE_* code; only GZ_GATE_OK may measure anything. `cfg` may be NULL for
 * the default path. `require_geometry` clear waives a MISMATCH, which only the
 * probe may do, since the probe's job is to settle a field of the geometry and
 * it therefore runs before that geometry is right. */
int gz_connect_and_gate(struct gz_client *c, const char *sock, const char *cfg,
                        int require_geometry, struct gz_rect *out_want);

int gz_correction_path(char *buf, size_t cap);

/* Writes through a temporary file and renames, like the blob. `rep` may be
 * NULL; when present its numbers are appended as extra fit_* keys, which
 * gz_correction_parse does not model and therefore ignores. `mm_per_px` turns
 * the fit's millimetres into the pixels spec 4.1 names those keys in; the fit
 * cannot do it itself, because it knows nothing about a panel's resolution.
 * Returns 0, or -1 after printing why. */
int gz_correction_save_to(const char *path, const struct gz_correction *c,
                          const struct gz_fit_report *rep, double eye_z_mm,
                          double mm_per_px);
int gz_correction_save(const struct gz_correction *c,
                       const struct gz_fit_report *rep, double eye_z_mm,
                       double mm_per_px);

/* Returns 1 when a usable correction was read, 0 when there is no file, and -1
 * when a file exists but is unusable, having said so on stderr. `want` is the
 * display area the device is currently holding: a correction fitted against a
 * different geometry is meaningless and is refused rather than applied. */
int gz_correction_load_from(const char *path, struct gz_rect want,
                            struct gz_correction *out);
int gz_correction_load(struct gz_rect want, struct gz_correction *out);

/* ---------------- CLI entries ----------------
 *
 * The stimulus arrives already open, because opening it is the only part that
 * needs Xlib and main.c is the only file that links it. */
int gz_cmd_calibrate(const char *sock, const char *cfg,
                     const struct gz_stim_ops *stim, const struct gz_screen *scr);
int gz_cmd_accuracy(const char *sock, const char *cfg, const char *label,
                    const struct gz_stim_ops *stim, const struct gz_screen *scr);
int gz_cmd_fit(const char *sock, const char *cfg,
               const struct gz_stim_ops *stim, const struct gz_screen *scr);

/* The cores behind the two commands above. Same behaviour and same printed
 * output, plus the filled verdict. `out` is always written, including on a
 * gate failure, where rc carries the gate code and refused is 1. */
int gz_fit_core(const char *sock, const char *cfg, const struct gz_stim_ops *stim,
                const struct gz_screen *scr, struct gz_sweep_verdict *out);
int gz_accuracy_core(const char *sock, const char *cfg, const char *label,
                     const struct gz_stim_ops *stim, const struct gz_screen *scr,
                     struct gz_sweep_verdict *out);
int gz_cmd_probe(const char *sock, const char *cfg,
                 const struct gz_stim_ops *stim, const struct gz_screen *scr);
int gz_cmd_apply_saved(const char *sock);
int gz_cmd_preview(const char *sock, long nsamples);

#ifdef __cplusplus
}
#endif

#endif
