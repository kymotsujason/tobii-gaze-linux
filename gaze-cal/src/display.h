/* gaze-cal/src/display.h - the display area readback gate.
 *
 * Calibration is computed in the frame the display area defines, so a wrong
 * frame yields gaze that looks plausible and is wrong everywhere, and nothing
 * downstream can detect it or correct it. This is the last place it can be
 * caught, and it is caught by reading the geometry back off the device rather
 * than by trusting anyone's report of it.
 *
 * Specifically: Tracker.setDisplayArea (driver/src/tracker.zig) returns true as
 * soon as the SEND succeeds. It calls queryDisplayArea afterwards and assigns
 * the result, but never compares it to what it asked for, so a "success" from
 * the daemon is not evidence the device took the value. The readback here is
 * what turns it into evidence.
 *
 * Unlike proto.c this file prints and reads files, so Plan 2's OBS plugin does
 * not link it. The pure half, gz_decode_display_area / gz_corners_to_rect /
 * gz_rect_diff, lives in proto.c where the plugin can reach it.
 */
#ifndef GZ_DISPLAY_H
#define GZ_DISPLAY_H

#include "client.h"
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 mm on a 597 mm panel is 0.17%, well inside anything that matters for gaze
 * and far outside the Q42 quantisation of 2^-42 mm. The device round-trips
 * exact integers today; the tolerance exists for a future config in tenths,
 * not to absorb a real disagreement. */
#define GZ_DA_TOL_MM 1.0

/* Tilt is not measured in mm, so it cannot share the mm tolerance. 0.5 degrees
 * over a 336 mm panel moves the top edge by 2.9 mm. */
#define GZ_DA_TOL_DEG 0.5

/* Where the daemon reads its geometry from: $HOME/.config/tobii.json,
 * CONFIG_PATH in applications/tobiifreed/src/main.zig. Reading the same file
 * is the point. The gate asserts "the device holds what the daemon was told",
 * which is precisely the proposition setDisplayArea fails to establish. It
 * does NOT assert that the file is right: z_mm, tilt, cx and cy in the shipped
 * config are template defaults rather than measurements, and no readback can
 * tell the difference. That is Task 13's human obligation. */
#define GZ_CONFIG_RELPATH ".config/tobii.json"

/* Returns 0, or -1 if $HOME is unset or the path does not fit. */
int gz_config_path(char *buf, size_t cap);

/* Parses display_area out of the daemon's config. Returns 0 on success, -1 if
 * the file cannot be read, -2 if it does not parse.
 *
 * REFUSES rather than defaults, which is the one place this deliberately
 * diverges from the daemon. loadDisplayArea() in main.zig swallows every
 * failure with `catch return .{}` and silently uses the 1500x1000 template, so
 * a typo there becomes a geometry rather than an error. A gate that did the
 * same would compare the device against a number nobody chose. */
int gz_config_load_rect(const char *path, struct gz_rect *out);

/* The cx/cy anchor grammar from main.zig evalAnchorExpr: an anchor letter
 * (t/b/l/r/c) optionally followed by + or - and a number. Returns 0 and writes
 * the offset from the area's centre, or -1. Exposed for its tests. */
int gz_parse_anchor_expr(const char *s, double half, int is_vertical, double *out);

/* Prints the comparison and returns 0 only when every field agrees. Prints the
 * refusal text on a mismatch: the operator has to be told what to do, because
 * the remedy is a daemon flag rather than anything gaze-cal can do itself. */
int gz_display_verify(const double got[9], struct gz_rect want,
                      double tol_mm, double tol_deg);

/* One get_display_area against a connected client, decoded into out[9].
 * Returns 0, or one of the GZ_CLIENT_* negatives. Retries usb_busy, which
 * means nothing reached the device, and never retries on the same connection
 * after a timeout, because after a timeout the two sides are out of step and
 * a late err frame would be misattributed to the retry.
 *
 * `path` is the socket to reconnect through, or NULL to give up instead. The
 * timeout is a parameter so the tests can exercise the give-up path without
 * spending GZ_CLIENT_CMD_TIMEOUT_MS on every case. */
int gz_display_read(struct gz_client *c, const char *path, int timeout_ms,
                    double out[9]);

/* Status must arrive and must be a protocol version this build understands
 * before any command is sent. Returns 0, or -1 after printing why. */
int gz_display_gate_status(struct gz_client *c, int timeout_ms);

/* Kept apart because only one of them has a remedy. MISMATCH means the device
 * disagrees with the config, which --force-display-area fixes. UNKNOWN means
 * the geometry could not be established at all, and forcing would then write a
 * value nobody has checked. A caller that folded the two together would offer
 * the wrong advice on a dead daemon. Also the process exit codes. */
#define GZ_GATE_OK       0
#define GZ_GATE_MISMATCH 1
#define GZ_GATE_UNKNOWN  3

/* status gate, then read, then verify. What Task 13 calls. Returns one of the
 * GZ_GATE_* codes; only GZ_GATE_OK may proceed to calibration. */
int gz_display_gate(struct gz_client *c, const char *path, struct gz_rect want,
                    double tol_mm, double tol_deg);

/* The `display` subcommand. */
int gz_cmd_display(const char *sock_path, const char *cfg_path, double tol_mm);

#ifdef __cplusplus
}
#endif

#endif
