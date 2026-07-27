/* gaze-cal/src/display.c - see display.h. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "display.h"

/* ---------------- config: $HOME/.config/tobii.json ----------------
 *
 * A hand-rolled reader rather than a dependency, because gaze-cal has none and
 * the shape read is six keys of one object. It is strict on purpose: anything
 * it does not understand is an error, never a default. */

/* main.zig loadDisplayArea reads the config into a [4096]u8 and parses exactly
 * that, so a longer file is already truncated by the time the daemon sees it.
 * Matching the bound keeps the two readers looking at the same bytes. Past it
 * this refuses, where the daemon parses the truncated prefix, fails, and falls
 * back to its template. */
#define GZ_CFG_MAX 4096

struct js {
    const char *s;
    size_t n, pos;
};

static void js_ws(struct js *j) {
    while (j->pos < j->n) {
        char c = j->s[j->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        j->pos++;
    }
}

static int js_ch(struct js *j, char c) {
    js_ws(j);
    if (j->pos >= j->n || j->s[j->pos] != c) return 0;
    j->pos++;
    return 1;
}

/* Copies the string into out and NUL-terminates. out may be NULL to skip.
 * \u is refused: the keys and values here are ASCII, and half-decoding a
 * surrogate pair would be a silent wrong answer. */
static int js_string(struct js *j, char *out, size_t cap) {
    js_ws(j);
    if (j->pos >= j->n || j->s[j->pos] != '"') return 0;
    j->pos++;
    size_t w = 0;
    while (j->pos < j->n) {
        char c = j->s[j->pos++];
        if (c == '"') {
            if (out) {
                if (w >= cap) return 0;
                out[w] = '\0';
            }
            return 1;
        }
        if (c == '\\') {
            if (j->pos >= j->n) return 0;
            char e = j->s[j->pos++];
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            default:   return 0;
            }
        }
        if (out) {
            if (w + 1 >= cap) return 0;
            out[w++] = c;
        }
    }
    return 0;
}

static int js_number(struct js *j, double *out) {
    js_ws(j);
    if (j->pos >= j->n) return 0;
    /* strtod on a non-NUL-terminated span is not safe, so the caller
     * guarantees termination by reading the file into an oversized buffer. */
    char *end = NULL;
    errno = 0;
    double v = strtod(j->s + j->pos, &end);
    if (end == j->s + j->pos) return 0;
    j->pos = (size_t)(end - j->s);
    if (out) *out = v;
    return 1;
}

static int js_skip_value(struct js *j, int depth);

static int js_skip_object(struct js *j, int depth) {
    if (!js_ch(j, '{')) return 0;
    js_ws(j);
    if (js_ch(j, '}')) return 1;
    for (;;) {
        if (!js_string(j, NULL, 0)) return 0;
        if (!js_ch(j, ':')) return 0;
        if (!js_skip_value(j, depth + 1)) return 0;
        if (js_ch(j, ',')) continue;
        return js_ch(j, '}');
    }
}

static int js_skip_array(struct js *j, int depth) {
    if (!js_ch(j, '[')) return 0;
    js_ws(j);
    if (js_ch(j, ']')) return 1;
    for (;;) {
        if (!js_skip_value(j, depth + 1)) return 0;
        if (js_ch(j, ',')) continue;
        return js_ch(j, ']');
    }
}

static int js_lit(struct js *j, const char *lit) {
    size_t l = strlen(lit);
    if (j->n - j->pos < l) return 0;
    if (memcmp(j->s + j->pos, lit, l) != 0) return 0;
    j->pos += l;
    return 1;
}

/* Bounded so that a file of nothing but '[' cannot recurse until the stack
 * runs out. Nothing legitimate in this config nests at all. */
#define JS_MAX_DEPTH 32

static int js_skip_value(struct js *j, int depth) {
    if (depth > JS_MAX_DEPTH) return 0;
    js_ws(j);
    if (j->pos >= j->n) return 0;
    char c = j->s[j->pos];
    if (c == '{') return js_skip_object(j, depth);
    if (c == '[') return js_skip_array(j, depth);
    if (c == '"') return js_string(j, NULL, 0);
    if (c == 't') return js_lit(j, "true");
    if (c == 'f') return js_lit(j, "false");
    if (c == 'n') return js_lit(j, "null");
    return js_number(j, NULL);
}

int gz_parse_anchor_expr(const char *s, double half, int is_vertical, double *out) {
    /* main.zig evalAnchorExpr, term for term. */
    size_t i = 0;
    while (s[i] == ' ') i++;
    if (s[i] == '\0') return -1;

    double anchor;
    switch (s[i]) {
    case 't': if (!is_vertical) return -1; anchor =  half; break;
    case 'b': if (!is_vertical) return -1; anchor = -half; break;
    case 'l': if ( is_vertical) return -1; anchor = -half; break;
    case 'r': if ( is_vertical) return -1; anchor =  half; break;
    case 'c': anchor = 0; break;
    default:  return -1;
    }
    i++;

    while (s[i] == ' ') i++;
    if (s[i] == '\0') { *out = anchor; return 0; }

    double sign;
    if (s[i] == '+')      sign =  1;
    else if (s[i] == '-') sign = -1;
    else return -1;
    i++;

    while (s[i] == ' ') i++;
    if (s[i] == '\0') return -1;

    char *end = NULL;
    errno = 0;
    double num = strtod(s + i, &end);
    /* Zig's parseFloat consumes the whole slice or errors, so trailing text is
     * a parse failure there too. Refusing a superset the daemon would refuse
     * keeps the two from ever disagreeing on a value. */
    if (end == s + i || *end != '\0') return -1;

    *out = anchor + sign * num;
    return 0;
}

int gz_config_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') return -1;
    int n = snprintf(buf, cap, "%s/%s", home, GZ_CONFIG_RELPATH);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

int gz_config_load_rect(const char *path, struct gz_rect *out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;

    /* One byte past the bound, so a file of exactly GZ_CFG_MAX is accepted and
     * anything longer is detected by the read itself rather than by feof,
     * which is not set when a read stops exactly on the last byte. */
    char text[GZ_CFG_MAX + 2];
    size_t n = fread(text, 1, GZ_CFG_MAX + 1, f);
    fclose(f);
    if (n > GZ_CFG_MAX) return -2;
    text[n] = '\0';

    struct js j = { text, n, 0 };

    /* Tracker.DisplayArea's own defaults, so an absent key means what it means
     * to the daemon. */
    struct gz_rect r = { 1500, 1000, -750, -500, 0, 0 };

    int have_cx = 0, have_cy = 0;
    double cx = 0, cy = 0;
    char cx_expr[64], cy_expr[64];
    int cx_is_expr = 0, cy_is_expr = 0;

    if (!js_ch(&j, '{')) return -2;
    int found = 0;
    js_ws(&j);
    if (!js_ch(&j, '}')) {
        for (;;) {
            char key[64];
            if (!js_string(&j, key, sizeof key)) return -2;
            if (!js_ch(&j, ':')) return -2;

            if (strcmp(key, "display_area") == 0) {
                if (found) return -2;
                found = 1;
                if (!js_ch(&j, '{')) return -2;
                js_ws(&j);
                if (!js_ch(&j, '}')) {
                    for (;;) {
                        char k[64];
                        if (!js_string(&j, k, sizeof k)) return -2;
                        if (!js_ch(&j, ':')) return -2;

                        if (strcmp(k, "w_mm") == 0) {
                            if (!js_number(&j, &r.w_mm)) return -2;
                        } else if (strcmp(k, "h_mm") == 0) {
                            if (!js_number(&j, &r.h_mm)) return -2;
                        } else if (strcmp(k, "z_mm") == 0) {
                            if (!js_number(&j, &r.z_mm)) return -2;
                        } else if (strcmp(k, "tilt") == 0) {
                            if (!js_number(&j, &r.tilt_deg)) return -2;
                        } else if (strcmp(k, "cx") == 0 || strcmp(k, "cy") == 0) {
                            int vert = (k[1] == 'y');
                            char *buf  = vert ? cy_expr : cx_expr;
                            double *num = vert ? &cy : &cx;
                            int *is_expr = vert ? &cy_is_expr : &cx_is_expr;
                            js_ws(&j);
                            if (j.pos < j.n && j.s[j.pos] == '"') {
                                if (!js_string(&j, buf, 64)) return -2;
                                *is_expr = 1;
                            } else {
                                if (!js_number(&j, num)) return -2;
                                *is_expr = 0;
                            }
                            if (vert) have_cy = 1; else have_cx = 1;
                        } else if (!js_skip_value(&j, 1)) {
                            return -2;
                        }

                        if (js_ch(&j, ',')) continue;
                        if (!js_ch(&j, '}')) return -2;
                        break;
                    }
                }
            } else if (!js_skip_value(&j, 1)) {
                return -2;
            }

            if (js_ch(&j, ',')) continue;
            if (!js_ch(&j, '}')) return -2;
            break;
        }
    }
    js_ws(&j);
    if (j.pos != j.n) return -2;
    if (!found) return -2;

    /* main.zig computes the halves from w_mm/h_mm regardless of key order,
     * because it reads the whole object into a map first. Deferring the
     * conversion to here reproduces that: a config listing cx before w_mm gets
     * the same origin from both readers.
     *
     * An absent cx or cy leaves ox or oy at the 1500x1000 template's value,
     * which is what the daemon does and is a trap worth naming: a config with
     * w_mm but no cx is centred for a 1500 mm panel, not for its own. */
    double half_w = r.w_mm / 2.0, half_h = r.h_mm / 2.0;
    if (have_cx) {
        if (cx_is_expr && gz_parse_anchor_expr(cx_expr, half_w, 0, &cx) != 0) return -2;
        r.ox_mm = -cx - half_w;
    }
    if (have_cy) {
        if (cy_is_expr && gz_parse_anchor_expr(cy_expr, half_h, 1, &cy) != 0) return -2;
        r.oy_mm = -cy - half_h;
    }

    *out = r;
    return 0;
}

/* ---------------- the gate ---------------- */

static void print_rect(const char *label, struct gz_rect r) {
    fprintf(stderr, "  %-8s %.1f x %.1f mm  origin=(%.1f, %.1f)  z=%.1f  tilt=%.2f\n",
            label, r.w_mm, r.h_mm, r.ox_mm, r.oy_mm, r.z_mm, r.tilt_deg);
}

static void print_corners(const double c[9]) {
    fprintf(stderr, "  corners  TL=(%.1f,%.1f,%.1f) TR=(%.1f,%.1f,%.1f) BL=(%.1f,%.1f,%.1f)\n",
            c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
}

int gz_display_verify(const double got[9], struct gz_rect want,
                      double tol_mm, double tol_deg) {
    /* Before converting, not after. gz_corners_to_rect takes the width from the
     * top edge and the origin from bl, so a quad the daemon could never have
     * written collapses into a rectangle that matches neither edge and could
     * agree with the config by accident. */
    if (!gz_corners_are_rectangular(got, tol_mm)) {
        fprintf(stderr, "display area: NOT A RECTANGLE\n");
        print_corners(got);
        fprintf(stderr,
            "REFUSING TO CALIBRATE. setDisplayArea always writes tl.x = bl.x and a\n"
            "level top edge, so these corners did not come from any geometry the\n"
            "daemon set. Converting them would produce a rectangle matching neither\n"
            "edge. Read the device again, and if it persists, restart the daemon as\n"
            "tobiifreed --force-display-area.\n");
        return -1;
    }

    struct gz_rect r = gz_corners_to_rect(got);
    unsigned d = gz_rect_diff(r, want, tol_mm, tol_deg);

    fprintf(stderr, "display area: %s\n", d == 0 ? "OK" : "MISMATCH");
    print_rect("device", r);
    print_rect("config", want);
    print_corners(got);

    if (d == 0) {
        /* THE CAVEAT FOLLOWS THE VERDICT, not the CLI. gz_display_gate is the
         * library entry Task 13 was told to use, and an OK from it is the only
         * thing standing between a calibration and a wrong one. A readback
         * cannot tell an unmeasured 0 from a measured one, so whoever sees the
         * OK has to see this too. */
        fprintf(stderr,
            "NOTE: this proves the device holds what the config asks for. It does\n"
            "not prove the config is right. z_mm, tilt, cx and cy are the daemon's\n"
            "template defaults unless somebody measured them, and no readback can\n"
            "tell an unmeasured 0 from a measured one.\n");
        return 0;
    }

    /* Named individually. "MISMATCH" alone sends the operator to remeasure the
     * panel when the disagreement is an origin they never set. */
    fprintf(stderr, "  differs:");
    if (d & GZ_DA_DIFF_W)    fprintf(stderr, " width");
    if (d & GZ_DA_DIFF_H)    fprintf(stderr, " height");
    if (d & GZ_DA_DIFF_OX)   fprintf(stderr, " origin-x");
    if (d & GZ_DA_DIFF_OY)   fprintf(stderr, " origin-y");
    if (d & GZ_DA_DIFF_Z)    fprintf(stderr, " z");
    if (d & GZ_DA_DIFF_TILT) fprintf(stderr, " tilt");
    fprintf(stderr, "  (tolerance %.1f mm, %.2f deg)\n", tol_mm, tol_deg);

    fprintf(stderr,
        "REFUSING TO CALIBRATE. Restart the daemon as:\n"
        "    tobiifreed --force-display-area\n"
        "then run this again to confirm the device took the value.\n"
        "Calibration is computed in the frame the display area defines, so a\n"
        "wrong frame produces plausible-but-wrong gaze that no later step can\n"
        "detect or correct. The daemon cannot catch this itself: setDisplayArea\n"
        "reports success as soon as the send succeeds, without comparing the\n"
        "readback, and isReset() only fires below 50 mm.\n");
    return -1;
}

/* usb_busy means the command never reached the device, so the connection is
 * still in step and a plain retry is correct. 50 ms is well past the daemon's
 * ~30 ms command service interval. */
#define GZ_DA_RETRIES 3
#define GZ_DA_RETRY_MS 50

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void dump_head(const unsigned char *b, size_t n) {
    size_t show = n < 32 ? n : 32;
    fprintf(stderr, "  first %zu bytes:", show);
    for (size_t i = 0; i < show; i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

int gz_display_read(struct gz_client *c, const char *path, int timeout_ms,
                    double out[9]) {
    for (int attempt = 0; attempt < GZ_DA_RETRIES; attempt++) {
        int r = gz_client_request(c, GZ_CMD_GET_DISPLAY_AREA, NULL, 0, timeout_ms);
        if (r == 0) {
            if (gz_decode_display_area(c->resp, c->resp_len, out)) return 0;
            /* Not a transport failure: the daemon forwarded whatever the
             * device sent. Refusing beats guessing, and the bytes are printed
             * because a shape change is the only thing that gets here. */
            fprintf(stderr,
                    "display area: reply did not parse as TTP TLV (%zu bytes, "
                    "expected at least %d)\n", c->resp_len, GZ_DA_MIN_BODY);
            dump_head(c->resp, c->resp_len);
            return -1;
        }

        if (r == GZ_CLIENT_REMOTE) {
            if (!gz_err_retryable(c->err_code) || attempt + 1 == GZ_DA_RETRIES) {
                fprintf(stderr, "display area: daemon error %u%s\n", c->err_code,
                        gz_err_retryable(c->err_code) ? " (usb_busy, out of retries)" : "");
                return GZ_CLIENT_REMOTE;
            }
            sleep_ms(GZ_DA_RETRY_MS);
            continue;
        }

        if (r == GZ_CLIENT_TIMEOUT || r == GZ_CLIENT_RECONNECT) {
            /* A timeout leaves the two sides out of step: the daemon may still
             * answer, and an err frame carries no cmd_type, so a late one would
             * be charged to the next command. Only a reconnect puts them back
             * in step, and it is not optional. */
            if (path == NULL || attempt + 1 == GZ_DA_RETRIES) {
                fprintf(stderr, "display area: no usable reply (%s)\n",
                        r == GZ_CLIENT_TIMEOUT ? "timeout" : "link lost");
                return r;
            }
            fprintf(stderr, "display area: %s, reconnecting\n",
                    r == GZ_CLIENT_TIMEOUT ? "timeout" : "link lost");
            if (gz_client_reconnect(c, path) != 0) {
                fprintf(stderr, "reconnect %s: %s\n", path, strerror(errno));
                return GZ_CLIENT_RECONNECT;
            }
            /* RE-GATE. gz_client_reconnect goes through gz_client_init, which
             * memsets the client and so clears have_status and
             * version_mismatch. Without this the loop would send the next
             * command to a daemon it has never identified: a restart between
             * the two connections can put a different build on the other end,
             * and its body would be decoded under the grammar the first one
             * agreed to. That is the exact failure the version gate exists to
             * prevent, reached through the one door it did not cover.
             *
             * The command timeout doubles as the status deadline. Both ask the
             * same question, how long to wait for the daemon to say anything,
             * and the daemon's status is the first frame on every connection. */
            if (gz_display_gate_status(c, timeout_ms) != 0) {
                fprintf(stderr, "display area: the reconnected daemon did not pass "
                                "the status gate, refusing\n");
                return -1;
            }
            continue;
        }

        fprintf(stderr, "display area: request failed (%d)\n", r);
        return r;
    }
    return GZ_CLIENT_TIMEOUT;
}

int gz_display_gate_status(struct gz_client *c, int timeout_ms) {
    uint64_t deadline = gz_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    while (!c->have_status && gz_now_ns() < deadline) {
        if (gz_client_poll(c, 100) == GZ_CLIENT_RECONNECT) break;
    }

    if (!c->have_status) {
        /* The daemon sends status as the first frame on every connection, so
         * its absence is not slowness. Proceeding would mean issuing commands
         * to something that has not identified itself. */
        fprintf(stderr, "no status frame within %d ms: refusing to continue\n", timeout_ms);
        return -1;
    }

    /* THE VERSION GATE. Refuse, with no override.
     *
     * PROTOCOL_VERSION is bumped when a message changes shape. Every claim this
     * gate makes runs through the byte layout of a response body, and the
     * response the geometry arrives in has no length or shape this client can
     * cross-check: a 164-byte body that decodes cleanly under the wrong grammar
     * yields a wrong-but-plausible rectangle, which is the one failure the
     * whole chain exists to prevent. An unknown version is exactly the case
     * where we cannot know the grammar still holds.
     *
     * No --force flag, deliberately. The escape hatch for a wrong geometry is
     * tobiifreed --force-display-area, which fixes the device. There is no
     * corresponding fix for an unknown protocol except updating gaze-cal, so a
     * flag here would only let someone skip the check that says so. */
    if (c->version_mismatch) {
        fprintf(stderr,
                "daemon speaks protocol version %u, this build understands %d.\n"
                "REFUSING. A response body is read by shape, and a version bump\n"
                "means a shape changed, so the geometry could decode cleanly and\n"
                "wrongly. Rebuild gaze-cal against the daemon you are running.\n",
                c->status.protocol_version, GZ_PROTOCOL_VERSION);
        return -1;
    }

    if (!c->status.device_present) {
        fprintf(stderr, "daemon reports no device: nothing to read a geometry from.\n");
        return -1;
    }
    return 0;
}

int gz_display_gate(struct gz_client *c, const char *path, struct gz_rect want,
                    double tol_mm, double tol_deg) {
    if (gz_display_gate_status(c, 2000) != 0) return GZ_GATE_UNKNOWN;

    double corners[9];
    if (gz_display_read(c, path, GZ_CLIENT_CMD_TIMEOUT_MS, corners) != 0)
        return GZ_GATE_UNKNOWN;

    return gz_display_verify(corners, want, tol_mm, tol_deg) == 0
         ? GZ_GATE_OK : GZ_GATE_MISMATCH;
}

int gz_cmd_display(const char *sock_path, const char *cfg_path, double tol_mm) {
    char buf[512];
    if (cfg_path == NULL) {
        if (gz_config_path(buf, sizeof buf) != 0) {
            fprintf(stderr, "cannot build the config path: $HOME unset or too long\n");
            return 2;
        }
        cfg_path = buf;
    }

    struct gz_rect want;
    int cr = gz_config_load_rect(cfg_path, &want);
    if (cr == -1) {
        fprintf(stderr, "cannot read %s: %s\n", cfg_path, strerror(errno));
        fprintf(stderr, "REFUSING. Without it there is nothing to compare the device against,\n"
                        "and the daemon would silently have used its 1500x1000 template.\n");
        return 2;
    }
    if (cr != 0) {
        fprintf(stderr, "%s does not parse as a display_area config\n", cfg_path);
        fprintf(stderr, "REFUSING rather than falling back to a default, which is what the\n"
                        "daemon does and is how a typo becomes a geometry.\n");
        return 2;
    }

    /* Named because the caveat gz_display_verify prints on success says "the
     * config" without knowing which file that was. */
    fprintf(stderr, "config: %s\n", cfg_path);

    struct gz_client c;
    if (gz_client_connect(&c, sock_path) != 0) {
        fprintf(stderr, "connect %s: %s\n", sock_path, strerror(errno));
        return GZ_GATE_UNKNOWN;
    }

    int rc = gz_display_gate(&c, sock_path, want, tol_mm, GZ_DA_TOL_DEG);
    gz_client_close(&c);
    return rc;
}
