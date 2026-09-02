/* gaze-cal/src/calibrate.c - see calibrate.h. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "calibrate.h"
#include "display.h"

/* ---------------- output ----------------
 *
 * Every human-facing line goes to stderr and, when a run has opened one, to
 * the log under ~/.local/share/tobii-gaze. The experiment this task exists to
 * settle spans a daemon restart and a physical unplug, so its numbers cannot
 * live in a terminal scrollback that a reboot eats. */
static FILE *g_log;

/* format() is what makes -Wformat check these call sites at all: a wrong
 * conversion inside a variadic wrapper is otherwise invisible. nonnull() is
 * not decoration either. Without it GCC 16 at -O1 with the sanitizers
 * hypothesises a NULL format reaching vfprintf and fails the build under
 * -Werror, and -fsanitize=undefined turns the attribute into a real runtime
 * check rather than a silencer. */
static void say(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), nonnull(1)));

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log != NULL) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static int data_dir(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') return -1;
    int n = snprintf(buf, cap, "%s/%s", home, GZ_BLOB_RELDIR);
    if (n < 0 || (size_t)n >= cap) return -1;
    /* EEXIST is the normal case and is not an error. Anything else is left to
     * the open that follows, which reports errno properly. */
    mkdir(buf, 0755);
    return 0;
}

/* Beside data_dir rather than beside gz_correction_path, so log_open below is
 * built on it. The setup view appends to the same file, and two expressions
 * for one path is how they would come to differ. */
int gz_log_path(char *buf, size_t cap) {
    char dir[512];
    if (data_dir(dir, sizeof dir) != 0) return -1;
    int n = snprintf(buf, cap, "%s/gaze-cal.log", dir);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

static void log_open(const char *what, const char *label) {
    char path[600];
    if (gz_log_path(path, sizeof path) != 0) return;
    g_log = fopen(path, "a");
    if (g_log == NULL) return;

    time_t t = time(NULL);
    struct tm tmv;
    char when[64] = "unknown time";
    if (localtime_r(&t, &tmv) != NULL) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(g_log, "\n===== %s%s%s  %s =====\n", what,
            label ? " label=" : "", label ? label : "", when);
    fflush(g_log);
    fprintf(stderr, "log: %s\n", path);
}

static void log_close(void) {
    if (g_log != NULL) { fclose(g_log); g_log = NULL; }
}

static void sleep_ms(unsigned ms) {
    struct timespec t;
    t.tv_sec = (time_t)(ms / 1000);
    t.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&t, NULL);
}

/* ---------------- screen geometry ---------------- */

void gz_screen_point_px(const struct gz_screen *s, double nx, double ny,
                        int *px, int *py) {
    /* nx * w - 0.5 is the centre of the pixel column the normalised coordinate
     * falls in, so 0.1 and 0.9 land symmetrically about the middle (256 and
     * 2304 on a 2560 px panel) rather than drifting one pixel right. */
    double fx = nx * (double)s->w - 0.5;
    double fy = ny * (double)s->h - 0.5;

    /* NaN survives every comparison below as false, so it would fall through
     * with an indeterminate lround. Pinned to the centre instead: a caller
     * that produced NaN has a bug, and a dot in the middle is the one position
     * that cannot be mistaken for a real stimulus point. */
    if (!(fx == fx)) fx = (double)s->w / 2.0;
    if (!(fy == fy)) fy = (double)s->h / 2.0;

    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > (double)(s->w - 1)) fx = (double)(s->w - 1);
    if (fy > (double)(s->h - 1)) fy = (double)(s->h - 1);

    *px = s->x + (int)lround(fx);
    *py = s->y + (int)lround(fy);
}

/* ---------------- the nine points ----------------
 *
 * Row-major from the top-left, matching the normalised frame gaze comes back
 * in, so the i-th stimulus and the i-th measurement are the same point. */
const double GZ_CAL_PTS[GZ_CAL_POINTS][2] = {
    {0.1, 0.1}, {0.5, 0.1}, {0.9, 0.1},
    {0.1, 0.5}, {0.5, 0.5}, {0.9, 0.5},
    {0.1, 0.9}, {0.5, 0.9}, {0.9, 0.9},
};

const struct gz_cal_opts GZ_CAL_DEFAULTS = {
    NULL,
    /* 1200 ms total per the brief, split so the validity window is the 300 ms
     * immediately before the point is sent. Judging the eye over the whole
     * 1200 ms would count the saccade, which is not tracked, against a
     * fixation that is fine. */
    900, 300,
    /* Two thirds of the sample window with both eyes tracked. A point added
     * with the eyes shut is not a missing point, it is a wrong one, and it
     * degrades the whole fit rather than the corner it belongs to. */
    0.66, 2,
    GZ_CAL_START_TIMEOUT_MS, GZ_CAL_POINT_TIMEOUT_MS, GZ_CAL_FINISH_TIMEOUT_MS,
    GZ_CAL_GAP_WAIT_MS
};

/* ---------------- CRC and the blob file ---------------- */

uint32_t gz_crc32(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

int gz_blob_path(char *buf, size_t cap) {
    char dir[512];
    if (data_dir(dir, sizeof dir) != 0) return -1;
    int n = snprintf(buf, cap, "%s/%s", dir, GZ_BLOB_RELNAME);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

static void put_le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static uint32_t get_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int gz_blob_save_to(const char *path, const unsigned char *blob, size_t n) {
    /* Against the device's ceiling, never against a local buffer. out_scratch
     * is [4096]u8 and cal_finish_blob_ptr returns a pointer into it, so 4096
     * is the only bound that means anything here. */
    if (n == 0 || n > GZ_CAL_BLOB_MAX) {
        fprintf(stderr, "calibration blob is %zu bytes, outside 1..%d: refusing to save\n",
                n, GZ_CAL_BLOB_MAX);
        return -1;
    }

    char tmp[600];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        fprintf(stderr, "calibration path too long: %s\n", path);
        return -1;
    }

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    unsigned char hdr[GZ_BLOB_HDR];
    put_le32(hdr + 0, GZ_BLOB_MAGIC);
    put_le32(hdr + 4, GZ_BLOB_VERSION);
    put_le32(hdr + 8, (uint32_t)n);
    put_le32(hdr + 12, gz_crc32(blob, n));

    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(blob, 1, n, f) == n;
    /* Flushed and synced before the rename, so the rename cannot publish a
     * name whose contents are still in the page cache. */
    if (ok && fflush(f) != 0) ok = 0;
    if (ok && fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;

    if (!ok) {
        fprintf(stderr, "cannot write %s: %s\n", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "cannot rename %s to %s: %s\n", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

int gz_blob_load_from(const char *path, unsigned char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;

    unsigned char hdr[GZ_BLOB_HDR];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fprintf(stderr, "%s: shorter than a header\n", path);
        fclose(f);
        return -1;
    }
    uint32_t magic = get_le32(hdr + 0), version = get_le32(hdr + 4);
    uint32_t len = get_le32(hdr + 8), crc = get_le32(hdr + 12);

    if (magic != GZ_BLOB_MAGIC) {
        fprintf(stderr, "%s: not a calibration file (magic %08x)\n", path, magic);
        fclose(f);
        return -1;
    }
    if (version != GZ_BLOB_VERSION) {
        fprintf(stderr, "%s: format version %u, this build writes %u\n",
                path, version, GZ_BLOB_VERSION);
        fclose(f);
        return -1;
    }
    if (len == 0 || len > GZ_CAL_BLOB_MAX) {
        fprintf(stderr, "%s: declares %u bytes, outside 1..%d\n", path, len, GZ_CAL_BLOB_MAX);
        fclose(f);
        return -1;
    }
    if (len > cap) {
        fprintf(stderr, "%s: %u bytes does not fit in %zu\n", path, len, cap);
        fclose(f);
        return -1;
    }

    if (fread(out, 1, len, f) != len) {
        fprintf(stderr, "%s: truncated, %u bytes declared\n", path, len);
        fclose(f);
        return -1;
    }
    /* One byte past the declared end. A file with extra bytes is not the file
     * that was written, and the CRC alone cannot see them. */
    unsigned char extra;
    int trailing = fread(&extra, 1, 1, f) == 1;
    fclose(f);
    if (trailing) {
        fprintf(stderr, "%s: %u bytes declared but the file is longer\n", path, len);
        return -1;
    }

    uint32_t got = gz_crc32(out, len);
    if (got != crc) {
        fprintf(stderr, "%s: CRC %08x, expected %08x. REFUSING.\n"
                        "A corrupt blob applied to the device is worse than no calibration:\n"
                        "it is wrong rather than absent, and nothing downstream can tell.\n",
                path, got, crc);
        return -1;
    }
    return (int)len;
}

int gz_blob_save(const unsigned char *blob, size_t n) {
    char path[600];
    if (gz_blob_path(path, sizeof path) != 0) {
        fprintf(stderr, "cannot build the calibration path: $HOME unset or too long\n");
        return -1;
    }
    if (gz_blob_save_to(path, blob, n) != 0) return -1;
    say("saved %zu byte calibration (crc %08x) to %s\n", n, gz_crc32(blob, n), path);
    return 0;
}

int gz_blob_load(unsigned char *out, size_t cap) {
    char path[600];
    if (gz_blob_path(path, sizeof path) != 0) {
        fprintf(stderr, "cannot build the calibration path: $HOME unset or too long\n");
        return -1;
    }
    return gz_blob_load_from(path, out, cap);
}

/* ---------------- sampling ---------------- */

double gz_valid_frac(const struct gz_samples *s) {
    if (s->n_seen < GZ_CAL_MIN_SEEN) return 0.0;
    return (double)s->n_both / (double)s->n_seen;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

struct gz_stat gz_stat_of(double *v, unsigned n) {
    struct gz_stat s = { 0, 0, 0 };
    if (n == 0) return s;
    qsort(v, n, sizeof *v, cmp_double);
    s.median = (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    s.p05 = v[(unsigned)((double)(n - 1) * 0.05)];
    s.p95 = v[(unsigned)((double)(n - 1) * 0.95 + 0.5)];
    return s;
}

int gz_collect(struct gz_client *c, unsigned ms, struct gz_samples *s) {
    return gz_collect_eyes(c, ms, s, NULL);
}

int gz_collect_eyes(struct gz_client *c, unsigned ms, struct gz_samples *s,
                    struct gz_eye_state *e) {
    memset(s, 0, sizeof *s);

    /* Scoped to this window when the caller did not supply one. Zeroing it is
     * not optional: gz_sample_eye_mid reads have_lr before writing it. */
    struct gz_eye_state local;
    if (e == NULL) {
        gz_eye_state_init(&local);
        e = &local;
    }

    uint64_t frames_before = c->gaze_frames;
    uint64_t end = gz_now_ns() + (uint64_t)ms * 1000000ULL;
    uint32_t last_counter = 0;
    int have_last = 0;

    for (;;) {
        uint64_t now = gz_now_ns();
        if (now >= end) break;
        uint64_t left_ms = (end - now) / 1000000ULL;
        /* 10 ms slices against a 30.1 ms sample interval, so a poll rarely
         * coalesces two frames and c->latest rarely hides one. */
        int slice = (int)(left_ms > 10 ? 10 : left_ms) + 1;

        int r = gz_client_poll(c, slice);
        if (r == GZ_CLIENT_RECONNECT) return GZ_CLIENT_RECONNECT;
        if (r <= 0 || !c->have_latest) continue;

        const struct gz_gaze_sample *g = &c->latest;
        if (have_last && g->frame_counter == last_counter) continue;
        last_counter = g->frame_counter;
        have_last = 1;
        s->n_seen++;

        if (!gz_sample_any_eye_valid(g)) continue;
        s->n_any++;
        if (gz_sample_both_eyes_valid(g)) s->n_both++;

        int have_gaze = (g->present_mask & GZ_BIT_GAZE_2D) != 0;
        if (s->n < GZ_SAMPLE_CAP && have_gaze) {
            s->x[s->n] = g->gaze_point_2d_norm[0];
            s->y[s->n] = g->gaze_point_2d_norm[1];
            s->n++;
        }

        /* Both halves of the pair or neither, so the two means the fit uses
         * come from the same frames. The eye half is gated on VALIDITY inside
         * gz_sample_eye_mid and never on present_mask, which is 0x003fffff in
         * every frame this firmware sends including eyeless ones, where the
         * eye-origin fields are plain 0.0. */
        double mid[3];
        if (have_gaze && gz_sample_eye_mid(e, g, mid)) {
            s->fit_gaze_sum[0] += g->gaze_point_2d_norm[0];
            s->fit_gaze_sum[1] += g->gaze_point_2d_norm[1];
            s->fit_eye_sum[0] += mid[0];
            s->fit_eye_sum[1] += mid[1];
            s->fit_eye_sum[2] += mid[2];
            s->nfit++;
        }

        /* Tracker space, origin at the IR sensor array. Averaged over whichever
         * eyes the frame carried, because the sign is the question and both
         * eyes answer it the same way. */
        double zs = 0;
        int nz = 0;
        if (g->present_mask & GZ_BIT_EYE_ORIGIN_L) { zs += g->eye_origin_L_mm[2]; nz++; }
        if (g->present_mask & GZ_BIT_EYE_ORIGIN_R) { zs += g->eye_origin_R_mm[2]; nz++; }
        if (nz > 0 && s->nz < GZ_SAMPLE_CAP) s->z[s->nz++] = zs / nz;

        if ((g->present_mask & GZ_BIT_EYE_ORIGIN_L) && s->nex < GZ_SAMPLE_CAP) {
            s->ex[s->nex++] = g->eye_origin_L_mm[0];
        }
        if ((g->present_mask & GZ_BIT_EYE_ORIGIN_L) && s->ney < GZ_SAMPLE_CAP) {
            s->ey[s->ney++] = g->eye_origin_L_mm[1];
        }
    }

    /* From the client's own counter rather than from the loop, so frames a
     * single poll coalesced are still counted. */
    s->n_total = (unsigned)(c->gaze_frames - frames_before);
    return 0;
}

/* ---------------- the host-side correction ---------------- */

/* One univariate OLS over the points `use` selects. Returns 0 when there are
 * too few of them or when the regressor does not vary, which would otherwise
 * divide by zero and hand back a gain of infinity that every later check would
 * have to catch. */
static int ols(const double *v, const double *u, const int *use, int n,
               double *slope, double *intercept) {
    int m = 0;
    double sv = 0, su = 0;
    for (int i = 0; i < n; i++) {
        if (!use[i]) continue;
        sv += v[i]; su += u[i]; m++;
    }
    if (m < 3) return 0;

    double mv = sv / m, mu = su / m, svv = 0, svu = 0;
    for (int i = 0; i < n; i++) {
        if (!use[i]) continue;
        double dv = v[i] - mv;
        svv += dv * dv;
        svu += dv * (u[i] - mu);
    }
    /* Nine points spanning 0.1 to 0.9 give svv around 0.4, so this only fires
     * on degenerate input such as a sweep whose stimulus never moved. */
    if (!(svv > 1e-9)) return 0;

    *slope = svu / svv;
    *intercept = mu - *slope * mv;
    return 1;
}

/* The parameters and the residual summary over whichever points survived.
 * Called on the refusal paths too, so a rejected fit still reports the numbers
 * that got it rejected rather than a row of zeros. `out->valid` stays 0; only
 * the caller sets it, and only after the envelope check. */
static void fit_summary(struct gz_fit_report *rep, const struct gz_fit_input *in,
                        const int *use, int n, double gx, double gy,
                        double bx, double by, struct gz_rect area,
                        struct gz_correction *out) {
    double sorted[GZ_CAL_POINTS], zsum = 0;
    unsigned ns = 0;
    rep->worst_resid_mm = 0;
    for (int i = 0; i < n; i++) {
        if (!use[i]) continue;
        sorted[ns++] = rep->resid_mm[i];
        zsum += in[i].eye_mm[2];
        if (rep->resid_mm[i] > rep->worst_resid_mm) rep->worst_resid_mm = rep->resid_mm[i];
    }
    rep->n_used = (int)ns;
    rep->median_resid_mm = gz_stat_of(sorted, ns).median;
    rep->eye_z_mm = ns > 0 ? zsum / ns : 0.0;

    out->gx = gx; out->gy = gy;
    out->bx = bx; out->by = by;
    out->area = area;
    out->form = GZ_CORR_FORM_STATIC;
    out->valid = 0;
    out->eye_proj[0] = out->eye_proj[1] = 0.0;

    /* The seat this fit belongs to. Form S is scoped per seating position, so
     * this is the number that says which position, and it is what a later sweep
     * is compared against to predict the degradation. */
    double ex = 0, ey = 0;
    unsigned ne = 0;
    for (int i = 0; i < n; i++) {
        if (!use[i]) continue;
        double ep[2];
        gz_eye_proj(area, in[i].eye_mm, ep);
        ex += ep[0]; ey += ep[1]; ne++;
    }
    rep->eye_proj[0] = ne > 0 ? ex / ne : 0.0;
    rep->eye_proj[1] = ne > 0 ? ey / ne : 0.0;
    out->eye_proj[0] = rep->eye_proj[0];
    out->eye_proj[1] = rep->eye_proj[1];
}

int gz_correction_fit(const struct gz_fit_input *in, int n, struct gz_rect area,
                      struct gz_correction *out, struct gz_fit_report *rep) {
    memset(rep, 0, sizeof *rep);
    memset(out, 0, sizeof *out);
    rep->n_in = n;
    rep->refit_outlier = -1;

    if (n < 4 || n > GZ_CAL_POINTS) return GZ_FIT_ERR_POINTS;
    if (!(area.w_mm > 0) || !(area.h_mm > 0)) return GZ_FIT_ERR_FLAT;

    /* Form S regresses the reported point on the target directly. The eye is
     * not in the model at all: spec test 5.3 measured the eye-relative form
     * losing to this one, 69 px against 53, at a seat displaced 115 mm. It is
     * still read below, for the provenance the file carries. */
    double vx[GZ_CAL_POINTS], vy[GZ_CAL_POINTS];
    double ux[GZ_CAL_POINTS], uy[GZ_CAL_POINTS];
    for (int i = 0; i < n; i++) {
        vx[i] = in[i].target[0];
        vy[i] = in[i].target[1];
        ux[i] = in[i].reported[0];
        uy[i] = in[i].reported[1];
    }

    int use[GZ_CAL_POINTS];
    for (int i = 0; i < n; i++) use[i] = 1;

    double gx = 0, gy = 0, bx = 0, by = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (!ols(vx, ux, use, n, &gx, &bx)) return GZ_FIT_ERR_FLAT;
        if (!ols(vy, uy, use, n, &gy, &by)) return GZ_FIT_ERR_FLAT;
        if (!(gx > 0) || !(gy > 0)) return GZ_FIT_ERR_BOUNDS;

        /* The residual that matters is how far the CORRECTED point lands from
         * the target, not the regression residual: they differ by the factor
         * g, and the acceptance test measures the first. */
        struct gz_correction trial;
        memset(&trial, 0, sizeof trial);
        trial.gx = gx; trial.gy = gy; trial.bx = bx; trial.by = by;
        trial.area = area;
        trial.form = GZ_CORR_FORM_STATIC;
        for (int i = 0; i < n; i++) {
            double cor[2];
            gz_correct_point(&trial, in[i].reported, cor);
            rep->resid_mm[i] = hypot((cor[0] - in[i].target[0]) * area.w_mm,
                                     (cor[1] - in[i].target[1]) * area.h_mm);
        }

        /* The second pass only runs when the first dropped a point, so a sweep
         * that rejected nothing never reaches this and is untouched. When one
         * did go, a survivor still past 3x the REFIT median is the second bad
         * point that its own axis absorbed: measured on the pinned synthetic
         * case it drags gy 5.7 percent while moving isotropy the wrong way, so
         * neither the envelope nor the isotropy bound sees it. Scored over the
         * seven recorded nine-point sweeps this refuses none of them. */
        if (pass == 1) {
            double kept[GZ_CAL_POINTS];
            unsigned nk = 0;
            for (int i = 0; i < n; i++) if (use[i]) kept[nk++] = rep->resid_mm[i];
            double med2 = gz_stat_of(kept, nk).median;
            /* Same floor the first pass uses. Without it the eight survivors of
             * a near-perfect sweep have a median of picometres and refuse
             * themselves. */
            if (med2 > 1e-6) {
                double worst = 0;
                for (int i = 0; i < n; i++) {
                    if (use[i] && rep->resid_mm[i] > 3.0 * med2 &&
                        rep->resid_mm[i] > worst) {
                        worst = rep->resid_mm[i];
                        rep->refit_outlier = i;
                    }
                }
            }
            if (rep->refit_outlier >= 0) {
                fit_summary(rep, in, use, n, gx, gy, bx, by, area, out);
                return GZ_FIT_ERR_REFIT;
            }
            break;
        }

        double sorted[GZ_CAL_POINTS];
        unsigned ns = 0;
        for (int i = 0; i < n; i++) if (use[i]) sorted[ns++] = rep->resid_mm[i];
        double med = gz_stat_of(sorted, ns).median;
        /* A median of zero makes 3x the median zero, at which point every
         * nonzero residual is an outlier and a perfect fit rejects itself. */
        if (!(med > 1e-6)) break;

        int drops = 0;
        for (int i = 0; i < n; i++) {
            if (use[i] && rep->resid_mm[i] > 3.0 * med) {
                rep->rejected[i] = 1;
                drops++;
            }
        }
        if (drops == 0) break;
        rep->n_rejected = drops;
        /* Two bad points is not an outlier, it is a sweep where the eye was
         * somewhere else. Refitting around it would encode that. The summary
         * is filled first: a refusal that reports a median of zero tells the
         * human nothing about how far out the sweep was. */
        if (drops > 1) {
            fit_summary(rep, in, use, n, gx, gy, bx, by, area, out);
            return GZ_FIT_ERR_OUTLIER;
        }
        for (int i = 0; i < n; i++) if (rep->rejected[i]) use[i] = 0;
    }

    fit_summary(rep, in, use, n, gx, gy, bx, by, area, out);

    /* Refused loudly rather than stored. A gain outside the measured envelope
     * means bad input or a bug, and a wrong correction is worse than none
     * because downstream it looks exactly like a calibration. */
    if (!gz_correction_check(out)) return GZ_FIT_ERR_BOUNDS;
    out->valid = 1;
    return GZ_FIT_OK;
}

int gz_correction_path(char *buf, size_t cap) {
    char dir[512];
    if (data_dir(dir, sizeof dir) != 0) return -1;
    int n = snprintf(buf, cap, "%s/%s", dir, GZ_CORR_RELNAME);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

int gz_correction_save_to(const char *path, const struct gz_correction *c,
                          const struct gz_fit_report *rep, double eye_z_mm,
                          double mm_per_px) {
    char text[GZ_CORRECTION_TEXT_MAX];
    size_t n = gz_correction_format(c, text, sizeof text);
    if (n == 0) {
        fprintf(stderr, "correction does not serialise: refusing to save\n");
        return -1;
    }

    char tmp[600];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        fprintf(stderr, "correction path too long: %s\n", path);
        return -1;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    int ok = fwrite(text, 1, n, f) == n;

    /* Provenance. gz_correction_parse ignores keys it does not model, so these
     * cost the parser nothing and answer the question this project keeps
     * having to ask: where did the number come from. */
    if (ok) {
        char when[64] = "unknown";
        time_t t = time(NULL);
        struct tm tmv;
        if (gmtime_r(&t, &tmv) != NULL) strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tmv);
        ok = fprintf(f, "fit_utc=%s\n", when) > 0;
        if (ok && rep != NULL) {
            /* Pixels, because spec 4.1 names the key fit_median_resid_px and
             * because pixels are the unit the acceptance bands are quoted in.
             * The fit itself works in mm, which is why the conversion is the
             * caller's: proto.c and the fit know nothing about a panel's
             * resolution. */
            ok = fprintf(f, "fit_points=%d\nfit_rejected=%d\n"
                            "fit_median_resid_px=%.1f\nfit_worst_resid_px=%.1f\n",
                         rep->n_used, rep->n_rejected,
                         rep->median_resid_mm / mm_per_px,
                         rep->worst_resid_mm / mm_per_px) > 0;
            /* The seat itself is a modelled key that gz_correction_format
             * already wrote. This is the rate leaving it costs, which is
             * derivable from the gains and recorded so nobody has to. */
            if (ok) {
                double g = (c->gx + c->gy) / 2.0;
                ok = fprintf(f, "degrade_px_per_mm=%.3f\n",
                             g > 0 ? (g - 1.0) / g / mm_per_px : 0.0) > 0;
            }
        }
        if (ok) ok = fprintf(f, "fit_eye_z_mm=%.1f\n", eye_z_mm) > 0;
    }

    if (ok && fflush(f) != 0) ok = 0;
    if (ok && fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "cannot write %s: %s\n", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "cannot rename %s to %s: %s\n", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

int gz_correction_save(const struct gz_correction *c,
                       const struct gz_fit_report *rep, double eye_z_mm,
                       double mm_per_px) {
    char path[600];
    if (gz_correction_path(path, sizeof path) != 0) {
        fprintf(stderr, "cannot build the correction path: $HOME unset or too long\n");
        return -1;
    }
    if (gz_correction_save_to(path, c, rep, eye_z_mm, mm_per_px) != 0) return -1;
    say("saved the correction to %s\n", path);
    return 0;
}

/* Named the same way gz_rect_diff's bits are, so a refusal says which field. */
static void say_area_diff(unsigned d) {
    say("  differing fields:%s%s%s%s%s%s\n",
        (d & GZ_DA_DIFF_W) ? " w" : "", (d & GZ_DA_DIFF_H) ? " h" : "",
        (d & GZ_DA_DIFF_OX) ? " ox" : "", (d & GZ_DA_DIFF_OY) ? " oy" : "",
        (d & GZ_DA_DIFF_Z) ? " z" : "", (d & GZ_DA_DIFF_TILT) ? " tilt" : "");
}

int gz_correction_load_from(const char *path, struct gz_rect want,
                            struct gz_correction *out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;                 /* no correction is not an error */

    char text[GZ_CORRECTION_TEXT_MAX * 4];
    size_t n = fread(text, 1, sizeof text, f);
    /* One byte past the end, into its own storage: reading it back into `text`
     * would overwrite the first character of the file. */
    char extra;
    int overlong = fread(&extra, 1, 1, f) == 1;
    fclose(f);
    if (overlong) {
        say("%s: longer than %zu bytes. REFUSING.\n", path, sizeof text);
        return -1;
    }

    struct gz_correction c;
    int r = gz_correction_parse(text, n, &c);
    if (r == GZ_CORR_PARSE_MALFORMED) {
        say("%s: does not parse as a correction file. REFUSING.\n", path);
        return -1;
    }
    if (r == GZ_CORR_PARSE_STALE) {
        say("%s: written by an older build, or in a form this one does not\n"
            "apply. REFUSING, which is deliberate: a correction fitted as form H\n"
            "measures its offsets relative to the eye's projection rather than\n"
            "absolutely, so applying it here would be wrong by about 90 px and\n"
            "would look exactly like a working calibration.\n"
            "Re-run `gaze-cal fit` where you sit.\n", path);
        return -1;
    }
    if (r == GZ_CORR_PARSE_BOUNDS) {
        say("%s: one of its parameters is outside what this build accepts.\n"
            "Gains read %.4f, %.4f against %.2f..%.2f and an isotropy tolerance of\n"
            "%.2f; the form, the offsets, the display area and the fitted seat are\n"
            "checked as well, so a gain in range does not clear the file.\n"
            "REFUSING. Re-run `gaze-cal fit`.\n",
            path, c.gx, c.gy, GZ_CORR_G_MIN, GZ_CORR_G_MAX, GZ_CORR_ISO_TOL);
        return -1;
    }

    /* The gate that makes this file safe to keep. Calibration and this fit are
     * both computed in the display-area frame, so a correction fitted against
     * one geometry says nothing about another. Same tolerance the readback gate
     * uses, because it is the same question. */
    unsigned d = gz_rect_diff(c.area, want, GZ_DA_TOL_MM, GZ_DA_TOL_DEG);
    if (d != 0) {
        say("%s: fitted against a different display area. REFUSING.\n"
            "  fitted %.2f x %.2f at (%.2f, %.2f) z %.2f tilt %.2f\n"
            "  device %.2f x %.2f at (%.2f, %.2f) z %.2f tilt %.2f\n",
            path,
            c.area.w_mm, c.area.h_mm, c.area.ox_mm, c.area.oy_mm,
            c.area.z_mm, c.area.tilt_deg,
            want.w_mm, want.h_mm, want.ox_mm, want.oy_mm, want.z_mm, want.tilt_deg);
        say_area_diff(d);
        say("Re-run `gaze-cal fit` against the geometry the device now holds.\n");
        return -1;
    }

    *out = c;
    return 1;
}

int gz_correction_load(struct gz_rect want, struct gz_correction *out) {
    char path[600];
    if (gz_correction_path(path, sizeof path) != 0) {
        fprintf(stderr, "cannot build the correction path: $HOME unset or too long\n");
        return -1;
    }
    return gz_correction_load_from(path, want, out);
}

/* ---------------- one calibration command ---------------- */

static const char *cmd_name(uint8_t cmd) {
    switch (cmd) {
    case GZ_CMD_START_CAL:     return "start_calibration";
    case GZ_CMD_ADD_CAL_POINT: return "add_calibration_point";
    case GZ_CMD_FINISH_CAL:    return "finish_calibration";
    case GZ_CMD_CAL_APPLY:     return "cal_apply";
    default:                   return "command";
    }
}

/* usb_busy means nothing reached the device, so the connection is still in
 * step and a plain retry is correct. Any other error is not retried. */
#define GZ_CAL_RETRIES  3
#define GZ_CAL_RETRY_MS 50

/* The gaze inter-arrival gap that straddles the reply. Measured from the last
 * arrival the client saw when the reply landed, not from before the send: if
 * gaze kept flowing through the command there was no stall, and measuring from
 * before the send would report the whole command duration as one.
 *
 * This sees a stall that reaches the end of the command, which is the shape
 * the daemon produces, since it parks the USB thread for the command's whole
 * duration (main.zig:328). A park that ended before the reply would be
 * under-reported. */
static double measure_gap(struct gz_client *c, const struct gz_cal_opts *o) {
    if (c->gaze_frames == 0) return -1;      /* nothing subscribed, or no device */

    uint64_t last = c->last_gaze_ns;
    uint64_t frames = c->gaze_frames;
    uint64_t end = gz_now_ns() + (uint64_t)o->gap_wait_ms * 1000000ULL;

    while (c->gaze_frames == frames) {
        uint64_t now = gz_now_ns();
        if (now >= end) return (double)(now - last) / 1e6;   /* a floor, not the gap */
        int slice = (int)((end - now) / 1000000ULL) + 1;
        if (slice > 20) slice = 20;
        if (gz_client_poll(c, slice) == GZ_CLIENT_RECONNECT) {
            return (double)(gz_now_ns() - last) / 1e6;
        }
    }
    return (double)(c->last_gaze_ns - last) / 1e6;
}

/* A negative gap means the measurement was not available, which is not the
 * same as a stall of zero and must not print as one. */
static const char *gap_str(char *buf, size_t cap, double gap_ms) {
    if (gap_ms < 0) snprintf(buf, cap, "n/a");
    else snprintf(buf, cap, "%.0f ms", gap_ms);
    return buf;
}

static void record_step(struct gz_cal_result *r, uint8_t cmd, double req_ms, double gap_ms) {
    if (r == NULL) return;
    if (r->nsteps < (int)(sizeof r->step / sizeof r->step[0])) {
        r->step[r->nsteps].cmd = cmd;
        r->step[r->nsteps].req_ms = req_ms;
        r->step[r->nsteps].gap_ms = gap_ms;
        r->nsteps++;
    }
    if (gap_ms > r->max_gap_ms) r->max_gap_ms = gap_ms;
}

/* Sends one command and waits for it. On a timeout it reconnects and re-gates,
 * then still fails: a calibration session cannot be resumed across a
 * reconnect, and the reconnect is there to leave the link usable for whatever
 * the caller does next rather than to retry. */
static int cal_request(struct gz_client *c, const struct gz_cal_opts *o,
                       uint8_t cmd, const void *payload, size_t n,
                       int timeout_ms, struct gz_cal_result *r) {
    for (int attempt = 0; attempt < GZ_CAL_RETRIES; attempt++) {
        uint64_t t0 = gz_now_ns();
        int rc = gz_client_request(c, cmd, payload, n, timeout_ms);
        double req_ms = (double)(gz_now_ns() - t0) / 1e6;

        if (rc == 0) {
            record_step(r, cmd, req_ms, measure_gap(c, o));
            return 0;
        }

        if (rc == GZ_CLIENT_REMOTE) {
            /* The enum is non-exhaustive, so anything that is not usb_busy is
             * treated as a hard failure rather than switched over. */
            if (gz_err_retryable(c->err_code) && attempt + 1 < GZ_CAL_RETRIES) {
                say("  %s: daemon error %u (usb_busy), retrying\n", cmd_name(cmd), c->err_code);
                sleep_ms(GZ_CAL_RETRY_MS);
                continue;
            }
            say("  %s: daemon error %u%s\n", cmd_name(cmd), c->err_code,
                gz_err_retryable(c->err_code) ? " (usb_busy, out of retries)" : "");
            return GZ_CLIENT_REMOTE;
        }

        if (rc == GZ_CLIENT_TIMEOUT || rc == GZ_CLIENT_RECONNECT) {
            say("  %s: %s after %.0f ms\n", cmd_name(cmd),
                rc == GZ_CLIENT_TIMEOUT ? "no reply" : "link lost", req_ms);
            if (o->sock_path != NULL) {
                /* Not optional after a timeout. The daemon may still answer,
                 * and an err frame carries no cmd_type, so a late one would be
                 * charged to whatever command goes out next. */
                if (gz_client_reconnect(c, o->sock_path) != 0) {
                    say("  reconnect %s: %s\n", o->sock_path, strerror(errno));
                } else if (gz_display_gate_status(c, GZ_CLIENT_CMD_TIMEOUT_MS) != 0) {
                    /* gz_client_reconnect goes through gz_client_init, which
                     * clears have_status and version_mismatch, so the gate has
                     * to run again on the new connection. */
                    say("  the reconnected daemon did not pass the status gate\n");
                }
            }
            return rc;
        }

        say("  %s: request failed (%d)\n", cmd_name(cmd), rc);
        return rc;
    }
    return GZ_CLIENT_TIMEOUT;
}

/* ---------------- the sequence ---------------- */

int gz_calibrate(struct gz_client *c, const struct gz_stim_ops *stim,
                 const struct gz_cal_opts *o, struct gz_cal_result *r) {
    memset(r, 0, sizeof *r);

    say("calibration: %d points, %u ms settle + %u ms sample each\n",
        GZ_CAL_POINTS, o->settle_ms, o->sample_ms);

    int rc = cal_request(c, o, GZ_CMD_START_CAL, NULL, 0, o->start_timeout_ms, r);
    if (rc != 0) {
        say("start_calibration failed, nothing was calibrated\n");
        return rc;
    }

    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        double nx = GZ_CAL_PTS[i][0], ny = GZ_CAL_PTS[i][1];
        struct gz_samples s;
        double frac = 0;
        unsigned tries = 0;

        for (;;) {
            if (stim->show(stim->ctx, nx, ny) != 0) {
                say("stimulus failed at point %d\n", i + 1);
                return -1;
            }
            if (o->settle_ms > 0) {
                struct gz_samples ignore;
                if (gz_collect(c, o->settle_ms, &ignore) == GZ_CLIENT_RECONNECT) {
                    say("link lost while settling on point %d\n", i + 1);
                    return GZ_CLIENT_RECONNECT;
                }
            }
            if (gz_collect(c, o->sample_ms, &s) == GZ_CLIENT_RECONNECT) {
                say("link lost while sampling point %d\n", i + 1);
                return GZ_CLIENT_RECONNECT;
            }

            frac = gz_valid_frac(&s);
            if (o->min_valid_frac <= 0 || frac >= o->min_valid_frac) break;
            if (tries >= o->point_retries) {
                say("point %d (%.1f, %.1f): only %.0f%% of %u frames had both eyes.\n"
                    "ABORTING. A point added with the eyes shut is not a missing point,\n"
                    "it is a wrong one, and it degrades the whole fit rather than the\n"
                    "corner it belongs to. Re-run once the tracker sees both eyes; a\n"
                    "failed run leaves the calibration realm open and the next\n"
                    "start_calibration unlocks it again.\n",
                    i + 1, nx, ny, frac * 100.0, s.n_seen);
                return -1;
            }
            tries++;
            r->retries++;
            say("  point %d: %.0f%% both-eyes over %u of %u frames, retrying (%u)\n",
                i + 1, frac * 100.0, s.n_seen, s.n_total, tries);
        }
        r->valid_frac[i] = frac;

        /* Two f64 in the device's normalised display frame, little-endian, the
         * shape main.zig buildRequest memcpys straight out of the payload. */
        double p[2] = { nx, ny };
        rc = cal_request(c, o, GZ_CMD_ADD_CAL_POINT, p, sizeof p, o->point_timeout_ms, r);
        if (rc != 0) {
            say("add_calibration_point %d failed, aborting\n", i + 1);
            return rc;
        }
        char gb[32];
        say("  point %d/%d (%.1f, %.1f)  both-eyes %.0f%%  %.1f ms  gaze gap %s\n",
            i + 1, GZ_CAL_POINTS, nx, ny, frac * 100.0, r->step[r->nsteps - 1].req_ms,
            gap_str(gb, sizeof gb, r->step[r->nsteps - 1].gap_ms));
    }

    rc = cal_request(c, o, GZ_CMD_FINISH_CAL, NULL, 0, o->finish_timeout_ms, r);
    if (rc != 0) {
        say("finish_calibration failed, nothing was applied\n");
        return rc;
    }

    /* Against 4096, the size of out_scratch, not against sizeof r->blob. The
     * daemon slices cal_finish_blob_ptr() by cal_finish_blob_len(), so a
     * length past the scratch is the daemon reporting something impossible and
     * is refused rather than truncated. */
    if (c->resp_len == 0 || c->resp_len > GZ_CAL_BLOB_MAX) {
        say("finish_calibration returned %zu bytes, outside 1..%d. REFUSING.\n",
            c->resp_len, GZ_CAL_BLOB_MAX);
        return -1;
    }
    memcpy(r->blob, c->resp, c->resp_len);
    r->blob_len = c->resp_len;
    say("finish_calibration returned a %zu byte blob (crc %08x)\n",
        r->blob_len, gz_crc32(r->blob, r->blob_len));
    return 0;
}

int gz_apply_blob(struct gz_client *c, const char *sock_path,
                  const unsigned char *blob, size_t n) {
    if (n == 0 || n > GZ_CAL_BLOB_MAX) {
        say("cal_apply refused: %zu bytes is outside 1..%d\n", n, GZ_CAL_BLOB_MAX);
        return -1;
    }
    struct gz_cal_opts o = GZ_CAL_DEFAULTS;
    o.sock_path = sock_path;
    struct gz_cal_result r;
    memset(&r, 0, sizeof r);

    int rc = cal_request(c, &o, GZ_CMD_CAL_APPLY, blob, n, GZ_CAL_APPLY_TIMEOUT_MS, &r);
    if (rc != 0) {
        say("cal_apply failed (%d)\n", rc);
        return rc;
    }
    char gb[32];
    say("cal_apply: %zu bytes accepted, %.1f ms, gaze gap %s\n",
        n, r.step[0].req_ms, gap_str(gb, sizeof gb, r.step[0].gap_ms));
    return 0;
}

/* ---------------- shared CLI plumbing ---------------- */

/* Connect, gate the status, and check the device holds the geometry the config
 * asks for. Returns GZ_GATE_*; only GZ_GATE_OK may calibrate or measure.
 *
 * Exported rather than static because record.c needs exactly this: the trace
 * recorder must refuse a correction fitted against different geometry the same
 * way accuracy does, and it can only do that against the rect this proves the
 * device is holding. */
int gz_connect_and_gate(struct gz_client *c, const char *sock, const char *cfg,
                        int require_geometry, struct gz_rect *out_want) {
    /* Before anything that can return early. Every caller closes the client on
     * a non-OK gate, and the two config failures below return before
     * gz_client_connect has touched it, so without this that close reads an
     * uninitialised fd off the caller's stack and may close an unrelated one. */
    gz_client_init(c);

    char cfgbuf[512];
    if (cfg == NULL) {
        if (gz_config_path(cfgbuf, sizeof cfgbuf) != 0) {
            say("cannot build the config path: $HOME unset or too long\n");
            return GZ_GATE_UNKNOWN;
        }
        cfg = cfgbuf;
    }

    struct gz_rect want;
    int cr = gz_config_load_rect(cfg, &want);
    if (cr != 0) {
        say("%s: %s\n", cfg, cr == -1 ? strerror(errno) : "does not parse");
        say("REFUSING. Without it there is nothing to compare the device against.\n");
        return GZ_GATE_UNKNOWN;
    }
    say("config: %s\n", cfg);
    if (out_want != NULL) *out_want = want;

    if (gz_client_connect(c, sock) != 0) {
        say("connect %s: %s\n", sock, strerror(errno));
        return GZ_GATE_UNKNOWN;
    }

    int g = gz_display_gate(c, sock, want, GZ_DA_TOL_MM, GZ_DA_TOL_DEG);
    /* Only a MISMATCH is waivable, and only for the probe, whose job is to
     * settle a field of the geometry and which therefore runs before it is
     * right. UNKNOWN means the geometry could not be read at all, which means
     * the daemon or the device is not there, and nothing below would measure
     * anything. */
    if (g == GZ_GATE_MISMATCH && !require_geometry) {
        say("continuing anyway: this command settles the geometry rather than\n"
            "depending on it\n");
        return GZ_GATE_OK;
    }
    return g;
}

static void report_parks(const struct gz_cal_result *r) {
    say("\nUSB park, per command. Every forwarded command parks the daemon's USB\n"
        "thread, so gaze production stops for every subscribed client, not only\n"
        "this one. GZ_WATCHDOG_NS is %.0f ms.\n",
        (double)GZ_WATCHDOG_NS / 1e6);
    for (int i = 0; i < r->nsteps; i++) {
        char gb[32];
        say("  %-22s reply %7.1f ms   gaze gap %8s%s\n",
            cmd_name(r->step[i].cmd), r->step[i].req_ms,
            gap_str(gb, sizeof gb, r->step[i].gap_ms),
            r->step[i].gap_ms > (double)GZ_WATCHDOG_NS / 1e6 ? "  OVER WATCHDOG" : "");
    }
    say("  worst gaze gap %.0f ms: %s\n", r->max_gap_ms,
        r->max_gap_ms > (double)GZ_WATCHDOG_NS / 1e6
            ? "a second subscribed client would have reconnected. Pass a longer\n"
              "  interval to gz_client_watchdog_for rather than raising the default."
            : "inside the 1 s default, so a second subscribed client rides it out.");
}

/* ---------------- calibrate ---------------- */

int gz_cmd_calibrate(const char *sock, const char *cfg,
                     const struct gz_stim_ops *stim, const struct gz_screen *scr) {
    log_open("calibrate", NULL);
    say("screen: %s %dx%d at +%d+%d\n", scr->name, scr->w, scr->h, scr->x, scr->y);

    struct gz_client c;
    int g = gz_connect_and_gate(&c, sock, cfg, 1, NULL);
    if (g != GZ_GATE_OK) {
        gz_client_close(&c);
        log_close();
        return g;
    }

    struct gz_cal_opts o = GZ_CAL_DEFAULTS;
    o.sock_path = sock;

    struct gz_cal_result r;
    int rc = gz_calibrate(&c, stim, &o, &r);
    if (rc != 0) {
        gz_client_close(&c);
        say("CALIBRATION FAILED (%d)\n", rc);
        log_close();
        return 1;
    }

    /* Saved before applied. The device is already calibrated at this point,
     * because finish_calibration is compute_and_apply then retrieve, so a
     * failure in cal_apply must not also lose the only copy of the blob. */
    int saved = gz_blob_save(r.blob, r.blob_len);

    int applied = gz_apply_blob(&c, sock, r.blob, r.blob_len);
    if (applied == 0) {
        say("cal_apply accepted: the daemon will now replay this blob onto a\n"
            "reconnected session by itself, and advertises calibration_applied=1.\n");
    } else {
        say("cal_apply failed. The DEVICE is still calibrated, because\n"
            "finish_calibration computed and applied it, but the daemon does not\n"
            "know the blob and will not replay it after a reconnect.\n");
    }

    report_parks(&r);

    say("\nvalidity per point:");
    for (int i = 0; i < GZ_CAL_POINTS; i++) say(" %.0f%%", r.valid_frac[i] * 100.0);
    say("\nretries: %u\n", r.retries);

    gz_client_close(&c);
    say("\nCALIBRATION DONE. Next: gaze-cal accuracy --label cal\n");
    log_close();
    return (saved == 0 && applied == 0) ? 0 : 1;
}

/* ---------------- apply-saved ---------------- */

int gz_cmd_apply_saved(const char *sock) {
    log_open("apply-saved", NULL);

    unsigned char blob[GZ_CAL_BLOB_MAX];
    int n = gz_blob_load(blob, sizeof blob);
    if (n < 0) {
        say("no valid saved calibration\n");
        log_close();
        return 1;
    }
    say("loaded %d byte calibration (crc %08x)\n", n, gz_crc32(blob, (size_t)n));

    struct gz_client c;
    if (gz_client_connect(&c, sock) != 0) {
        say("connect %s: %s\n", sock, strerror(errno));
        log_close();
        return 1;
    }
    /* The version gate before any command, exactly as the display gate does:
     * a response body is read by shape, so an unknown protocol version means
     * the shape cannot be assumed. */
    if (gz_display_gate_status(&c, GZ_CLIENT_CMD_TIMEOUT_MS) != 0) {
        gz_client_close(&c);
        log_close();
        return 1;
    }

    int rc = gz_apply_blob(&c, sock, blob, (size_t)n);
    gz_client_close(&c);
    /* Non-zero matters: an ExecStartPost that exits 0 on a rejected blob is how
     * an operator ends up trusting a calibration that is not there. */
    say("%s\n", rc == 0 ? "applied" : "NOT applied");
    log_close();
    return rc == 0 ? 0 : 1;
}

/* ---------------- the nine-point sweep, shared by accuracy and fit ----------
 *
 * One runner, so the correction is always fitted from exactly the measurement
 * the accuracy table reports, and so every sweep records its per-point eye
 * origins whether or not it is the one being fitted. Those origins are what
 * spec test 5.3 was scored from, and they are what says how far a later sweep
 * sat from the seat the fit belongs to. */

/* GZ_ACC_TARGET_PX, the one-degree yardstick these figures are judged against,
 * is in calibrate.h: the verdict fills need it too.
 *
 * Settle, then measure. Split so the validity window excludes the saccade. */
#define GZ_SWEEP_SETTLE_MS 700
#define GZ_SWEEP_SAMPLE_MS 600

static int run_sweep(struct gz_client *c, const struct gz_stim_ops *stim,
                     const struct gz_screen *scr, struct gz_rect panel,
                     struct gz_sweep *out) {
    memset(out, 0, sizeof *out);

    /* One state for the whole sweep, so the reconstruction is armed by the
     * first both-eyes frame and stays armed for later points where one eye
     * drops out. Per-window state would re-arm nine times. */
    struct gz_eye_state eye;
    gz_eye_state_init(&eye);

    say("\n  point                target px        gaze px       dx      dy   err px  err deg  frames\n");
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        double nx = GZ_CAL_PTS[i][0], ny = GZ_CAL_PTS[i][1];
        struct gz_sweep_point *p = &out->pt[i];
        p->target[0] = nx;
        p->target[1] = ny;
        p->err_px = -1;

        if (stim->show(stim->ctx, nx, ny) != 0) {
            say("stimulus failed at point %d\n", i + 1);
            return -1;
        }
        struct gz_samples ignore, s;
        if (gz_collect_eyes(c, GZ_SWEEP_SETTLE_MS, &ignore, &eye) == GZ_CLIENT_RECONNECT ||
            gz_collect_eyes(c, GZ_SWEEP_SAMPLE_MS, &s, &eye) == GZ_CLIENT_RECONNECT) {
            say("link lost measuring point %d\n", i + 1);
            return -1;
        }

        p->frames = s.n_total;
        p->n_gaze = s.n;
        p->n_fit  = s.nfit;

        int tx = 0, ty = 0;
        gz_screen_point_px(scr, nx, ny, &tx, &ty);

        if (s.nfit > 0) {
            p->gaze_mean[0] = s.fit_gaze_sum[0] / s.nfit;
            p->gaze_mean[1] = s.fit_gaze_sum[1] / s.nfit;
            for (int k = 0; k < 3; k++) p->eye_mm[k] = s.fit_eye_sum[k] / s.nfit;
            out->n_fit_ok++;
        }

        if (s.n == 0) {
            say("  %d/%d (%.1f,%.1f)  %6d,%-6d   NO VALID GAZE (%u frames)\n",
                i + 1, GZ_CAL_POINTS, nx, ny, tx, ty, s.n_total);
            continue;
        }

        struct gz_stat sx = gz_stat_of(s.x, s.n);
        struct gz_stat sy = gz_stat_of(s.y, s.n);
        p->gaze_med[0] = sx.median;
        p->gaze_med[1] = sy.median;
        double gx = sx.median * (double)scr->w, gy = sy.median * (double)scr->h;
        double dx = gx - (double)(tx - scr->x), dy = gy - (double)(ty - scr->y);
        p->err_px = hypot(dx, dy);
        out->n_gaze_ok++;

        for (unsigned k = 0; k < s.nz && out->nz < GZ_SAMPLE_CAP; k++)
            out->zbuf[out->nz++] = s.z[k];

        /* Degrees from this session's own eye distance when the frame carried
         * one, since 45 px/deg is only true at 600 mm. */
        double zmed = 0;
        if (s.nz > 0) {
            double tmp[GZ_SAMPLE_CAP];
            memcpy(tmp, s.z, s.nz * sizeof tmp[0]);
            zmed = fabs(gz_stat_of(tmp, s.nz).median);
        }
        /* From the config the gate just matched against the device, not a
         * constant: a different panel makes every degree here wrong. */
        double mm_per_px = panel.w_mm / (double)scr->w;
        double deg = (zmed > 1.0)
                   ? atan2(p->err_px * mm_per_px, zmed) * 180.0 / 3.14159265358979323846
                   : p->err_px / GZ_ACC_TARGET_PX;

        say("  %d/%d (%.1f,%.1f)  %6d,%-6d  %7.0f,%-7.0f %7.0f %7.0f %8.0f %8.2f %7u\n",
            i + 1, GZ_CAL_POINTS, nx, ny, tx, ty,
            gx + scr->x, gy + scr->y, dx, dy, p->err_px, deg, s.n_total);
    }

    /* The machine-readable record, at full precision and in the frame the
     * correction is fitted in. The table above rounds to whole pixels and
     * carries no eye position, so it cannot be refitted later. Written for
     * every sweep, including ones nobody intends to fit. */
    say("\n  raw sweep, normalised display coords, eye midpoint in tracker mm\n");
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        const struct gz_sweep_point *p = &out->pt[i];
        if (p->n_fit == 0) {
            say("  pt %d target %.6f %.6f  NO PAIRED FRAME (gaze %u, frames %u)\n",
                i + 1, p->target[0], p->target[1], p->n_gaze, p->frames);
            continue;
        }
        double ep[2];
        gz_eye_proj(panel, p->eye_mm, ep);
        say("  pt %d target %.6f %.6f  gaze %.6f %.6f  eye_mm %.2f %.2f %.2f"
            "  eproj %.6f %.6f  n %u\n",
            i + 1, p->target[0], p->target[1], p->gaze_mean[0], p->gaze_mean[1],
            p->eye_mm[0], p->eye_mm[1], p->eye_mm[2], ep[0], ep[1], p->n_fit);
    }
    return 0;
}

/* ---------------- accuracy ---------------- */

/* The acceptance bands, in corrected median pixels, and the movement that
 * explains the middle one. Named once because report_corrected prints them and
 * gz_accuracy_verdict_fill fills the same words from them, and two copies of a
 * threshold drift. The movement one is in PIXELS of predicted decay rather
 * than in millimetres: 15 px is 24 mm at GZ_CORR_DEGRADE_PX_PER_MM. */
#define GZ_ACC_FALSIFIED_PX 80.0
#define GZ_ACC_MIDBAND_PX   50.0
#define GZ_ACC_PREDICTED_PX 35.0
#define GZ_ACC_MOVED_PX     15.0

unsigned gz_corrected_stats(const struct gz_sweep *sw, const struct gz_screen *scr,
                            const struct gz_correction *corr, struct gz_corr_stats *out) {
    memset(out, 0, sizeof *out);
    out->moved_mm = -1;

    double raws[GZ_CAL_POINTS], cors[GZ_CAL_POINTS];
    double eye_sum[2] = { 0, 0 };
    unsigned n = 0;
    double worst = 0;
    /* gz_stat_of sorts in place, so every per-point number is taken here and
     * the two medians are computed once the loop is done. */
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        const struct gz_sweep_point *p = &sw->pt[i];
        if (p->n_fit == 0) continue;

        double ep[2], cor[2];
        gz_eye_proj(corr->area, p->eye_mm, ep);
        eye_sum[0] += ep[0];
        eye_sum[1] += ep[1];
        gz_correct_point(corr, p->gaze_mean, cor);
        raws[n] = hypot((p->gaze_mean[0] - p->target[0]) * scr->w,
                        (p->gaze_mean[1] - p->target[1]) * scr->h);
        cors[n] = hypot((cor[0] - p->target[0]) * scr->w,
                        (cor[1] - p->target[1]) * scr->h);
        if (cors[n] > worst) worst = cors[n];
        n++;
    }
    if (n == 0) return 0;

    out->n = n;
    out->median_px = gz_stat_of(cors, n).median;
    out->raw_median_px = gz_stat_of(raws, n).median;
    out->worst_px = worst;
    if (corr->eye_proj[0] != 0.0 || corr->eye_proj[1] != 0.0) {
        double dx = (eye_sum[0] / n - corr->eye_proj[0]) * corr->area.w_mm;
        double dy = (eye_sum[1] / n - corr->eye_proj[1]) * corr->area.h_mm;
        out->moved_mm = hypot(dx, dy);
    }
    return n;
}

/* Reports the sweep against the correction currently on disk, when there is
 * one. Uses the paired means rather than the medians the table above prints,
 * because the correction is affine in both the gaze point and the eye position,
 * so correcting the mean is exactly the mean of the corrected samples. The raw
 * column here is recomputed from the same means, so the two columns are
 * comparable to each other rather than to the table.
 *
 * `out` is not optional and is always written, so the caller has the same
 * numbers this printed without walking the sweep a second time. */
static void report_corrected(const struct gz_sweep *sw, const struct gz_screen *scr,
                             const struct gz_correction *corr,
                             struct gz_corr_stats *out) {
    say("\n  correction applied: gx %.5f gy %.5f bx %+.6f by %+.6f (form S)\n",
        corr->gx, corr->gy, corr->bx, corr->by);
    say("  point         raw px   corrected px\n");

    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        const struct gz_sweep_point *p = &sw->pt[i];
        if (p->n_fit == 0) continue;

        double cor[2];
        gz_correct_point(corr, p->gaze_mean, cor);
        double rawe = hypot((p->gaze_mean[0] - p->target[0]) * scr->w,
                            (p->gaze_mean[1] - p->target[1]) * scr->h);
        double core = hypot((cor[0] - p->target[0]) * scr->w,
                            (cor[1] - p->target[1]) * scr->h);
        say("  %d/%d (%.1f,%.1f) %8.0f %12.0f\n",
            i + 1, GZ_CAL_POINTS, p->target[0], p->target[1], rawe, core);
    }

    gz_corrected_stats(sw, scr, corr, out);
    if (out->n == 0) {
        say("  no point carried both a gaze sample and an eye position\n");
        return;
    }

    say("\nCORRECTED median %.0f px, worst %.0f px, over %u points"
        " (raw median %.0f px)\n", out->median_px, out->worst_px, out->n,
        out->raw_median_px);
    say("verdict: %s (target %.0f px, one degree at 600 mm)\n",
        out->median_px <= GZ_ACC_TARGET_PX ? "WITHIN ONE DEGREE" : "OUTSIDE ONE DEGREE",
        GZ_ACC_TARGET_PX);

    /* How far this sweep sat from the seat the fit was taken at, and what form
     * S costs for it. This is the first thing to check on a degraded run, and
     * with form S it is the only term that moved. */
    double moved_px = -1;
    if (out->moved_mm >= 0.0) {
        moved_px = out->moved_mm * GZ_CORR_DEGRADE_PX_PER_MM;
        say("head moved %.0f mm in the screen plane from the seat this fit was"
            " taken at,\n  which costs form S about %.0f px at %.2f px/mm."
            " Depth costs nothing.\n",
            out->moved_mm, moved_px, GZ_CORR_DEGRADE_PX_PER_MM);
    }

    /* Reported rather than judged into a single pass or fail: the middle band
     * has a specific and checkable cause under form S, and it is printed just
     * above rather than left for the operator to guess at. */
    say("acceptance: %s\n",
        out->median_px > GZ_ACC_FALSIFIED_PX
            ? "ABOVE 80 px, the affine model is FALSIFIED"
      : out->median_px > GZ_ACC_MIDBAND_PX ? (moved_px > GZ_ACC_MOVED_PX
            ? "50 to 80 px, and the head has left the fitted seat: re-fit where"
              " you actually sit"
            : "50 to 80 px with the head still at the fitted seat, so movement"
              " does not explain it")
      : out->median_px >= GZ_ACC_PREDICTED_PX ? "35 to 50 px, as predicted"
                          : "under 35 px, better than predicted");
}

/* The two gate refusals, which are the same words for both commands and are
 * kept apart for the same reason display.h keeps the codes apart: only the
 * mismatch has a remedy. */
static void gate_verdict_fill(int kind, int g, struct gz_sweep_verdict *out) {
    memset(out, 0, sizeof *out);
    out->kind = kind;
    out->rc = g;
    out->refused = 1;
    out->moved_mm = -1;
    if (g == GZ_GATE_MISMATCH) {
        snprintf(out->reason, sizeof out->reason,
                 "the device does not hold the configured display area");
        snprintf(out->next, sizeof out->next,
                 "fix it with tobiifreed --force-display-area, then run this again");
    } else {
        snprintf(out->reason, sizeof out->reason,
                 "the daemon didn't answer, so the display area could not be read");
        snprintf(out->next, sizeof out->next,
                 "start tobiifreed, then run this again");
    }
}

void gz_accuracy_verdict_fill(int have_corr, const struct gz_corr_stats *cs,
                              double raw_median_px, double raw_worst_px,
                              struct gz_sweep_verdict *out) {
    memset(out, 0, sizeof *out);
    out->kind = GZ_VERDICT_ACCURACY;
    out->moved_mm = -1;
    if (!have_corr || cs == NULL || cs->n == 0) {
        out->median_px = raw_median_px;
        out->worst_px = raw_worst_px;
        out->within_one_degree = raw_median_px <= GZ_ACC_TARGET_PX;
        snprintf(out->next, sizeof out->next,
                 "these are the device's raw numbers. Run a fit");
        return;
    }
    out->corrected = 1;
    out->n_used = (int)cs->n;
    out->median_px = cs->median_px;
    out->worst_px = cs->worst_px;
    out->within_one_degree = cs->median_px <= GZ_ACC_TARGET_PX;
    out->moved_mm = cs->moved_mm;

    /* The same bands, in the same order, off the same constants as the
     * acceptance line above, including the middle one splitting on the cost in
     * pixels rather than on the millimetres themselves. */
    double moved_px = cs->moved_mm >= 0.0 ? cs->moved_mm * GZ_CORR_DEGRADE_PX_PER_MM : -1;
    if (cs->median_px > GZ_ACC_FALSIFIED_PX) {
        snprintf(out->next, sizeof out->next, "above 80 px: the affine model is FALSIFIED");
    } else if (cs->median_px > GZ_ACC_MIDBAND_PX) {
        snprintf(out->next, sizeof out->next, moved_px > GZ_ACC_MOVED_PX
                 ? "50 to 80 px and the head has left the fitted seat: re-fit where you sit"
                 : "50 to 80 px with the head at the fitted seat, so movement does not explain it");
    } else if (cs->median_px >= GZ_ACC_PREDICTED_PX) {
        snprintf(out->next, sizeof out->next, "35 to 50 px, as predicted");
    } else {
        snprintf(out->next, sizeof out->next, "under 35 px, better than predicted");
    }
}

int gz_accuracy_core(const char *sock, const char *cfg, const char *label,
                     const struct gz_stim_ops *stim, const struct gz_screen *scr,
                     struct gz_sweep_verdict *out) {
    log_open("accuracy", label);
    say("screen: %s %dx%d at +%d+%d\n", scr->name, scr->w, scr->h, scr->x, scr->y);

    struct gz_client c;
    struct gz_rect panel;
    int g = gz_connect_and_gate(&c, sock, cfg, 1, &panel);
    if (g != GZ_GATE_OK) {
        gate_verdict_fill(GZ_VERDICT_ACCURACY, g, out);
        gz_client_close(&c);
        log_close();
        return g;
    }
    say("status: device_present=%u calibration_applied=%u (the daemon's own flag,\n"
        "  set by cal_apply and cleared by a restart: it is not read from the device)\n",
        c.status.device_present, c.status.calibration_applied);

    /* Loaded before the sweep so a refusal is on screen before the human spends
     * twelve seconds staring at dots. `panel` is what the gate just proved the
     * device is holding, which is the geometry the stored fit must match. */
    struct gz_correction corr;
    int have_corr = gz_correction_load(panel, &corr) == 1;

    struct gz_sweep sw;
    if (run_sweep(&c, stim, scr, panel, &sw) != 0) {
        memset(out, 0, sizeof *out);
        out->kind = GZ_VERDICT_ACCURACY;
        out->rc = 1;
        out->refused = 1;
        out->moved_mm = -1;
        snprintf(out->reason, sizeof out->reason, "the sweep was interrupted");
        snprintf(out->next, sizeof out->next, "re-run the sweep");
        gz_client_close(&c);
        log_close();
        return 1;
    }

    if (sw.n_gaze_ok == 0) {
        say("\nNO VALID GAZE AT ANY POINT. Either nobody was looking at the screen or\n"
            "the tracker is not seeing eyes. Nothing here says anything about\n"
            "calibration.\n");
        memset(out, 0, sizeof *out);
        out->kind = GZ_VERDICT_ACCURACY;
        out->rc = 1;
        out->refused = 1;
        out->moved_mm = -1;
        snprintf(out->reason, sizeof out->reason, "no valid gaze at any point");
        snprintf(out->next, sizeof out->next, "look at each dot, with the lights on");
        gz_client_close(&c);
        log_close();
        return 1;
    }

    double sorted[GZ_CAL_POINTS];
    unsigned ns = 0;
    double worst = 0;
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        if (sw.pt[i].err_px < 0) continue;
        sorted[ns++] = sw.pt[i].err_px;
        if (sw.pt[i].err_px > worst) worst = sw.pt[i].err_px;
    }
    struct gz_stat es = gz_stat_of(sorted, ns);
    struct gz_stat zs = gz_stat_of(sw.zbuf, sw.nz);

    /* The raw line is deliberately labelled rather than left to be read as the
     * result. With a correction loaded it is the BEFORE figure, and it always
     * says OUTSIDE ONE DEGREE, so an operator who stops reading here concludes
     * a passing run failed. The verdict that matters is printed last, by
     * report_corrected. */
    say("\n%smedian error %.0f px, worst %.0f px, over %d of %d points\n",
        have_corr ? "BEFORE CORRECTION: " : "", es.median, worst,
        sw.n_gaze_ok, GZ_CAL_POINTS);
    if (sw.nz > 0) say("eye distance |z| median %.0f mm\n", fabs(zs.median));
    struct gz_corr_stats cs;
    memset(&cs, 0, sizeof cs);
    if (have_corr) {
        say("this is the device's raw output, not the result. The corrected\n"
            "figures are below.\n");
        report_corrected(&sw, scr, &corr, &cs);
    } else {
        say("verdict: %s (target %.0f px, one degree at 600 mm)\n",
            es.median <= GZ_ACC_TARGET_PX ? "WITHIN ONE DEGREE" : "OUTSIDE ONE DEGREE",
            GZ_ACC_TARGET_PX);
        say("\nno host-side correction on disk. The numbers above are the device's\n"
            "raw output. Run `gaze-cal fit` to fit one.\n");
    }
    gz_accuracy_verdict_fill(have_corr, &cs, es.median, worst, out);

    gz_client_close(&c);
    log_close();
    return 0;
}

int gz_cmd_accuracy(const char *sock, const char *cfg, const char *label,
                    const struct gz_stim_ops *stim, const struct gz_screen *scr) {
    struct gz_sweep_verdict v;
    return gz_accuracy_core(sock, cfg, label, stim, scr, &v);
}

/* ---------------- fit ---------------- */

int gz_missing_cause(const int *paired, const double *eye_z, int n, double *out_med_z) {
    double z[GZ_CAL_POINTS];
    unsigned nz = 0;
    int top_row_lost = 0, other_lost = 0;

    if (n > GZ_CAL_POINTS) n = GZ_CAL_POINTS;
    for (int i = 0; i < n; i++) {
        if (paired[i]) {
            /* A zero is the eyeless placeholder rather than a measurement, and
             * fabs because the sign of z is the tracker's, not a distance. */
            if (eye_z[i] != 0.0) z[nz++] = fabs(eye_z[i]);
        } else if (i < 3) {
            top_row_lost++;
        } else {
            other_lost++;
        }
    }

    double med = nz > 0 ? gz_stat_of(z, nz).median : 0.0;
    if (out_med_z != NULL) *out_med_z = med;

    if (nz == 0 || !(med < GZ_FIT_TOO_CLOSE_MM)) return GZ_MISS_LIGHTS;
    if (top_row_lost > 0 && other_lost == 0) return GZ_MISS_TOO_CLOSE;
    return GZ_MISS_CLOSE;
}

static void say_missing_points(const struct gz_sweep *sw) {
    int paired[GZ_CAL_POINTS];
    double z[GZ_CAL_POINTS];

    say("missing:");
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        paired[i] = sw->pt[i].n_fit > 0;
        z[i] = sw->pt[i].eye_mm[2];
        if (!paired[i]) say(" %d(%.1f,%.1f)", i + 1, sw->pt[i].target[0], sw->pt[i].target[1]);
    }
    say("\n");

    double med = 0;
    int cause = gz_missing_cause(paired, z, GZ_CAL_POINTS, &med);
    if (med > 0) say("eye distance |z| median over the points that did work: %.0f mm\n", med);

    if (cause == GZ_MISS_TOO_CLOSE) {
        say("YOU ARE TOO CLOSE. At %.0f mm the top row sits outside the tracker's\n"
            "view, which is exactly why that row and only that row is missing.\n"
            "Move back to about 600 mm, roughly where you play, and re-run.\n", med);
    } else if (cause == GZ_MISS_CLOSE) {
        say("You are at %.0f mm, closer than the ~600 mm playing position. Move\n"
            "back and re-run. If points are still missing after that, turn the\n"
            "room lights up, which this tracker needs.\n", med);
    } else {
        say("Turn the room lights on and re-run: with them off this tracker loses\n"
            "the top-left point entirely, and that has cost three runs already.\n");
    }
}

const char *gz_fit_err_text(int rc) {
    switch (rc) {
    case GZ_FIT_ERR_POINTS:  return "too few usable points";
    case GZ_FIT_ERR_FLAT:    return "the stimulus did not span an axis";
    case GZ_FIT_ERR_OUTLIER: return "more than one point sat past 3x the median residual";
    case GZ_FIT_ERR_BOUNDS:  return "the fitted gain is outside the measured envelope";
    case GZ_FIT_ERR_REFIT:   return "a point still sat past 3x the median after the refit";
    default:                 return "unknown";
    }
}

void gz_fit_verdict_fill(int fit_rc, const struct gz_fit_report *rep,
                         const struct gz_correction *corr, double mm_per_px,
                         struct gz_sweep_verdict *out) {
    memset(out, 0, sizeof *out);
    out->kind = GZ_VERDICT_FIT;
    out->n_used = rep->n_used;
    out->n_rejected = rep->n_rejected;
    out->median_px = rep->median_resid_mm / mm_per_px;
    out->worst_px = rep->worst_resid_mm / mm_per_px;
    out->within_one_degree = out->median_px <= GZ_ACC_TARGET_PX;
    out->moved_mm = -1;
    if (fit_rc != GZ_FIT_OK) {
        out->rc = 1;
        out->refused = 1;
        /* The same point the terminal names, and the same one-based number. */
        if (fit_rc == GZ_FIT_ERR_REFIT && rep->refit_outlier >= 0) {
            snprintf(out->reason, sizeof out->reason,
                     "%s: point %d survived the rejection and still sits past 3x the median",
                     gz_fit_err_text(fit_rc), rep->refit_outlier + 1);
        } else {
            snprintf(out->reason, sizeof out->reason, "%s", gz_fit_err_text(fit_rc));
        }
        snprintf(out->next, sizeof out->next,
                 "nothing was written. Re-run the sweep, holding each dot until it moves");
        return;
    }
    out->gx = corr->gx; out->gy = corr->gy; out->bx = corr->bx; out->by = corr->by;
    snprintf(out->next, sizeof out->next, "now verify, without moving");
}

/* The partial sweep, in the words say_missing_points prints. */
void gz_missing_verdict_fill(const struct gz_sweep *sw, struct gz_sweep_verdict *out) {
    memset(out, 0, sizeof *out);
    out->kind = GZ_VERDICT_FIT;
    out->rc = 1;
    out->refused = 1;
    out->moved_mm = -1;

    int paired[GZ_CAL_POINTS];
    double z[GZ_CAL_POINTS];
    char list[64] = "";
    size_t at = 0;
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        paired[i] = sw->pt[i].n_fit > 0;
        z[i] = sw->pt[i].eye_mm[2];
        if (paired[i]) continue;
        int w = snprintf(list + at, sizeof list - at, "%s%d", at ? ", " : "", i + 1);
        if (w > 0 && (size_t)w < sizeof list - at) at += (size_t)w;
    }
    snprintf(out->reason, sizeof out->reason,
             "only %d of %d points carried a gaze sample and an eye position (missing %s)",
             sw->n_fit_ok, GZ_CAL_POINTS, list);

    double med = 0;
    int cause = gz_missing_cause(paired, z, GZ_CAL_POINTS, &med);
    if (cause == GZ_MISS_TOO_CLOSE) {
        snprintf(out->next, sizeof out->next,
                 "too close at %.0f mm: the top row is outside the tracker's view."
                 " Sit at about 600 mm", med);
    } else if (cause == GZ_MISS_CLOSE) {
        snprintf(out->next, sizeof out->next,
                 "at %.0f mm, closer than the 600 mm playing position. Move back,"
                 " then the lights", med);
    } else {
        snprintf(out->next, sizeof out->next,
                 "turn the room lights on: with them off this tracker loses the top left point");
    }
}

int gz_fit_core(const char *sock, const char *cfg, const struct gz_stim_ops *stim,
                const struct gz_screen *scr, struct gz_sweep_verdict *out) {
    log_open("fit", NULL);
    say("screen: %s %dx%d at +%d+%d\n", scr->name, scr->w, scr->h, scr->x, scr->y);

    struct gz_client c;
    struct gz_rect panel;
    int g = gz_connect_and_gate(&c, sock, cfg, 1, &panel);
    if (g != GZ_GATE_OK) {
        gate_verdict_fill(GZ_VERDICT_FIT, g, out);
        gz_client_close(&c);
        log_close();
        return g;
    }
    say("status: device_present=%u calibration_applied=%u\n",
        c.status.device_present, c.status.calibration_applied);
    say("\nROOM LIGHTS ON, and sit where you play. THIS FIT IS PER SEATING\n"
        "POSITION: it has no head term, so it decays about 0.63 px for every mm\n"
        "you leave the seat you fitted at. Fit it where you actually sit, and\n"
        "re-fit whenever the seat changes for good. Re-fit as well if the display\n"
        "area changes or you recalibrate the device.\n");

    struct gz_sweep sw;
    if (run_sweep(&c, stim, scr, panel, &sw) != 0) {
        memset(out, 0, sizeof *out);
        out->kind = GZ_VERDICT_FIT;
        out->rc = 1;
        out->refused = 1;
        out->moved_mm = -1;
        snprintf(out->reason, sizeof out->reason, "the sweep was interrupted");
        snprintf(out->next, sizeof out->next, "re-run the sweep");
        gz_client_close(&c);
        log_close();
        return 1;
    }
    gz_client_close(&c);

    /* All nine, no exceptions. A fit from six points looks fine and encodes a
     * row-selection bias instead of a gain, which is exactly the artifact that
     * sent an earlier round of this investigation to the wrong conclusion. */
    if (sw.n_fit_ok != GZ_CAL_POINTS) {
        say("\nONLY %d OF %d POINTS carried both a gaze sample and an eye position.\n"
            "REFUSING TO FIT. A fit from a partial sweep encodes which points were\n"
            "missing rather than the gain.\n", sw.n_fit_ok, GZ_CAL_POINTS);
        say_missing_points(&sw);
        gz_missing_verdict_fill(&sw, out);
        log_close();
        return 1;
    }

    struct gz_fit_input in[GZ_CAL_POINTS];
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        in[i].target[0]   = sw.pt[i].target[0];
        in[i].target[1]   = sw.pt[i].target[1];
        in[i].reported[0] = sw.pt[i].gaze_mean[0];
        in[i].reported[1] = sw.pt[i].gaze_mean[1];
        for (int k = 0; k < 3; k++) in[i].eye_mm[k] = sw.pt[i].eye_mm[k];
    }

    struct gz_correction corr;
    struct gz_fit_report rep;
    int rc = gz_correction_fit(in, GZ_CAL_POINTS, panel, &corr, &rep);

    say("\nfit: gx %.5f  gy %.5f  bx %+.6f  by %+.6f\n",
        corr.gx, corr.gy, corr.bx, corr.by);
    if (corr.gy != 0.0) say("isotropy gx/gy %.4f (measured 0.9991 to 1.0148)\n", corr.gx / corr.gy);
    say("points used %d of %d, rejected %d\n", rep.n_used, rep.n_in, rep.n_rejected);

    double mm_per_px = panel.w_mm / (double)scr->w;
    gz_fit_verdict_fill(rc, &rep, &corr, mm_per_px, out);
    say("per-point residual, corrected minus target:\n");
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        say("  pt %d  %6.1f mm  %6.0f px%s\n", i + 1, rep.resid_mm[i],
            rep.resid_mm[i] / mm_per_px, rep.rejected[i] ? "   REJECTED" : "");
    }
    say("median residual %.1f mm (%.0f px), worst %.1f mm (%.0f px)\n",
        rep.median_resid_mm, rep.median_resid_mm / mm_per_px,
        rep.worst_resid_mm, rep.worst_resid_mm / mm_per_px);

    if (rc != GZ_FIT_OK) {
        say("\nFIT REFUSED: %s.\n"
            "Nothing was written. A wrong correction is worse than none, because\n"
            "downstream it is indistinguishable from a calibration.\n", gz_fit_err_text(rc));
        if (rc == GZ_FIT_ERR_REFIT && rep.refit_outlier >= 0) {
            say("POINT %d is the one. It survived the rejection above and still sits\n"
                "past 3x the median, which means this sweep has a second bad point\n"
                "that its own axis absorbed. Left in, it moves a gain by several\n"
                "percent and nothing downstream would flag it.\n"
                "RE-RUN THE SWEEP, holding each dot until it moves.\n",
                rep.refit_outlier + 1);
        }
        log_close();
        return 1;
    }

    if (gz_correction_save(&corr, &rep, rep.eye_z_mm, mm_per_px) != 0) {
        out->rc = 1;
        out->refused = 1;
        snprintf(out->reason, sizeof out->reason, "couldn't write the correction file");
        snprintf(out->next, sizeof out->next, "check the disk, then re-run the fit");
        log_close();
        return 1;
    }
    say("\nFIT DONE. Next, without moving: gaze-cal accuracy --label verify\n"
        "Optionally, move well to one side, or clearly up or down, and run\n"
        "  gaze-cal accuracy --label lateral\n"
        "which measures how fast the correction decays as you leave this seat,\n"
        "about 0.63 px per mm. It decides nothing: spec test 5.3 ran on\n"
        "2026-07-27 and chose this form. A change in DEPTH measures nothing\n"
        "either, because the correction has no depth term.\n");
    log_close();
    return 0;
}

int gz_cmd_fit(const char *sock, const char *cfg,
               const struct gz_stim_ops *stim, const struct gz_screen *scr) {
    struct gz_sweep_verdict v;
    return gz_fit_core(sock, cfg, stim, scr, &v);
}

/* ---------------- probe ----------------
 *
 * Settles two things that no document in this project records and that both
 * have to be right BEFORE a calibration is burned:
 *
 *   1. the sign of the tracker's z axis. eye_origin_*_mm is the eye position
 *      in tracker space, so a seated human reads about +600 or about -600 and
 *      the sign falls out. The screen sits 7.5 mm behind the tracker's front
 *      face, so the config's z_mm is -7.5 if +z points at the user and +7.5 if
 *      it points away. Only the sign was ever in doubt.
 *
 *   2. the orientation of the normalised gaze frame. If the device's y grew
 *      upward while the stimulus assumed downward, calibration would train a
 *      mirrored map and every later measurement would agree with it. Four dots
 *      answer it in eight seconds, uncalibrated, because the default map is
 *      nowhere near wrong enough to flip a 0.8 separation. */

#define GZ_PROBE_MM_IN_FRONT 7.5
#define GZ_PROBE_DOT_MS 2200

static int probe_dot(struct gz_client *c, const struct gz_stim_ops *stim,
                     double nx, double ny, struct gz_stat *ox, struct gz_stat *oy,
                     unsigned *n) {
    struct gz_samples ignore, s;
    if (stim->show(stim->ctx, nx, ny) != 0) return -1;
    if (gz_collect(c, 700, &ignore) == GZ_CLIENT_RECONNECT) return -1;
    if (gz_collect(c, GZ_PROBE_DOT_MS, &s) == GZ_CLIENT_RECONNECT) return -1;
    *n = s.n;
    if (s.n == 0) return -1;
    *ox = gz_stat_of(s.x, s.n);
    *oy = gz_stat_of(s.y, s.n);
    return 0;
}

int gz_cmd_probe(const char *sock, const char *cfg,
                 const struct gz_stim_ops *stim, const struct gz_screen *scr) {
    log_open("probe", NULL);
    say("screen: %s %dx%d at +%d+%d\n", scr->name, scr->w, scr->h, scr->x, scr->y);

    struct gz_client c;
    /* The geometry is printed but not required: this command runs BEFORE the
     * display area is settled, because it is what settles z_mm. */
    int g = gz_connect_and_gate(&c, sock, cfg, 0, NULL);
    if (g != GZ_GATE_OK) {
        gz_client_close(&c);
        log_close();
        return g;
    }

    say("\nLOOK AT THE WHITE DOT. It moves five times, about two seconds each.\n");
    if (stim->show(stim->ctx, 0.5, 0.5) != 0) {
        gz_client_close(&c);
        log_close();
        return 1;
    }
    /* Drained, not slept through. The receive accumulator holds about 41 gaze
     * frames, so 1.5 s of not reading backs the socket up and the window that
     * follows opens on a backlog: the first measurement of this reported 149
     * frames in three seconds against a device that ships 33.2. */
    struct gz_samples warmup;
    if (gz_collect(&c, 1500, &warmup) == GZ_CLIENT_RECONNECT) {
        say("link lost while settling\n");
        gz_client_close(&c);
        log_close();
        return 1;
    }

    struct gz_samples s;
    if (gz_collect(&c, 3000, &s) == GZ_CLIENT_RECONNECT) {
        say("link lost\n");
        gz_client_close(&c);
        log_close();
        return 1;
    }
    say("\ncentre dot: %u frames, %u with both eyes, %u with an eye origin\n",
        s.n_total, s.n_both, s.nz);

    if (s.nz == 0) {
        say("NO EYE ORIGIN IN ANY FRAME. Nothing here settles the z sign. Sit in\n"
            "front of the tracker, look at the screen, and run this again.\n");
        gz_client_close(&c);
        log_close();
        return 1;
    }

    struct gz_stat zs = gz_stat_of(s.z, s.nz);
    struct gz_stat exs = gz_stat_of(s.ex, s.nex);
    struct gz_stat eys = gz_stat_of(s.ey, s.ney);
    say("eye origin, tracker space, mm: x %.0f  y %.0f  z %.0f  (z spread %.0f to %.0f)\n",
        exs.median, eys.median, zs.median, zs.p05, zs.p95);

    double recommended = 0;
    int settled = 0;
    if (fabs(zs.median) < 100.0) {
        say("\nZ SIGN UNSETTLED: |z| is %.0f mm, and a seated head is 400 to 900 mm\n"
            "away. Either nobody was in front of the tracker or this field is not\n"
            "what it is documented to be. Do not change z_mm on this.\n", fabs(zs.median));
    } else if (zs.median > 0) {
        settled = 1;
        recommended = -GZ_PROBE_MM_IN_FRONT;
        say("\nZ SIGN: +z POINTS AT THE USER (eye z = %+.0f mm), which is Tobii's\n"
            "published UCS. The screen surface sits %.1f mm BEHIND the tracker's\n"
            "front face, so it is at negative z.\n", zs.median, GZ_PROBE_MM_IN_FRONT);
    } else {
        settled = 1;
        recommended = GZ_PROBE_MM_IN_FRONT;
        say("\nZ SIGN: +z POINTS AWAY FROM THE USER (eye z = %+.0f mm). The screen\n"
            "surface sits %.1f mm behind the tracker's front face, so it is at\n"
            "positive z.\n", zs.median, GZ_PROBE_MM_IN_FRONT);
    }

    /* Orientation. Uncalibrated gaze is good enough: the question is only
     * which way the numbers move over a 0.8 normalised separation. */
    struct gz_stat tx, ty, bx, by, lx, ly, rx, ry;
    unsigned nt = 0, nb = 0, nl = 0, nr = 0;
    int ok = probe_dot(&c, stim, 0.5, 0.1, &tx, &ty, &nt) == 0
          && probe_dot(&c, stim, 0.5, 0.9, &bx, &by, &nb) == 0
          && probe_dot(&c, stim, 0.1, 0.5, &lx, &ly, &nl) == 0
          && probe_dot(&c, stim, 0.9, 0.5, &rx, &ry, &nr) == 0;

    int y_ok = 0, x_ok = 0;
    if (!ok) {
        say("\nORIENTATION UNSETTLED: at least one dot produced no valid gaze\n"
            "(top %u, bottom %u, left %u, right %u frames).\n", nt, nb, nl, nr);
    } else {
        double dy = by.median - ty.median;
        double dx = rx.median - lx.median;
        say("\ntop dot y %.3f, bottom dot y %.3f, difference %+.3f\n",
            ty.median, by.median, dy);
        say("left dot x %.3f, right dot x %.3f, difference %+.3f\n",
            lx.median, rx.median, dx);

        if (fabs(dy) < 0.2) {
            say("Y AXIS UNSETTLED: the two dots moved gaze by only %.3f.\n", fabs(dy));
        } else if (dy > 0) {
            y_ok = 1;
            say("Y AXIS: normalised y grows DOWNWARD, which is what the stimulus\n"
                "  assumes. The nine points are in the frame the device expects.\n");
        } else {
            say("Y AXIS INVERTED: normalised y grows UPWARD. STOP. Calibrating now\n"
                "  would train a mirrored map and every later measurement would\n"
                "  agree with it. GZ_CAL_PTS needs its y flipped first.\n");
        }
        if (fabs(dx) < 0.2) {
            say("X AXIS UNSETTLED: the two dots moved gaze by only %.3f.\n", fabs(dx));
        } else if (dx > 0) {
            x_ok = 1;
            say("X AXIS: normalised x grows RIGHTWARD, as assumed.\n");
        } else {
            say("X AXIS INVERTED: normalised x grows LEFTWARD. STOP, as above.\n");
        }
    }

    say("\n---- what to do with this ----\n");
    if (settled) {
        say("Set z_mm to %+.1f in ~/.config/tobii.json, then restart the daemon once\n"
            "with --force-display-area and confirm with `gaze-cal display`.\n"
            "Calibration is computed in the display-area frame, so this has to be\n"
            "right BEFORE calibrating, not after.\n", recommended);
    } else {
        say("The z sign is not settled. Do not guess it.\n");
    }
    say("Then, and only if both axes read as expected: gaze-cal calibrate\n");

    gz_client_close(&c);
    log_close();
    return (settled && y_ok && x_ok) ? 0 : 1;
}

/* ---------------- preview ----------------
 *
 * Raw normalised gaze on stdout, one sample per line, so the persistence
 * experiment has a data path that does not depend on any of the analysis
 * above. */
int gz_cmd_preview(const char *sock, long nsamples) {
    struct gz_client c;
    if (gz_client_connect(&c, sock) != 0) {
        fprintf(stderr, "connect %s: %s\n", sock, strerror(errno));
        return 1;
    }
    if (gz_display_gate_status(&c, GZ_CLIENT_CMD_TIMEOUT_MS) != 0) {
        gz_client_close(&c);
        return 1;
    }

    long got = 0;
    uint32_t last = 0;
    int have_last = 0;
    uint64_t end = gz_now_ns() + 120000000000ULL;   /* 120 s, so it cannot hang */

    while (got < nsamples && gz_now_ns() < end) {
        int r = gz_client_poll(&c, 50);
        if (r == GZ_CLIENT_RECONNECT) {
            fprintf(stderr, "link lost after %ld samples\n", got);
            gz_client_close(&c);
            return 1;
        }
        if (r <= 0 || !c.have_latest) continue;
        if (have_last && c.latest.frame_counter == last) continue;
        last = c.latest.frame_counter;
        have_last = 1;
        printf("%.6f %.6f %d\n", c.latest.gaze_point_2d_norm[0],
               c.latest.gaze_point_2d_norm[1], gz_sample_any_eye_valid(&c.latest));
        got++;
    }
    fprintf(stderr, "%ld samples\n", got);
    gz_client_close(&c);
    return got == nsamples ? 0 : 1;
}
