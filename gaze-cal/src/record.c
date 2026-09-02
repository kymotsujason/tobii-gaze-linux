/* gaze-cal/src/record.c - see record.h for what a trace is for.
 *
 * The socket is driven through gz_client_poll and gz_client_feed like every
 * other command here, not by reading c->fd directly: the client owns the
 * incremental framing, the mandatory subscribe and the watchdog, and a second
 * reader would have to reimplement all three to be correct.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "calibrate.h"
#include "display.h"
#include "record.h"

/* Well under the 30.1 ms the device delivers at, so a poll normally returns
 * with at most one new sample in it. c->latest holds one sample, so two frames
 * arriving in the same read cost the first one; the gap counter below reports
 * that honestly rather than hiding it. */
#define REC_POLL_MS 10

/* A crash mid-session keeps everything up to the last flush. One second is
 * about 33 rows. */
#define REC_FLUSH_NS 1000000000ULL

static volatile sig_atomic_t g_stop = 0;

static void on_stop(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ---------------- the pure half ---------------- */

/* Returns 0 when the value is too wide for the scratch field. snprintf
 * truncates in silence, so without this a corrected coordinate of 1e300 would
 * be written as its first 31 digits and read back as a real measurement. */
static int fmt_corr(char *buf, size_t cap, int have, double v) {
    int n = have ? snprintf(buf, cap, "%.6f", v) : snprintf(buf, cap, "nan");
    return n >= 0 && (size_t)n < cap;
}

size_t gz_record_row(char *buf, size_t cap, const struct gz_gaze_sample *s,
                     const struct gz_correction *corr, uint64_t host_ns) {
    /* A zeroed-but-never-loaded correction has gains of 0.0, so applying it
     * would divide by zero and write inf into the trace. */
    int usable = (corr != NULL && corr->valid);
    double cl[2] = {0, 0}, cr[2] = {0, 0}, cc[2] = {0, 0};
    int hl = 0, hr = 0, hc = 0;

    if (usable) {
        if (gz_eye_valid(s->validity_L)) {
            gz_correct_point(corr, s->gaze_point_2d_L_norm, cl);
            hl = 1;
        }
        if (gz_eye_valid(s->validity_R)) {
            gz_correct_point(corr, s->gaze_point_2d_R_norm, cr);
            hr = 1;
        }
        hc = gz_gaze_correct(corr, s, cc);
    }

    char t[6][32];
    int ok = 1;
    ok &= fmt_corr(t[0], sizeof t[0], hl, cl[0]);
    ok &= fmt_corr(t[1], sizeof t[1], hl, cl[1]);
    ok &= fmt_corr(t[2], sizeof t[2], hr, cr[0]);
    ok &= fmt_corr(t[3], sizeof t[3], hr, cr[1]);
    ok &= fmt_corr(t[4], sizeof t[4], hc, cc[0]);
    ok &= fmt_corr(t[5], sizeof t[5], hc, cc[1]);
    if (!ok) return 0;

    /* Formatted into scratch and copied only on success, so a row that does
     * not fit leaves the caller's buffer alone rather than half written. */
    char row[GZ_RECORD_ROW_MAX];
    int n = snprintf(row, sizeof row,
                     "%llu,%lld,%u,%u,%u,%u,"
                     "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.3f,%.3f,"
                     "%s,%s,%s,%s,%s,%s\n",
                     (unsigned long long)host_ns, (long long)s->timestamp_us,
                     s->frame_counter, s->present_mask, s->validity_L, s->validity_R,
                     s->gaze_point_2d_L_norm[0], s->gaze_point_2d_L_norm[1],
                     s->gaze_point_2d_R_norm[0], s->gaze_point_2d_R_norm[1],
                     s->gaze_point_2d_norm[0], s->gaze_point_2d_norm[1],
                     s->gaze_point_2d_unfiltered[0], s->gaze_point_2d_unfiltered[1],
                     s->pupil_L_mm, s->pupil_R_mm,
                     t[0], t[1], t[2], t[3], t[4], t[5]);
    if (n < 0 || (size_t)n >= sizeof row) return 0;
    if ((size_t)n >= cap) return 0;

    memcpy(buf, row, (size_t)n + 1);
    return (size_t)n;
}

int gz_record_decide(int load_rc, int raw) {
    if (raw) return GZ_REC_RAW;
    if (load_rc == 1) return GZ_REC_CORRECTED;
    return GZ_REC_REFUSE;
}

/* ---------------- the file half ---------------- */

/* traces/ is gitignored and therefore routinely absent on a fresh clone, and
 * failing a five-minute recording on a missing directory is a bad trade. Only
 * the immediate parent, because anything deeper is a typo rather than an
 * intention. */
static void ensure_parent_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) return;

    char dir[512];
    size_t n = (size_t)(slash - path);
    if (n >= sizeof dir) return;
    memcpy(dir, path, n);
    dir[n] = '\0';
    mkdir(dir, 0755);
}

/* The trace says which fit produced it, by carrying a byte copy of the
 * correction file beside it. Without this a trace is a set of numbers nobody
 * can attribute, and the fit on disk is overwritten by the next `gaze-cal
 * fit`. Returns 0, or -1 after printing why. */
static int copy_correction_beside(const char *trace_path, const char *corr_path) {
    char dest[600];
    if (snprintf(dest, sizeof dest, "%s.correction.conf", trace_path) >= (int)sizeof dest) {
        fprintf(stderr, "trace path too long to write the correction beside it\n");
        return -1;
    }

    FILE *in = fopen(corr_path, "rb");
    if (in == NULL) {
        fprintf(stderr, "cannot read %s: %s\n", corr_path, strerror(errno));
        return -1;
    }
    FILE *out = fopen(dest, "wb");
    if (out == NULL) {
        fprintf(stderr, "cannot write %s: %s\n", dest, strerror(errno));
        fclose(in);
        return -1;
    }

    char b[4096];
    size_t r;
    int ok = 1;
    while ((r = fread(b, 1, sizeof b, in)) > 0) {
        if (fwrite(b, 1, r, out) != r) { ok = 0; break; }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;

    if (!ok) {
        fprintf(stderr, "cannot copy %s to %s: %s\n", corr_path, dest, strerror(errno));
        return -1;
    }
    fprintf(stderr, "provenance: %s\n", dest);
    return 0;
}

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    /* No SA_RESTART on purpose: the signal has to break poll() so that ctrl-C
     * ends the recording within one poll interval rather than at the deadline.
     * gz_client_poll maps EINTR to 0, so an interrupted poll is not an error. */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int gz_cmd_record(const struct gz_record_opts *o) {
    if (o->path == NULL || o->path[0] == '\0') {
        fprintf(stderr, "record wants a path to write\n");
        return 2;
    }
    if (o->seconds == 0) {
        fprintf(stderr, "record wants a positive number of seconds\n");
        return 2;
    }

    struct gz_client c;
    struct gz_rect panel;
    int g = gz_connect_and_gate(&c, o->sock, o->cfg, 1, &panel);
    if (g != GZ_GATE_OK) {
        gz_client_close(&c);
        return g;
    }

    /* Decided before the file is created, so a refusal leaves no half-trace
     * behind for someone to find later and trust. */
    struct gz_correction corr;
    memset(&corr, 0, sizeof corr);
    int load_rc = o->raw ? 0 : gz_correction_load(panel, &corr);
    int mode = gz_record_decide(load_rc, o->raw);

    if (mode == GZ_REC_REFUSE) {
        if (load_rc == 0) {
            fprintf(stderr,
                    "no host-side correction on disk. REFUSING to record.\n"
                    "The device's gaze carries an isotropic gain of about 1.18 that\n"
                    "only the correction removes, so a raw trace isn't a trace of\n"
                    "what the overlay will show and can't fit its filter.\n"
                    "Run `gaze-cal fit` where you sit, or pass --raw if you really\n"
                    "want an uncorrected trace.\n");
        } else {
            fprintf(stderr,
                    "REFUSING to record: the correction on disk is unusable, for the\n"
                    "reason printed above. Run `gaze-cal fit`, or pass --raw if you\n"
                    "really want an uncorrected trace.\n");
        }
        gz_client_close(&c);
        return 1;
    }

    if (mode == GZ_REC_RAW) {
        fprintf(stderr,
                "WARNING: --raw. Every corr_* column in this trace will be nan.\n"
                "WARNING: this trace is UNCORRECTED and MUST NOT be used to fit the\n"
                "WARNING: overlay filter. It is a link and timing check, nothing more.\n");
    } else {
        fprintf(stderr, "correction: gx=%.6f gy=%.6f bx=%.6f by=%.6f, fitted at seat "
                        "(%.4f, %.4f)\n",
                corr.gx, corr.gy, corr.bx, corr.by, corr.eye_proj[0], corr.eye_proj[1]);
    }

    ensure_parent_dir(o->path);
    FILE *f = fopen(o->path, "w");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s: %s\n", o->path, strerror(errno));
        gz_client_close(&c);
        return 1;
    }
    if (fputs(GZ_RECORD_HEADER, f) == EOF) {
        fprintf(stderr, "cannot write %s: %s\n", o->path, strerror(errno));
        fclose(f);
        gz_client_close(&c);
        return 1;
    }

    if (mode == GZ_REC_CORRECTED) {
        char cpath[600];
        if (gz_correction_path(cpath, sizeof cpath) != 0 ||
            copy_correction_beside(o->path, cpath) != 0) {
            fprintf(stderr, "REFUSING to record a trace nobody can attribute to a fit\n");
            fclose(f);
            remove(o->path);
            gz_client_close(&c);
            return 1;
        }
    }

    install_handlers();

    uint64_t start = gz_now_ns();
    uint64_t deadline = start + (uint64_t)o->seconds * 1000000000ULL;
    uint64_t next_flush = start + REC_FLUSH_NS;
    unsigned long rows = 0, valid_rows = 0, oversize = 0;
    unsigned long long gaps = 0;
    uint32_t last_counter = 0;
    int have_last = 0;
    int failed = 0;

    fprintf(stderr, "recording %u s to %s. Ctrl-C ends it early.\n", o->seconds, o->path);

    while (!g_stop && gz_now_ns() < deadline) {
        int r = gz_client_poll(&c, REC_POLL_MS);
        if (r == GZ_CLIENT_RECONNECT) {
            fprintf(stderr, "link lost after %lu rows. Keeping the file.\n", rows);
            failed = 1;
            break;
        }

        int w = gz_client_watchdog(&c, gz_now_ns());
        if (w != GZ_LINK_OK) {
            fprintf(stderr, "%s after %lu rows. Keeping the file.\n",
                    w == GZ_LINK_STALE ? "the daemon says the device is gone"
                                       : "watchdog: gaze stopped with no explanation",
                    rows);
            failed = 1;
            break;
        }

        if (r <= 0 || !c.have_latest) continue;
        /* c.latest survives the frame it arrived in, so the have_latest clear
         * below is what stops a row being written twice. The counter check is
         * the second guard, and it is the one that also catches a duplicate
         * frame from the device. */
        if (have_last && c.latest.frame_counter == last_counter) {
            c.have_latest = 0;
            continue;
        }
        if (have_last) gaps += gz_frames_dropped(last_counter, c.latest.frame_counter);
        last_counter = c.latest.frame_counter;
        have_last = 1;

        char row[GZ_RECORD_ROW_MAX];
        /* c.last_gaze_ns is stamped inside gz_client_poll immediately after
         * recv and before any parsing, which is exactly the stamp the brief
         * asks for and a closer one than re-reading the clock out here. */
        size_t n = gz_record_row(row, sizeof row, &c.latest,
                                 mode == GZ_REC_CORRECTED ? &corr : NULL,
                                 c.last_gaze_ns);
        c.have_latest = 0;
        if (n == 0) {
            oversize++;
            continue;
        }
        if (fwrite(row, 1, n, f) != n) {
            fprintf(stderr, "write %s: %s\n", o->path, strerror(errno));
            failed = 1;
            break;
        }
        rows++;
        if (gz_sample_any_eye_valid(&c.latest)) valid_rows++;

        uint64_t now = gz_now_ns();
        if (now >= next_flush) {
            fflush(f);
            next_flush = now + REC_FLUSH_NS;
        }
    }

    /* Two statements, not one `||`: short-circuiting would skip the fclose on
     * a failed flush and leak the stream on the one path that most needs the
     * file closed properly. */
    int flush_rc = fflush(f);
    int close_rc = fclose(f);
    if (flush_rc != 0 || close_rc != 0) {
        fprintf(stderr, "closing %s: %s\n", o->path, strerror(errno));
        failed = 1;
    }
    gz_client_close(&c);

    double elapsed = (double)(gz_now_ns() - start) / 1e9;
    fprintf(stderr, "%lu rows in %.1f s (%.1f Hz), %lu with an eye tracked, "
                    "%llu samples missed, %llu dropped by the daemon\n",
            rows, elapsed, elapsed > 0 ? (double)rows / elapsed : 0.0,
            valid_rows, gaps, (unsigned long long)c.dropped);
    if (oversize > 0)
        fprintf(stderr, "%lu samples were too wide to format and were skipped\n", oversize);
    if (g_stop) fprintf(stderr, "stopped early on a signal\n");
    if (mode == GZ_REC_RAW)
        fprintf(stderr, "reminder: this trace is UNCORRECTED, not for fitting\n");

    if (failed) return 1;
    if (rows == 0) {
        fprintf(stderr, "no samples at all. The trace is empty and useless.\n");
        return 1;
    }
    return 0;
}
