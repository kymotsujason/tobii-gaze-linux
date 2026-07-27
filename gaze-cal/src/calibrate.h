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
};

/* Polls for ms, recording one sample per new frame_counter. Returns 0, or a
 * GZ_CLIENT_* negative. Draining matters even when the samples are thrown
 * away: the receive accumulator holds about 41 gaze frames, so 1.2 s of not
 * reading backs the socket up and eventually gets the client dropped. */
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

/* ---------------- CLI entries ----------------
 *
 * The stimulus arrives already open, because opening it is the only part that
 * needs Xlib and main.c is the only file that links it. */
int gz_cmd_calibrate(const char *sock, const char *cfg,
                     const struct gz_stim_ops *stim, const struct gz_screen *scr);
int gz_cmd_accuracy(const char *sock, const char *cfg, const char *label,
                    const struct gz_stim_ops *stim, const struct gz_screen *scr);
int gz_cmd_probe(const char *sock, const char *cfg,
                 const struct gz_stim_ops *stim, const struct gz_screen *scr);
int gz_cmd_apply_saved(const char *sock);
int gz_cmd_preview(const char *sock, long nsamples);

#ifdef __cplusplus
}
#endif

#endif
