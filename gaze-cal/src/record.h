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

/* The brief's sixteen raw columns, then the six corrected ones, then the six
 * eye-origin ones. Kept as one literal so the header line and the tests can't
 * drift apart.
 *
 * The eye origins are in TRACKER MILLIMETRES and are written exactly as the
 * device sent them, including the plain 0.0 this firmware puts there when it
 * sees no eye. They are not corrected and they are never nan, because the
 * validity columns already say which eye is real and substituting nan would
 * lose the distinction between "no eye" and "a value we declined to write".
 *
 * They are here because form S is scoped per seating position and degrades at
 * about GZ_CORR_DEGRADE_PX_PER_MM per mm of head movement in the screen plane.
 * How much the head moved during real play is the one thing a later analysis
 * can't recover from a trace that omits it, and recovering it costs the human
 * another five-minute session. */
#define GZ_RECORD_HEADER \
    "host_ns,device_us,frame_counter,present_mask,validity_L,validity_R," \
    "lx,ly,rx,ry,combined_x,combined_y,unfiltered_x,unfiltered_y," \
    "pupil_L,pupil_R," \
    "corr_lx,corr_ly,corr_rx,corr_ry,corr_cx,corr_cy," \
    "eye_lx,eye_ly,eye_lz,eye_rx,eye_ry,eye_rz\n"

/* Twenty-eight fields. The widest plausible one is a %.6f of a normalised
 * coordinate at 12 bytes, and the widest legal one is a 20-digit host_ns. A
 * whole row of plausible values measures about 300 bytes, so 768 leaves better
 * than double the room. A sample wide enough to exceed it is refused rather
 * than truncated, because a half-written line makes every row after it
 * unparseable. */
#define GZ_RECORD_ROW_MAX 768

/* Formats one row into buf, including the trailing newline. Returns the length
 * written, or 0 when it does not fit, in which case buf is untouched.
 *
 * Pure, so the whole shape of a trace is testable without a socket. `corr` may
 * be NULL, and a correction whose valid flag is clear counts as NULL: every
 * corr_* field is then the literal text "nan", which Python's float() reads.
 * A corr_* field whose input eye is invalid is "nan" too. Raw fields, which
 * includes the six eye_* ones, are always written as the device sent them,
 * valid or not.
 *
 * A correction with a zero gain counts as none as well, on the same test
 * gz_gaze_correct applies to the combined point. The per-eye path calls
 * gz_correct_point, which divides without checking, so without it the two
 * halves of one row disagree: inf in corr_lx and nan in corr_cx. The bounds
 * and isotropy in gz_correction_check are the loader's policy and are NOT
 * re-applied here, since a fit gz_correction_load accepted must not be
 * silently nanned by the formatter.
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

/* Writes "<trace_path>.correction.conf" into buf. Returns 0, or -1 when it does
 * not fit. One spelling, so the file the corrected path writes and the file the
 * raw path removes cannot name different things. */
int gz_record_provenance_path(char *buf, size_t cap, const char *trace_path);

/* Removes the provenance file beside a trace, so a --raw recording over a path
 * that once held a corrected one cannot leave a correction file behind claiming
 * a fit produced it. Returns 0 when the file is gone or was never there, and -1
 * after printing why on any other failure.
 *
 * Separate from the recorder because it is the whole of the fix and it has to
 * be testable without a socket. */
int gz_record_clear_provenance(const char *trace_path);

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
