/* gaze-cal/src/record.h - the gameplay trace recorder.
 *
 * Task 16 fits the overlay's filter constants from a recorded osu session, so
 * this file decides what a trace says. Two things about it are not obvious.
 *
 * It records the PER-EYE points as well as the combined one. Spec section 8
 * consumes the per-eye fields, because gaze_point_2d_norm is already filtered
 * on-device; recording both is what lets Task 16 measure the device's own group
 * delay by cross-correlation, which spec section 10 needs and can't otherwise
 * obtain.
 *
 * It records the CORRECTED points beside the raw ones. The device's gaze
 * direction carries an isotropic gain of about 1.18 that only the host-side
 * form S correction removes, so a filter fitted against raw gaze is fitted
 * against a signal the overlay never sees. A trace without the corr_* columns
 * isn't a trace of what Plan 2 will render, which is why `record` refuses to
 * run without a correction unless the operator asks for --raw in writing.
 */
#ifndef GZ_RECORD_H
#define GZ_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "client.h"
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The brief's sixteen raw columns, then the six corrected ones. Kept as one
 * literal so the header line and the tests can't drift apart. */
#define GZ_RECORD_HEADER \
    "host_ns,device_us,frame_counter,present_mask,validity_L,validity_R," \
    "lx,ly,rx,ry,combined_x,combined_y,unfiltered_x,unfiltered_y," \
    "pupil_L,pupil_R," \
    "corr_lx,corr_ly,corr_rx,corr_ry,corr_cx,corr_cy\n"

/* Twenty-two fields. The widest plausible one is a %.6f of a normalised
 * coordinate at 12 bytes, and the widest legal one is a 20-digit host_ns, so
 * 512 leaves better than double the room. A sample wide enough to exceed it is
 * refused rather than truncated, because a half-written line makes every row
 * after it unparseable. */
#define GZ_RECORD_ROW_MAX 512

/* Formats one row into buf, including the trailing newline. Returns the length
 * written, or 0 when it does not fit, in which case buf is untouched.
 *
 * Pure, so the whole shape of a trace is testable without a socket. `corr` may
 * be NULL, and a correction whose valid flag is clear counts as NULL: every
 * corr_* field is then the literal text "nan", which Python's float() reads.
 * A corr_* field whose input eye is invalid is "nan" too. Raw fields are
 * always written as the device sent them, valid or not.
 *
 * Remember that validity == 0 means VALID. */
size_t gz_record_row(char *buf, size_t cap, const struct gz_gaze_sample *s,
                     const struct gz_correction *corr, uint64_t host_ns);

/* What to do about the correction, kept pure and separate from the loading so
 * the refusal is testable without a home directory. */
#define GZ_REC_CORRECTED 0
#define GZ_REC_REFUSE    1
#define GZ_REC_RAW       2

/* `load_rc` is gz_correction_load's result: 1 usable, 0 no file, -1 unusable.
 * `raw` is the operator's --raw. */
int gz_record_decide(int load_rc, int raw);

struct gz_record_opts {
    const char *sock;       /* daemon socket */
    const char *cfg;        /* daemon config, or NULL for the default path */
    const char *path;       /* the CSV to write */
    unsigned seconds;
    int raw;                /* record uncorrected, and say so loudly */
};

/* Connects, gates the geometry, loads the correction unless --raw, then writes
 * a row per delivered sample until the deadline or SIGINT. Returns 0 when at
 * least one row was written and the link held, 1 on a refusal or a link
 * failure, 2 on a usage error, 3 when the geometry could not be read. */
int gz_cmd_record(const struct gz_record_opts *o);

#ifdef __cplusplus
}
#endif

#endif
