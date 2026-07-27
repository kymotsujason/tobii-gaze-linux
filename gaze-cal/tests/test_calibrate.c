/* gaze-cal/tests/test_calibrate.c
 *
 * The calibration sequence, driven with no display and no tracker. A forked
 * fake daemon on a socketpair answers each command and RECORDS what it was
 * sent, so the tests assert the exact bytes that would reach the device rather
 * than that the function returned zero.
 *
 * The point of this file is the refusals and the sequence. Every accepting
 * case is paired with a rejecting one, because a gz_calibrate that returned 0
 * without sending anything would pass a suite built only of happy paths, and a
 * gz_blob_load that returned the file contents unchecked would too.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/calibrate.h"
#include "../src/display.h"

#ifdef NDEBUG
#error "test_calibrate.c relies on assert(); do not build it with NDEBUG"
#endif

/* ---------- wire helpers, same shapes as tests/test_client.c ---------- */

static size_t put_hdr(unsigned char *b, uint8_t type, uint32_t len) {
    b[0] = type;
    b[1] = (unsigned char)(len & 0xFF);
    b[2] = (unsigned char)((len >> 8) & 0xFF);
    b[3] = (unsigned char)((len >> 16) & 0xFF);
    b[4] = (unsigned char)((len >> 24) & 0xFF);
    return 5;
}

static size_t put_status(unsigned char *b, uint8_t present, uint8_t cal, uint8_t ver) {
    size_t n = put_hdr(b, GZ_SRV_STATUS, GZ_STATUS_SIZE);
    b[n] = present; b[n + 1] = cal; b[n + 2] = ver;
    return n + GZ_STATUS_SIZE;
}

static size_t put_response(unsigned char *b, uint8_t cmd, const void *body, size_t len) {
    size_t n = put_hdr(b, GZ_SRV_RESPONSE, (uint32_t)(len + 1));
    b[n++] = cmd;
    if (len) memcpy(b + n, body, len);
    return n + len;
}

static size_t put_err(unsigned char *b, uint32_t code) {
    size_t n = put_hdr(b, GZ_SRV_ERR, 4);
    b[n] = (unsigned char)(code & 0xFFu);
    b[n + 1] = (unsigned char)((code >> 8) & 0xFFu);
    b[n + 2] = (unsigned char)((code >> 16) & 0xFFu);
    b[n + 3] = (unsigned char)((code >> 24) & 0xFFu);
    return n + 4;
}

static size_t put_gaze(unsigned char *b, uint32_t counter, double x, double y,
                       uint32_t vl, uint32_t vr, double z) {
    struct gz_gaze_sample s;
    memset(&s, 0, sizeof s);
    s.present_mask = GZ_BIT_FRAME_COUNTER | GZ_BIT_VALIDITY_L | GZ_BIT_VALIDITY_R
                   | GZ_BIT_GAZE_2D | GZ_BIT_EYE_ORIGIN_L | GZ_BIT_EYE_ORIGIN_R;
    s.frame_counter = counter;
    s.validity_L = vl;
    s.validity_R = vr;
    s.gaze_point_2d_norm[0] = x;
    s.gaze_point_2d_norm[1] = y;
    s.eye_origin_L_mm[2] = z;
    s.eye_origin_R_mm[2] = z;
    size_t n = put_hdr(b, GZ_SRV_GAZE, (uint32_t)sizeof s);
    memcpy(b + n, &s, sizeof s);
    return n + sizeof s;
}

/* ---------- the fake daemon ---------- */

#define FAKE_STEP_CAP 8300
#define FAKE_MAX_STEPS 16

struct step { unsigned char buf[FAKE_STEP_CAP]; size_t len; };

struct fake {
    pid_t pid;
    int fd;
    struct gz_client c;
    char record[128];
};

static int child_read(int fd, unsigned char *b, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

static int child_write(int fd, const unsigned char *b, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, b + sent, n - sent);
        if (w <= 0) return 0;
        sent += (size_t)w;
    }
    return 1;
}

/* Appends [u8 cmd][u32 LE len][payload] so the parent can assert the exact
 * command stream, including the f64 pairs, after the child has exited. */
static int child_record(int rfd, uint8_t cmd, const unsigned char *p, uint32_t len) {
    unsigned char hdr[5];
    put_hdr(hdr, cmd, len);
    if (!child_write(rfd, hdr, 5)) return 0;
    return len == 0 || child_write(rfd, p, len);
}

static void fake_start(struct fake *f, uint8_t present, uint8_t cal, uint8_t ver,
                       const struct step *steps, size_t nsteps, int gaze_after) {
    static int seq;
    snprintf(f->record, sizeof f->record, "/tmp/gz_cal_rec_%d_%d.bin", (int)getpid(), seq++);
    unlink(f->record);

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    f->pid = fork();
    assert(f->pid >= 0);

    if (f->pid == 0) {
        /* The parent may close its end before this child is ever scheduled,
         * and a fake daemon that dies on SIGPIPE reports the test's own
         * teardown as a crash. */
        signal(SIGPIPE, SIG_IGN);
        close(sv[0]);
        int d = sv[1];
        int rfd = open(f->record, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (rfd < 0) _exit(92);

        unsigned char hdr[5];
        if (!child_read(d, hdr, 5) || hdr[0] != GZ_CMD_SUBSCRIBE) _exit(90);
        unsigned char st[16];
        if (!child_write(d, st, put_status(st, present, cal, ver))) _exit(0);

        uint32_t counter = 100;
        int cmds = 0;
        for (size_t i = 0; i < nsteps; i++) {
            if (!child_read(d, hdr, 5)) _exit(cmds);
            uint32_t plen = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8)
                          | ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
            static unsigned char pay[FAKE_STEP_CAP];
            if (plen > sizeof pay) _exit(91);
            if (plen && !child_read(d, pay, plen)) _exit(cmds);
            if (!child_record(rfd, hdr[0], pay, plen)) _exit(93);
            cmds++;

            if (steps[i].len && !child_write(d, steps[i].buf, steps[i].len)) _exit(cmds);

            /* After the reply, so the gap measurement sees a stream that
             * resumes when the command ends, which is the shape the daemon
             * produces: it parks the USB thread for the whole command. */
            for (int g = 0; g < gaze_after; g++) {
                unsigned char gb[512];
                size_t n = put_gaze(gb, counter, 0.5, 0.5, GZ_VALIDITY_VALID,
                                    GZ_VALIDITY_VALID, 600.0);
                counter += GZ_FRAME_COUNTER_STEP;
                if (!child_write(d, gb, n)) _exit(cmds);
            }
        }
        /* Silence, not EOF: a client that sends more must see a timeout rather
         * than a dead peer, because those take different paths. */
        for (;;) {
            unsigned char junk[64];
            ssize_t r = read(d, junk, sizeof junk);
            if (r <= 0) break;
        }
        _exit(cmds);
    }

    close(sv[1]);
    f->fd = sv[0];
    assert(gz_client_adopt(&f->c, f->fd) == 0);
}

/* Returns the number of commands the daemon saw after the subscribe. */
static int fake_stop(struct fake *f) {
    gz_client_close(&f->c);
    int st = 0;
    assert(waitpid(f->pid, &st, 0) == f->pid);
    assert(WIFEXITED(st));
    assert(WEXITSTATUS(st) < 90);
    return WEXITSTATUS(st);
}

/* ---------- reading the record back ---------- */

struct rec { uint8_t cmd; uint32_t len; unsigned char pay[64]; };

static int rec_read(const char *path, struct rec *out, int cap) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    int n = 0;
    while (n < cap) {
        unsigned char hdr[5];
        if (fread(hdr, 1, 5, f) != 5) break;
        uint32_t len = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8)
                     | ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
        out[n].cmd = hdr[0];
        out[n].len = len;
        uint32_t keep = len < sizeof out[n].pay ? len : (uint32_t)sizeof out[n].pay;
        if (keep && fread(out[n].pay, 1, keep, f) != keep) break;
        if (len > keep && fseek(f, (long)(len - keep), SEEK_CUR) != 0) break;
        n++;
    }
    fclose(f);
    return n;
}

static double rec_f64(const struct rec *r, int index) {
    double v;
    memcpy(&v, r->pay + (size_t)index * sizeof(double), sizeof v);
    return v;
}

/* ---------- the stimulus stub ---------- */

struct stub {
    int shown;
    double x[32], y[32];
    int fail_at;               /* 1-based; 0 never fails */
};

static int stub_show(void *ctx, double nx, double ny) {
    struct stub *s = ctx;
    if (s->shown < 32) { s->x[s->shown] = nx; s->y[s->shown] = ny; }
    s->shown++;
    if (s->fail_at != 0 && s->shown == s->fail_at) return -1;
    return 0;
}

/* No settle, a short sample window, and no validity requirement unless the
 * case under test is the validity gate. The paths are the same ones the real
 * timings run through. */
static struct gz_cal_opts fast_opts(void) {
    struct gz_cal_opts o = GZ_CAL_DEFAULTS;
    o.sock_path = NULL;
    o.settle_ms = 0;
    o.sample_ms = 5;
    o.min_valid_frac = 0;
    o.point_retries = 0;
    o.start_timeout_ms = 400;
    o.point_timeout_ms = 400;
    o.finish_timeout_ms = 400;
    o.gap_wait_ms = 20;
    return o;
}

/* ---------------- screen geometry ---------------- */

/* The real gameplay monitor, read off xrandr on 2026-07-27. NOT the "DP-1-2 at
 * +4000+0" the plan, the brief and CLAUDE.md all name: that output does not
 * exist on this machine. */
static const struct gz_screen dp2 = { "DP-2", 4000, 1440, 2560, 1440 };

static void test_points_land_on_the_right_pixels(void) {
    int px, py;

    gz_screen_point_px(&dp2, 0.1, 0.1, &px, &py);
    assert(px == 4000 + 256 && py == 1440 + 144);
    gz_screen_point_px(&dp2, 0.5, 0.5, &px, &py);
    assert(px == 4000 + 1280 && py == 1440 + 720);
    gz_screen_point_px(&dp2, 0.9, 0.9, &px, &py);
    assert(px == 4000 + 2304 && py == 1440 + 1296);

    /* THE REGRESSION THIS FILE EXISTS FOR. The brief hardcoded +4000+0. On a
     * panel whose real offset is +4000+1440 that draws every dot 1440 px above
     * where the eye is looking, and the calibration comes out silently wrong
     * because nothing downstream can tell a wrong stimulus from wrong gaze. */
    struct gz_screen wrong = { "DP-1-2", 4000, 0, 2560, 1440 };
    int wx, wy;
    gz_screen_point_px(&wrong, 0.5, 0.5, &wx, &wy);
    gz_screen_point_px(&dp2, 0.5, 0.5, &px, &py);
    assert(wx == px && wy == py - 1440);

    /* Symmetric about the middle: 0.1 and 0.9 sit the same distance from the
     * two edges. */
    int lo, hi, dummy;
    gz_screen_point_px(&dp2, 0.1, 0.5, &lo, &dummy);
    gz_screen_point_px(&dp2, 0.9, 0.5, &hi, &dummy);
    assert((lo - dp2.x) + (hi - dp2.x) == dp2.w);

    /* Pixel i covers [i, i+1) and its centre is i+0.5, so the pixel nearest a
     * normalised point is round(n*w - 0.5). A plain round(n*w) picks the
     * neighbour whenever the fraction is above a half. The nine points all sit
     * on exact pixel boundaries at 2560 and 1440, so only a screen whose
     * arithmetic is not exact shows the difference. */
    struct gz_screen tiny = { "T", 0, 0, 10, 10 };
    gz_screen_point_px(&tiny, 0.07, 0.65, &px, &py);
    assert(px == 0 && py == 6);
    gz_screen_point_px(&tiny, 0.45, 0.55, &px, &py);
    assert(px == 4 && py == 5);
}

static void test_points_are_clamped_and_nan_safe(void) {
    int px, py;
    gz_screen_point_px(&dp2, -1.0, -1.0, &px, &py);
    assert(px == dp2.x && py == dp2.y);
    gz_screen_point_px(&dp2, 2.0, 2.0, &px, &py);
    assert(px == dp2.x + dp2.w - 1 && py == dp2.y + dp2.h - 1);
    gz_screen_point_px(&dp2, 1.0, 1.0, &px, &py);
    assert(px == dp2.x + dp2.w - 1 && py == dp2.y + dp2.h - 1);

    /* NaN compares false against every bound, so an unguarded clamp would fall
     * through to an indeterminate lround. */
    double nan_v = nan("");
    gz_screen_point_px(&dp2, nan_v, nan_v, &px, &py);
    assert(px == dp2.x + dp2.w / 2 && py == dp2.y + dp2.h / 2);
}

static void test_the_nine_points_are_the_documented_grid(void) {
    int seen[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        double x = GZ_CAL_PTS[i][0], y = GZ_CAL_PTS[i][1];
        int cx = (x == 0.1) ? 0 : (x == 0.5) ? 1 : (x == 0.9) ? 2 : -1;
        int cy = (y == 0.1) ? 0 : (y == 0.5) ? 1 : (y == 0.9) ? 2 : -1;
        assert(cx >= 0 && cy >= 0);
        assert(seen[cy][cx] == 0);
        seen[cy][cx] = 1;
    }
    /* Row-major from the top-left, which is the order the report and the
     * accuracy pass both index by. */
    assert(GZ_CAL_PTS[0][0] == 0.1 && GZ_CAL_PTS[0][1] == 0.1);
    assert(GZ_CAL_PTS[4][0] == 0.5 && GZ_CAL_PTS[4][1] == 0.5);
    assert(GZ_CAL_PTS[8][0] == 0.9 && GZ_CAL_PTS[8][1] == 0.9);
}

/* ---------------- CRC and the blob file ---------------- */

static void test_crc32_is_the_standard_one(void) {
    /* The check value every CRC-32/ISO-HDLC implementation agrees on. A
     * home-rolled polynomial that only agreed with itself would round-trip
     * here and disagree with anything that ever read the file. */
    assert(gz_crc32((const unsigned char *)"123456789", 9) == 0xCBF43926u);
    assert(gz_crc32((const unsigned char *)"", 0) == 0x00000000u);
    unsigned char a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 2, 3, 5 };
    assert(gz_crc32(a, 4) != gz_crc32(b, 4));
}

static const char *tmp_path(const char *name) {
    static char path[256];
    snprintf(path, sizeof path, "/tmp/gz_cal_%d_%s.bin", (int)getpid(), name);
    return path;
}

static void write_raw(const char *path, const unsigned char *b, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (n) assert(fwrite(b, 1, n, f) == n);
    fclose(f);
}

static void put_le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* A well-formed file with whatever header field the caller wants to break. */
static size_t build_blob_file(unsigned char *out, uint32_t magic, uint32_t version,
                              uint32_t declared, const unsigned char *body, size_t n,
                              uint32_t crc) {
    put_le32(out + 0, magic);
    put_le32(out + 4, version);
    put_le32(out + 8, declared);
    put_le32(out + 12, crc);
    if (n) memcpy(out + 16, body, n);
    return 16 + n;
}

static void test_blob_round_trips(void) {
    const char *p = tmp_path("rt");
    unsigned char blob[4096], back[4096];
    for (size_t i = 0; i < sizeof blob; i++) blob[i] = (unsigned char)(i * 31 + 7);

    /* 4096 exactly: out_scratch is [4096]u8, so this is the largest blob that
     * can exist and it must not be an off-by-one. */
    assert(gz_blob_save_to(p, blob, 4096) == 0);
    assert(gz_blob_load_from(p, back, sizeof back) == 4096);
    assert(memcmp(blob, back, 4096) == 0);

    assert(gz_blob_save_to(p, blob, 1) == 0);
    assert(gz_blob_load_from(p, back, sizeof back) == 1);
    assert(back[0] == blob[0]);

    /* No temporary left behind. */
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    assert(access(tmp, F_OK) != 0);
    unlink(p);
}

static void test_a_save_that_cannot_start_leaves_the_old_blob_alone(void) {
    /* The save writes a temporary and renames, so a write that fails partway
     * cannot leave a half-written calibration where a good one used to be.
     * Blocking the temporary with a directory is the portable way to make the
     * open fail after the arguments have already been validated: a save that
     * truncated the real path in place would succeed here and destroy the
     * blob that was already on disk. */
    const char *p = tmp_path("atomic");
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    unlink(p);
    rmdir(tmp);
    unlink(tmp);

    unsigned char first[64], second[64], back[4096];
    memset(first, 0xA1, sizeof first);
    memset(second, 0xB2, sizeof second);
    assert(gz_blob_save_to(p, first, sizeof first) == 0);

    assert(mkdir(tmp, 0755) == 0);
    assert(gz_blob_save_to(p, second, sizeof second) == -1);
    assert(gz_blob_load_from(p, back, sizeof back) == 64);
    assert(back[0] == 0xA1);

    rmdir(tmp);
    unlink(p);
}

static void test_blob_save_refuses_impossible_lengths(void) {
    const char *p = tmp_path("bad_save");
    unlink(p);
    unsigned char blob[8192];
    memset(blob, 0xAB, sizeof blob);

    /* Bounded against 4096, the device's scratch, never against the caller's
     * buffer: an 8192-byte buffer does not make a 5000-byte calibration real. */
    assert(gz_blob_save_to(p, blob, 4097) == -1);
    assert(gz_blob_save_to(p, blob, 8192) == -1);
    assert(gz_blob_save_to(p, blob, 0) == -1);
    assert(access(p, F_OK) != 0);

    /* A refused save must not destroy the calibration already on disk. */
    assert(gz_blob_save_to(p, blob, 64) == 0);
    assert(gz_blob_save_to(p, blob, 4097) == -1);
    unsigned char back[4096];
    assert(gz_blob_load_from(p, back, sizeof back) == 64);
    unlink(p);
}

static void test_blob_load_refuses_everything_it_cannot_account_for(void) {
    const char *p = tmp_path("bad_load");
    unsigned char body[64], file[8192], back[4096];
    for (size_t i = 0; i < sizeof body; i++) body[i] = (unsigned char)i;
    uint32_t good = gz_crc32(body, sizeof body);

    /* The control: this exact builder produces a file that loads. Without it
     * the refusals below would also pass against a loader that refused
     * everything. */
    size_t n = build_blob_file(file, GZ_BLOB_MAGIC, GZ_BLOB_VERSION, 64, body, 64, good);
    write_raw(p, file, n);
    assert(gz_blob_load_from(p, back, sizeof back) == 64);

    n = build_blob_file(file, 0x44414542u, GZ_BLOB_VERSION, 64, body, 64, good);
    write_raw(p, file, n);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);      /* magic */

    n = build_blob_file(file, GZ_BLOB_MAGIC, 99, 64, body, 64, good);
    write_raw(p, file, n);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);      /* version */

    n = build_blob_file(file, GZ_BLOB_MAGIC, GZ_BLOB_VERSION, 0, body, 0, gz_crc32(body, 0));
    write_raw(p, file, n);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);      /* empty */

    n = build_blob_file(file, GZ_BLOB_MAGIC, GZ_BLOB_VERSION, 4097, body, 64, good);
    write_raw(p, file, n);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);      /* past the device max */

    n = build_blob_file(file, GZ_BLOB_MAGIC, GZ_BLOB_VERSION, 64, body, 64, good);
    write_raw(p, file, n);
    assert(gz_blob_load_from(p, back, 32) == -1);               /* past the caller's buffer */

    n = build_blob_file(file, GZ_BLOB_MAGIC, GZ_BLOB_VERSION, 64, body, 64, good);
    write_raw(p, file, n - 8);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);      /* truncated */

    n = build_blob_file(file, GZ_BLOB_MAGIC, GZ_BLOB_VERSION, 64, body, 64, good);
    file[n] = 0xFF;
    write_raw(p, file, n + 1);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);      /* trailing byte */

    /* A single flipped bit. This is the case the CRC exists for: the file is
     * the right length, the right shape, and wrong. Applied to the device it
     * would be a calibration that is wrong rather than absent, and nothing
     * downstream could tell. */
    n = build_blob_file(file, GZ_BLOB_MAGIC, GZ_BLOB_VERSION, 64, body, 64, good);
    file[16 + 30] ^= 0x01;
    write_raw(p, file, n);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);

    write_raw(p, file, 15);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);      /* shorter than a header */

    unlink(p);
    assert(gz_blob_load_from(p, back, sizeof back) == -1);      /* absent */
}

/* ---------------- the eyes-open fraction ---------------- */

static void test_valid_frac_is_over_inspected_frames(void) {
    struct gz_samples s;
    memset(&s, 0, sizeof s);

    /* THE DENOMINATOR. Four frames arriving in one read collapse to one
     * inspection, because the client keeps only the latest, so n_total is
     * larger than n_seen whenever the daemon flushes its ring, which is
     * exactly what it does after parking the USB thread for a command.
     * Dividing by n_total would call this window 50% eyes-open and abort. */
    s.n_seen = 4; s.n_both = 4; s.n_total = 8;
    assert(gz_valid_frac(&s) == 1.0);

    s.n_seen = 4; s.n_both = 2; s.n_total = 4;
    assert(gz_valid_frac(&s) == 0.5);

    /* Too few frames is not evidence of anything, and must not pass a gate on
     * one lucky sample. */
    s.n_seen = 1; s.n_both = 1; s.n_total = 40;
    assert(gz_valid_frac(&s) == 0.0);
    s.n_seen = GZ_CAL_MIN_SEEN - 1; s.n_both = GZ_CAL_MIN_SEEN - 1;
    assert(gz_valid_frac(&s) == 0.0);
    s.n_seen = GZ_CAL_MIN_SEEN; s.n_both = GZ_CAL_MIN_SEEN;
    assert(gz_valid_frac(&s) == 1.0);

    memset(&s, 0, sizeof s);
    assert(gz_valid_frac(&s) == 0.0);
}

/* ---------------- statistics ---------------- */

static void test_stat_of(void) {
    double odd[5] = { 5, 1, 4, 2, 3 };
    assert(gz_stat_of(odd, 5).median == 3);
    double even[4] = { 4, 1, 3, 2 };
    assert(gz_stat_of(even, 4).median == 2.5);
    double one[1] = { 7 };
    struct gz_stat s = gz_stat_of(one, 1);
    assert(s.median == 7 && s.p05 == 7 && s.p95 == 7);
    s = gz_stat_of(one, 0);
    assert(s.median == 0 && s.p05 == 0 && s.p95 == 0);

    /* An outlier must not move the median, which is why the accuracy pass uses
     * one: a single blink-adjacent sample at the far corner would drag a mean
     * across the one-degree threshold. */
    double out[9] = { 1, 1, 1, 1, 1, 1, 1, 1, 1000 };
    assert(gz_stat_of(out, 9).median == 1);
}

/* ---------------- sampling ---------------- */

/* A bare socketpair with no child: the test writes the daemon side itself, so
 * it can choose each frame's validity. */
static int pair_client(struct gz_client *c) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(gz_client_adopt(c, sv[0]) == 0);
    return sv[1];
}

static void feed_one(int d, struct gz_client *c, uint32_t counter, double x, double y,
                     uint32_t vl, uint32_t vr, double z, struct gz_samples *out) {
    unsigned char b[512];
    size_t n = put_gaze(b, counter, x, y, vl, vr, z);
    assert(child_write(d, b, n));
    assert(gz_collect(c, 80, out) == 0);
}

static void test_collect_splits_validity_and_records_the_point(void) {
    struct gz_client c;
    int d = pair_client(&c);
    struct gz_samples s;

    feed_one(d, &c, 100, 0.25, 0.75, GZ_VALIDITY_VALID, GZ_VALIDITY_VALID, 600, &s);
    assert(s.n_total == 1 && s.n_seen == 1 && s.n_any == 1 && s.n_both == 1);
    assert(s.n == 1 && s.x[0] == 0.25 && s.y[0] == 0.75);
    assert(s.nz == 1 && s.z[0] == 600);

    /* validity == 0 means VALID. Inverting it is the easy C bug, and it would
     * turn every blink into a good calibration sample. */
    feed_one(d, &c, 104, 0.35, 0.65, GZ_VALIDITY_VALID, GZ_VALIDITY_NOT_DETECTED, 610, &s);
    assert(s.n_seen == 1 && s.n_any == 1 && s.n_both == 0 && s.n == 1);

    feed_one(d, &c, 108, 0.45, 0.55, GZ_VALIDITY_NOT_DETECTED,
             GZ_VALIDITY_NOT_DETECTED, 620, &s);
    assert(s.n_seen == 1 && s.n_any == 0 && s.n_both == 0 && s.n == 0);

    /* An empty window is not an error, and reports no validity it did not
     * see: the calibration gate divides by n_seen. */
    struct gz_samples empty;
    assert(gz_collect(&c, 30, &empty) == 0);
    assert(empty.n_total == 0 && empty.n_seen == 0 && empty.n_any == 0);

    close(d);
    struct gz_samples dead;
    assert(gz_collect(&c, 100, &dead) == GZ_CLIENT_RECONNECT);
    gz_client_close(&c);
}

static void test_a_burst_is_counted_but_only_the_last_is_inspected(void) {
    /* The client keeps one latest sample, so several frames arriving in one
     * read collapse to one inspection. THIS IS WHY THE VALIDITY FRACTION IS
     * OVER n_seen. Dividing n_both by n_total instead would read this burst as
     * one third eyes-open and abort a calibration that was fine, and the
     * daemon produces exactly this shape when it flushes its ring after
     * parking the USB thread for a command. */
    struct gz_client c;
    int d = pair_client(&c);

    unsigned char b[4096];
    size_t n = 0;
    n += put_gaze(b + n, 200, 0.1, 0.1, GZ_VALIDITY_VALID, GZ_VALIDITY_VALID, 600);
    n += put_gaze(b + n, 204, 0.2, 0.2, GZ_VALIDITY_VALID, GZ_VALIDITY_VALID, 600);
    n += put_gaze(b + n, 208, 0.3, 0.3, GZ_VALIDITY_VALID, GZ_VALIDITY_VALID, 600);
    assert(child_write(d, b, n));

    struct gz_samples s;
    assert(gz_collect(&c, 120, &s) == 0);
    assert(s.n_total == 3);
    assert(s.n_seen >= 1 && s.n_seen <= 3);
    assert(s.n_both == s.n_seen);

    close(d);
    gz_client_close(&c);
}

static void test_collect_ignores_a_repeated_frame_counter_across_polls(void) {
    /* Two frames carrying the same counter, far enough apart that they land in
     * separate polls, which is the only shape where the check can fire: within
     * one poll the client has already collapsed them into its single latest.
     * Counting the repeat would inflate the fraction the calibration gate
     * reads, and the device repeating a counter is not something this project
     * has ruled out. */
    struct gz_client c;
    int d = pair_client(&c);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        signal(SIGPIPE, SIG_IGN);
        unsigned char b[512];
        size_t n = put_gaze(b, 300, 0.4, 0.4, GZ_VALIDITY_VALID, GZ_VALIDITY_VALID, 600);
        child_write(d, b, n);
        struct timespec t = { 0, 60000000L };
        nanosleep(&t, NULL);
        child_write(d, b, n);
        nanosleep(&t, NULL);
        _exit(0);
    }

    struct gz_samples s;
    assert(gz_collect(&c, 250, &s) == 0);
    assert(s.n_total == 2);
    assert(s.n_seen == 1);

    int st = 0;
    assert(waitpid(pid, &st, 0) == pid);
    close(d);
    gz_client_close(&c);
}

static void test_collect_ignores_a_repeated_frame_counter_in_one_read(void) {
    /* Delivered samples step the counter by four. The same counter twice is
     * the same sample seen twice, and counting it would inflate the fraction
     * the calibration gate reads. */
    struct gz_client c;
    int d = pair_client(&c);

    unsigned char b[2048];
    size_t n = 0;
    n += put_gaze(b + n, 300, 0.4, 0.4, GZ_VALIDITY_NOT_DETECTED,
                  GZ_VALIDITY_NOT_DETECTED, 600);
    n += put_gaze(b + n, 300, 0.4, 0.4, GZ_VALIDITY_NOT_DETECTED,
                  GZ_VALIDITY_NOT_DETECTED, 600);
    assert(child_write(d, b, n));

    struct gz_samples s;
    assert(gz_collect(&c, 120, &s) == 0);
    assert(s.n_total == 2 && s.n_seen == 1);

    close(d);
    gz_client_close(&c);
}

/* ---------------- the sequence ---------------- */

/* start_calibration, nine points, finish_calibration: eleven scripted replies,
 * with the blob riding on the finish. */
static size_t script_full(struct step *s, const unsigned char *blob, size_t blob_len) {
    s[0].len = put_response(s[0].buf, GZ_CMD_START_CAL, NULL, 0);
    for (int i = 0; i < GZ_CAL_POINTS; i++)
        s[1 + i].len = put_response(s[1 + i].buf, GZ_CMD_ADD_CAL_POINT, NULL, 0);
    s[10].len = put_response(s[10].buf, GZ_CMD_FINISH_CAL, blob, blob_len);
    return 11;
}

static void test_calibration_sends_the_documented_sequence(void) {
    unsigned char blob[512];
    for (size_t i = 0; i < sizeof blob; i++) blob[i] = (unsigned char)(i ^ 0x5A);

    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    size_t nsteps = script_full(s, blob, sizeof blob);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, nsteps, 2);

    struct stub st;
    memset(&st, 0, sizeof st);
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    struct gz_cal_result r;

    assert(gz_calibrate(&f.c, &ops, &o, &r) == 0);
    assert(fake_stop(&f) == 11);

    /* The blob is what the device returned, not something reconstructed. */
    assert(r.blob_len == sizeof blob);
    assert(memcmp(r.blob, blob, sizeof blob) == 0);
    assert(r.nsteps == 11);
    assert(r.retries == 0);

    struct rec rec[32];
    int nr = rec_read(f.record, rec, 32);
    assert(nr == 11);

    assert(rec[0].cmd == GZ_CMD_START_CAL && rec[0].len == 0);
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        assert(rec[1 + i].cmd == GZ_CMD_ADD_CAL_POINT);
        /* Sixteen bytes, two f64, exactly what main.zig buildRequest memcpys
         * out of the payload. A shorter payload is dropped there with a
         * warning and NO reply at all, so the client would hang. */
        assert(rec[1 + i].len == 16);
        assert(rec_f64(&rec[1 + i], 0) == GZ_CAL_PTS[i][0]);
        assert(rec_f64(&rec[1 + i], 1) == GZ_CAL_PTS[i][1]);
    }
    assert(rec[10].cmd == GZ_CMD_FINISH_CAL && rec[10].len == 0);

    /* The stimulus was shown once per point, at the coordinates that were
     * sent. A dot drawn somewhere other than the point being trained is the
     * one failure that yields a plausible, wrong calibration. */
    assert(st.shown == GZ_CAL_POINTS);
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        assert(st.x[i] == GZ_CAL_PTS[i][0]);
        assert(st.y[i] == GZ_CAL_PTS[i][1]);
    }

    /* Every gap is either a measurement or the wait ceiling, never a runaway:
     * the ceiling is what stops one silent command from stalling a run. */
    for (int i = 0; i < r.nsteps; i++) assert(r.step[i].gap_ms <= 200);
    assert(r.max_gap_ms >= 0 && r.max_gap_ms <= 200);
    unlink(f.record);
}

static void test_no_gaze_means_no_gap_rather_than_a_zero(void) {
    /* A gap of zero would read as "the stream never stalled", which is the
     * opposite of what an unsubscribed or dead stream means. -1 says the
     * measurement was not available. */
    unsigned char blob[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    size_t nsteps = script_full(s, blob, sizeof blob);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, nsteps, 0);   /* no gaze at all */

    struct stub st;
    memset(&st, 0, sizeof st);
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    struct gz_cal_result r;

    assert(gz_calibrate(&f.c, &ops, &o, &r) == 0);
    assert(r.nsteps == 11);
    for (int i = 0; i < r.nsteps; i++) assert(r.step[i].gap_ms < 0);
    assert(r.max_gap_ms == 0);
    fake_stop(&f);
    unlink(f.record);
}

static void test_a_device_error_stops_the_run(void) {
    /* err_code 1 is proto.Err.failed, which means the device saw the command
     * and refused it. Sending the remaining points would build a calibration
     * on top of a start that never happened. */
    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    s[0].len = put_err(s[0].buf, GZ_ERRCODE_FAILED);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);

    struct stub st;
    memset(&st, 0, sizeof st);
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    struct gz_cal_result r;

    assert(gz_calibrate(&f.c, &ops, &o, &r) == GZ_CLIENT_REMOTE);
    assert(fake_stop(&f) == 1);
    assert(st.shown == 0);              /* no dot was ever drawn */
    assert(r.blob_len == 0);

    struct rec rec[8];
    assert(rec_read(f.record, rec, 8) == 1);
    unlink(f.record);
}

static void test_usb_busy_is_retried_and_the_run_continues(void) {
    /* usb_busy means the daemon could not park its USB thread, so nothing
     * reached the device and the connection is still in step. Treating it as
     * fatal would abandon a calibration over a 50 ms condition. */
    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    s[0].len = put_err(s[0].buf, GZ_ERRCODE_USB_BUSY);
    s[1].len = put_response(s[1].buf, GZ_CMD_START_CAL, NULL, 0);
    for (int i = 0; i < GZ_CAL_POINTS; i++)
        s[2 + i].len = put_response(s[2 + i].buf, GZ_CMD_ADD_CAL_POINT, NULL, 0);
    unsigned char blob[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    s[11].len = put_response(s[11].buf, GZ_CMD_FINISH_CAL, blob, sizeof blob);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 12, 1);

    struct stub st;
    memset(&st, 0, sizeof st);
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    struct gz_cal_result r;

    assert(gz_calibrate(&f.c, &ops, &o, &r) == 0);
    assert(fake_stop(&f) == 12);        /* the start went out twice */
    assert(r.blob_len == sizeof blob);

    struct rec rec[32];
    int nr = rec_read(f.record, rec, 32);
    assert(nr == 12);
    assert(rec[0].cmd == GZ_CMD_START_CAL);
    assert(rec[1].cmd == GZ_CMD_START_CAL);
    assert(rec[2].cmd == GZ_CMD_ADD_CAL_POINT);
    unlink(f.record);
}

static void test_a_timeout_stops_rather_than_sending_the_next_command(void) {
    /* THE CONTRACT: after a timeout the two sides are out of step, and an err
     * frame carries no cmd_type, so a late err would be charged to whatever
     * goes out next. Nothing may follow on the same connection. */
    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    s[0].len = 0;                      /* silence, not EOF */

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);

    struct stub st;
    memset(&st, 0, sizeof st);
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    struct gz_cal_result r;

    assert(gz_calibrate(&f.c, &ops, &o, &r) == GZ_CLIENT_TIMEOUT);
    assert(fake_stop(&f) == 1);
    assert(st.shown == 0);

    struct rec rec[8];
    assert(rec_read(f.record, rec, 8) == 1);
    unlink(f.record);
}

/* A real listening socket, so the reconnect after a timeout has somewhere to
 * go. The server answers each connection with a status frame and then keeps
 * quiet, which is what a daemon that has stopped answering looks like. */
struct server { pid_t pid; char path[64]; };

static void server_start(struct server *sv, int connections, uint8_t ver) {
    static int seq;
    snprintf(sv->path, sizeof sv->path, "/tmp/gz_cal_sock_%d_%d", (int)getpid(), seq++);
    unlink(sv->path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sv->path);
    assert(bind(fd, (struct sockaddr *)&a, sizeof a) == 0);
    assert(listen(fd, 4) == 0);

    sv->pid = fork();
    assert(sv->pid >= 0);
    if (sv->pid == 0) {
        signal(SIGPIPE, SIG_IGN);
        int accepted = 0;
        int held[8];
        for (int i = 0; i < connections && i < 8; i++) {
            int c = accept(fd, NULL, NULL);
            if (c < 0) break;
            held[accepted++] = c;
            unsigned char sub[5], st[16];
            if (!child_read(c, sub, 5)) continue;
            child_write(c, st, put_status(st, 1, 0, ver));
            /* Held open, never closed: an EOF would make the client report a
             * lost link, and the case under test is a timeout. */
        }
        for (;;) {
            unsigned char junk[64];
            if (accepted == 0) break;
            if (read(held[accepted - 1], junk, sizeof junk) <= 0) break;
        }
        _exit(accepted);
    }
    close(fd);
}

static int server_stop(struct server *sv) {
    int st = 0;
    assert(waitpid(sv->pid, &st, 0) == sv->pid);
    unlink(sv->path);
    assert(WIFEXITED(st));
    return WEXITSTATUS(st);
}

static void test_a_timeout_reconnects_and_re_gates(void) {
    /* gz_client_reconnect goes through gz_client_init, which memsets the
     * client and clears have_status and version_mismatch. Task 12 shipped a
     * version of this that skipped the re-gate and had to fix it. */
    struct server sv;
    server_start(&sv, 2, GZ_PROTOCOL_VERSION);

    struct gz_client c;
    assert(gz_client_connect(&c, sv.path) == 0);
    assert(gz_display_gate_status(&c, 1000) == 0);
    assert(c.have_status == 1);

    struct stub st;
    memset(&st, 0, sizeof st);
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    o.sock_path = sv.path;
    struct gz_cal_result r;

    assert(gz_calibrate(&c, &ops, &o, &r) == GZ_CLIENT_TIMEOUT);
    /* The re-gate ran on the new connection and found a status there, so the
     * client is usable again even though the calibration is abandoned. */
    assert(c.have_status == 1);
    assert(c.version_mismatch == 0);
    gz_client_close(&c);

    assert(server_stop(&sv) == 2);
}

static void test_finish_bounds_are_against_the_device_scratch(void) {
    unsigned char big[GZ_CAL_BLOB_MAX + 1];
    memset(big, 0x77, sizeof big);

    struct step s[FAKE_MAX_STEPS];
    struct stub st;
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    struct gz_cal_result r;
    struct fake f;

    /* 4097 bytes. A response can carry up to 8192, so this is a frame the wire
     * allows and the device cannot have produced: out_scratch is [4096]u8 and
     * cal_finish_blob_ptr points into it. Truncating to 4096 would hand the
     * device a blob it never computed. */
    memset(s, 0, sizeof s);
    size_t nsteps = script_full(s, big, GZ_CAL_BLOB_MAX + 1);
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, nsteps, 0);
    memset(&st, 0, sizeof st);
    assert(gz_calibrate(&f.c, &ops, &o, &r) == -1);
    assert(r.blob_len == 0);
    fake_stop(&f);
    unlink(f.record);

    /* An empty blob is not a calibration either. finish_calibration reports
     * its failures as an err, so a zero-length success is the daemon saying
     * something impossible. */
    memset(s, 0, sizeof s);
    nsteps = script_full(s, NULL, 0);
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, nsteps, 0);
    memset(&st, 0, sizeof st);
    assert(gz_calibrate(&f.c, &ops, &o, &r) == -1);
    assert(r.blob_len == 0);
    fake_stop(&f);
    unlink(f.record);

    /* Exactly 4096 is legal and must not be an off-by-one. */
    memset(s, 0, sizeof s);
    nsteps = script_full(s, big, GZ_CAL_BLOB_MAX);
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, nsteps, 0);
    memset(&st, 0, sizeof st);
    assert(gz_calibrate(&f.c, &ops, &o, &r) == 0);
    assert(r.blob_len == GZ_CAL_BLOB_MAX);
    fake_stop(&f);
    unlink(f.record);
}

static void test_a_failed_stimulus_stops_before_the_point_is_sent(void) {
    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    size_t nsteps = script_full(s, NULL, 0);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, nsteps, 0);

    struct stub st;
    memset(&st, 0, sizeof st);
    st.fail_at = 3;
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    struct gz_cal_result r;

    assert(gz_calibrate(&f.c, &ops, &o, &r) == -1);
    /* The start plus the two points whose dot really appeared. A point must
     * never be trained against a dot that was not drawn. */
    assert(fake_stop(&f) == 3);
    unlink(f.record);
}

static void test_the_eyes_open_gate_retries_then_aborts(void) {
    /* No gaze at all, so no frame has both eyes. A point added with the eyes
     * shut is a wrong point rather than a missing one: it drags the whole fit
     * instead of the corner it belongs to. */
    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    size_t nsteps = script_full(s, NULL, 0);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, nsteps, 0);

    struct stub st;
    memset(&st, 0, sizeof st);
    struct gz_stim_ops ops = { &st, stub_show };
    struct gz_cal_opts o = fast_opts();
    o.min_valid_frac = 0.66;
    o.point_retries = 2;
    struct gz_cal_result r;

    assert(gz_calibrate(&f.c, &ops, &o, &r) == -1);
    assert(r.retries == 2);
    assert(st.shown == 3);              /* the first point, three times */
    assert(fake_stop(&f) == 1);         /* only the start reached the daemon */
    unlink(f.record);
}

/* ---------------- cal_apply ---------------- */

static void test_apply_refuses_impossible_lengths_without_sending(void) {
    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 0, 0);

    unsigned char blob[GZ_CAL_BLOB_MAX + 1];
    memset(blob, 0x11, sizeof blob);
    assert(gz_apply_blob(&f.c, NULL, blob, 0) == -1);
    assert(gz_apply_blob(&f.c, NULL, blob, GZ_CAL_BLOB_MAX + 1) == -1);
    assert(fake_stop(&f) == 0);
    unlink(f.record);
}

static void test_apply_sends_the_blob_verbatim(void) {
    unsigned char blob[GZ_CAL_BLOB_MAX];
    for (size_t i = 0; i < sizeof blob; i++) blob[i] = (unsigned char)(i * 7 + 3);

    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    s[0].len = put_response(s[0].buf, GZ_CMD_CAL_APPLY, NULL, 0);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
    assert(gz_apply_blob(&f.c, NULL, blob, sizeof blob) == 0);
    assert(fake_stop(&f) == 1);

    struct rec rec[4];
    assert(rec_read(f.record, rec, 4) == 1);
    assert(rec[0].cmd == GZ_CMD_CAL_APPLY);
    /* A full 4096-byte blob is 4101 bytes on the wire and must go out whole.
     * GZ_CLIENT_OUTBUF is sized for exactly this. */
    assert(rec[0].len == GZ_CAL_BLOB_MAX);
    unlink(f.record);
}

static void test_apply_reports_a_rejection(void) {
    /* cal_apply answers a refused blob with proto.Err.failed. Exiting zero
     * there is how an operator ends up trusting a calibration that is not on
     * the device. */
    struct step s[FAKE_MAX_STEPS];
    memset(s, 0, sizeof s);
    s[0].len = put_err(s[0].buf, GZ_ERRCODE_FAILED);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
    unsigned char blob[16];
    memset(blob, 9, sizeof blob);
    assert(gz_apply_blob(&f.c, NULL, blob, sizeof blob) == GZ_CLIENT_REMOTE);
    assert(fake_stop(&f) == 1);
    unlink(f.record);
}

/* ---------------- the host-side correction ---------------- */

/* The measured display area, which is the only geometry a stored fit may
 * claim. Field order is w, h, ox, oy, z, tilt. */
static struct gz_rect corr_area(void) {
    struct gz_rect r = { 590.42, 333.72, -295.21, 5.0, -7.5, 0.0 };
    return r;
}

/* Builds a sweep from the forward model, so the fit has a known answer:
 * reported = E_proj + g*(target - E_proj) + b. `drift` moves the head between
 * points, which is what separates a per-point eye from a per-sweep average. */
static void synth_sweep(struct gz_fit_input *in, double gx, double gy,
                        double bx, double by, double drift_mm) {
    struct gz_rect a = corr_area();
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        in[i].target[0] = GZ_CAL_PTS[i][0];
        in[i].target[1] = GZ_CAL_PTS[i][1];
        in[i].eye_mm[0] = 18.42 + drift_mm * i;
        in[i].eye_mm[1] = 70.61;
        in[i].eye_mm[2] = 598.0;

        double ep[2];
        gz_eye_proj(a, in[i].eye_mm, ep);
        in[i].reported[0] = ep[0] + gx * (in[i].target[0] - ep[0]) + bx;
        in[i].reported[1] = ep[1] + gy * (in[i].target[1] - ep[1]) + by;
    }
}

static void test_fit_recovers_a_synthetic_gain(void) {
    /* Spec 5.2. This is what catches a sign error on the y axis, which is the
     * easiest thing to get wrong because normalised y grows downward while
     * tracker y grows upward: a flipped E_proj still yields a plausible gain
     * and a wrong offset. */
    struct gz_fit_input in[GZ_CAL_POINTS];
    synth_sweep(in, 1.1713, 1.1624, -0.0043, -0.1487, 0.0);

    struct gz_correction c;
    struct gz_fit_report rep;
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_OK);
    assert(c.valid == 1);
    assert(fabs(c.gx - 1.1713) < 1e-9);
    assert(fabs(c.gy - 1.1624) < 1e-9);
    assert(fabs(c.bx - -0.0043) < 1e-9);
    assert(fabs(c.by - -0.1487) < 1e-9);
    assert(rep.n_used == GZ_CAL_POINTS && rep.n_rejected == 0);
    assert(rep.median_resid_mm < 1e-9 && rep.worst_resid_mm < 1e-9);
    assert(fabs(rep.eye_z_mm - 598.0) < 1e-9);
    assert(gz_rect_diff(c.area, corr_area(), 1e-9, 1e-9) == 0);
}

static void test_fit_uses_each_points_own_eye(void) {
    /* Spec do-not #3. The head drifts 5 mm per point here, 40 mm over the
     * sweep. Fitting against each point's own eye recovers the gain exactly;
     * a per-sweep average would not, and the error it leaves is the same
     * 0.63 px per mm that separates the head-aware form from a plain affine. */
    struct gz_fit_input in[GZ_CAL_POINTS];
    synth_sweep(in, 1.1713, 1.1624, -0.0043, -0.1487, 5.0);

    struct gz_correction c;
    struct gz_fit_report rep;
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_OK);
    assert(fabs(c.gx - 1.1713) < 1e-9);
    assert(fabs(c.bx - -0.0043) < 1e-9);
    assert(rep.worst_resid_mm < 1e-9);

    /* And the same data fitted against the mean eye instead does NOT recover
     * it, which is what makes the previous assertion worth making. */
    struct gz_fit_input avg[GZ_CAL_POINTS];
    double mx = 0;
    for (int i = 0; i < GZ_CAL_POINTS; i++) mx += in[i].eye_mm[0];
    mx /= GZ_CAL_POINTS;
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        avg[i] = in[i];
        avg[i].eye_mm[0] = mx;
    }
    struct gz_correction c2;
    struct gz_fit_report rep2;
    assert(gz_correction_fit(avg, GZ_CAL_POINTS, corr_area(), &c2, &rep2) == GZ_FIT_OK);
    assert(rep2.worst_resid_mm > 0.5);
}

static void test_fit_drops_one_outlier_and_refits(void) {
    struct gz_fit_input in[GZ_CAL_POINTS];
    synth_sweep(in, 1.1713, 1.1624, -0.0043, -0.1487, 0.0);
    /* One point where the eye was somewhere else entirely. */
    in[4].reported[0] += 0.15;

    struct gz_correction c;
    struct gz_fit_report rep;
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_OK);
    assert(rep.n_rejected == 1 && rep.rejected[4] == 1);
    assert(rep.n_used == GZ_CAL_POINTS - 1);
    /* The refit lands back on the truth, which it cannot do if the outlier is
     * still carrying weight. */
    assert(fabs(c.gx - 1.1713) < 1e-9);
    assert(fabs(c.gy - 1.1624) < 1e-9);
    /* The rejected point still has a residual recorded, so a human can see how
     * far out it was rather than being told a number vanished. */
    assert(rep.resid_mm[4] > 10.0);
}

static void test_a_near_perfect_sweep_rejects_nothing(void) {
    /* The zero-median guard. Real fixation noise is nowhere near this small,
     * but a sweep whose residuals are all a few nanometres has a median of
     * essentially zero, and 3x essentially zero rejects every point that is not
     * bit-identical to the fit. Without the guard this sweep loses points, or
     * loses more than one and is refused outright. */
    struct gz_fit_input in[GZ_CAL_POINTS];
    synth_sweep(in, 1.1713, 1.1624, -0.0043, -0.1487, 0.0);
    /* One point off by a nanometre of normalised coordinate. That leaves it
     * eight times the median residual, which the 3x rule would reject, while
     * the median itself is 56 picometres. */
    in[4].reported[0] += 1e-9;

    struct gz_correction c;
    struct gz_fit_report rep;
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_OK);
    assert(rep.n_rejected == 0);
    assert(rep.n_used == GZ_CAL_POINTS);
    assert(rep.median_resid_mm > 0.0 && rep.median_resid_mm < 1e-6);
    assert(rep.worst_resid_mm > 3.0 * rep.median_resid_mm);
    assert(fabs(c.gx - 1.1713) < 1e-6);
}

static void test_fit_refuses_two_outliers(void) {
    /* Two is not an outlier, it is a sweep where the eye was elsewhere.
     * Refitting around it would encode that rather than the gain.
     *
     * Both are displaced on the same axis at the same target x, so the slope
     * is untouched and only the intercept moves. That is the shape where the
     * 3x-median rule sees both of them. */
    struct gz_fit_input in[GZ_CAL_POINTS];
    synth_sweep(in, 1.1713, 1.1624, -0.0043, -0.1487, 0.0);
    in[1].reported[0] += 0.20;
    in[7].reported[0] += 0.20;

    struct gz_correction c;
    struct gz_fit_report rep;
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_ERR_OUTLIER);
    assert(c.valid == 0);
    assert(rep.n_rejected == 2);
    assert(rep.rejected[1] == 1 && rep.rejected[7] == 1);
}

static void test_a_second_outlier_on_the_other_axis_still_refuses(void) {
    /* Worth pinning because it does NOT come back as an outlier. The rule the
     * spec gives scores one residual per point over both axes and rejects once,
     * so a y-displacement that the y regression partly absorbs can stay under
     * 3x the median while dragging gy down with it. Here gy lands near 1.095.
     *
     * The ISOTROPY invariant is what catches it: 1.1713 against 1.0951 is 7.0
     * percent apart, past the 5 percent the six recorded sweeps never came
     * close to. Nothing is stored either way, but a reviewer should know the
     * outlier rule alone is not the guard. With nine points and a single
     * rejection round it is a coarse filter, and the measured envelope in
     * gz_correction_check is the load-bearing one. */
    struct gz_fit_input in[GZ_CAL_POINTS];
    synth_sweep(in, 1.1713, 1.1624, -0.0043, -0.1487, 0.0);
    in[2].reported[0] += 0.15;
    in[6].reported[1] -= 0.15;

    struct gz_correction c;
    struct gz_fit_report rep;
    int rc = gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep);
    assert(rc == GZ_FIT_ERR_BOUNDS);
    assert(c.valid == 0);
    assert(c.gy >= GZ_CORR_G_MIN && c.gy <= GZ_CORR_G_MAX);   /* in range, but */
    assert(fabs(c.gx / c.gy - 1.0) > GZ_CORR_ISO_TOL);         /* not isotropic */
    assert(fabs(c.gy - 1.0951) < 1e-3);
}

static void test_fit_refuses_a_gain_outside_the_measured_envelope(void) {
    struct gz_fit_input in[GZ_CAL_POINTS];
    struct gz_correction c;
    struct gz_fit_report rep;

    synth_sweep(in, 1.60, 1.60, 0.0, 0.0, 0.0);
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_ERR_BOUNDS);
    assert(c.valid == 0);
    /* The numbers are still there, so the CLI can name what it refused. */
    assert(fabs(c.gx - 1.60) < 1e-9);

    synth_sweep(in, 1.00, 1.00, 0.0, 0.0, 0.0);
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_ERR_BOUNDS);

    /* Anisotropic past 5 percent: measured isotropy is 0.9991 to 1.0148. */
    synth_sweep(in, 1.25, 1.10, 0.0, 0.0, 0.0);
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_ERR_BOUNDS);

    /* A mirrored axis reads as a negative gain and must not be stored. */
    synth_sweep(in, 1.17, -1.17, 0.0, 0.0, 0.0);
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_ERR_BOUNDS);
}

static void test_fit_refuses_input_it_cannot_fit(void) {
    struct gz_fit_input in[GZ_CAL_POINTS];
    struct gz_correction c;
    struct gz_fit_report rep;
    synth_sweep(in, 1.1713, 1.1624, 0.0, 0.0, 0.0);

    assert(gz_correction_fit(in, 3, corr_area(), &c, &rep) == GZ_FIT_ERR_POINTS);
    assert(gz_correction_fit(in, 0, corr_area(), &c, &rep) == GZ_FIT_ERR_POINTS);
    assert(gz_correction_fit(in, GZ_CAL_POINTS + 1, corr_area(), &c, &rep) == GZ_FIT_ERR_POINTS);

    /* A stimulus that never moved gives no slope to measure. */
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        in[i].target[0] = 0.5;
        in[i].reported[0] = 0.5;
    }
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_ERR_FLAT);

    /* A zero-width area would divide by zero in the projection. */
    struct gz_rect flat = corr_area();
    flat.w_mm = 0;
    synth_sweep(in, 1.1713, 1.1624, 0.0, 0.0, 0.0);
    assert(gz_correction_fit(in, GZ_CAL_POINTS, flat, &c, &rep) == GZ_FIT_ERR_FLAT);
}

static void test_fit_residual_is_the_corrected_error(void) {
    /* Not the regression residual: the two differ by the factor g, and the
     * acceptance test in the spec measures the first. */
    struct gz_fit_input in[GZ_CAL_POINTS];
    synth_sweep(in, 1.1713, 1.1624, -0.0043, -0.1487, 0.0);

    /* Displace the whole middle COLUMN in reported x by d. All three share a
     * target x, so the slope is untouched and the intercept absorbs d/3,
     * leaving a regression residual of exactly 2d/3 on them and d/3 on the
     * other six. Nothing is far enough out to be rejected, so the arithmetic
     * is exact and the only question left is which residual is reported. */
    const double d = 0.03;
    in[1].reported[0] += d;
    in[4].reported[0] += d;
    in[7].reported[0] += d;

    struct gz_correction c;
    struct gz_fit_report rep;
    assert(gz_correction_fit(in, GZ_CAL_POINTS, corr_area(), &c, &rep) == GZ_FIT_OK);
    assert(rep.n_rejected == 0);
    assert(fabs(c.gx - 1.1713) < 1e-9);

    double as_reported = (2.0 * d / 3.0) * 590.42;          /* 11.81 mm */
    double as_corrected = as_reported / 1.1713;             /* 10.08 mm */
    assert(fabs(rep.worst_resid_mm - as_corrected) < 1e-6);
    assert(fabs(rep.worst_resid_mm - as_reported) > 1.0);
    assert(fabs(rep.median_resid_mm - as_corrected / 2.0) < 1e-6);
}

static void test_collect_pairs_the_gaze_with_the_eye_midpoint(void) {
    struct gz_client c;
    int d = pair_client(&c);
    struct gz_eye_state e;
    gz_eye_state_init(&e);
    struct gz_samples s;

    unsigned char b[512];
    size_t n = put_gaze(b, 300, 0.25, 0.75, GZ_VALIDITY_VALID, GZ_VALIDITY_VALID, 600);
    assert(child_write(d, b, n));
    assert(gz_collect_eyes(&c, 80, &s, &e) == 0);
    assert(s.nfit == 1);
    assert(fabs(s.fit_gaze_sum[0] - 0.25) < 1e-12);
    assert(fabs(s.fit_eye_sum[2] - 600.0) < 1e-12);

    /* An eyeless frame contributes nothing, even though present_mask claims
     * the eye-origin fields are there and they read as a clean 0.0. */
    n = put_gaze(b, 304, 0.30, 0.70, GZ_VALIDITY_NOT_DETECTED,
                 GZ_VALIDITY_NOT_DETECTED, 0);
    assert(child_write(d, b, n));
    assert(gz_collect_eyes(&c, 80, &s, &e) == 0);
    assert(s.nfit == 0);
    assert(s.fit_eye_sum[2] == 0.0);

    /* One eye is enough once the state has seen two, and the midpoint it
     * reconstructs is not the lone eye's own position. */
    n = put_gaze(b, 308, 0.35, 0.65, GZ_VALIDITY_VALID, GZ_VALIDITY_NOT_DETECTED, 600);
    assert(child_write(d, b, n));
    assert(gz_collect_eyes(&c, 80, &s, &e) == 0);
    assert(s.nfit == 1);

    close(d);
    gz_client_close(&c);
}

static void test_collect_without_a_state_scopes_it_to_the_window(void) {
    /* gz_collect passes NULL, so a window that opens on a one-eye frame has
     * nothing to reconstruct from and records no pair. That is the documented
     * behaviour, and it is why the sweep passes one state across all nine
     * points instead. */
    struct gz_client c;
    int d = pair_client(&c);
    struct gz_samples s;

    unsigned char b[512];
    size_t n = put_gaze(b, 400, 0.25, 0.75, GZ_VALIDITY_VALID,
                        GZ_VALIDITY_NOT_DETECTED, 600);
    assert(child_write(d, b, n));
    assert(gz_collect(&c, 80, &s) == 0);
    assert(s.n == 1);          /* the gaze point is still recorded */
    assert(s.nfit == 0);       /* but it cannot be paired with an eye */

    close(d);
    gz_client_close(&c);
}

static void test_missing_points_name_proximity_not_the_lights(void) {
    /* The real sweep this exists for: `gaze-cal fit` at 07:01 on 2026-07-27
     * lost points 1, 2 and 3 with the eye at 467 to 500 mm, and the message
     * sent the human to the light switch. These are that run's own numbers,
     * copied from the log. */
    const int paired[GZ_CAL_POINTS] = { 0, 0, 0, 1, 1, 1, 1, 1, 1 };
    const double z[GZ_CAL_POINTS] = {
        0, 0, 0, 499.77, 477.05, 482.73, 489.31, 467.67, 491.26
    };
    double med = 0;
    assert(gz_missing_cause(paired, z, GZ_CAL_POINTS, &med) == GZ_MISS_TOO_CLOSE);
    assert(fabs(med - 486.02) < 0.01);

    /* Same losses at the playing distance is not proximity, so it is the
     * lights, which is what the old message always said. */
    const double far_z[GZ_CAL_POINTS] = { 0, 0, 0, 586, 586, 586, 586, 586, 586 };
    assert(gz_missing_cause(paired, far_z, GZ_CAL_POINTS, &med) == GZ_MISS_LIGHTS);
    assert(fabs(med - 586.0) < 0.01);

    /* Close, but the missing points are not the top row, so the geometry does
     * not explain it on its own. */
    const int scattered[GZ_CAL_POINTS] = { 1, 0, 1, 1, 0, 1, 1, 1, 1 };
    const double close_z[GZ_CAL_POINTS] = { 480, 0, 480, 480, 0, 480, 480, 480, 480 };
    assert(gz_missing_cause(scattered, close_z, GZ_CAL_POINTS, &med) == GZ_MISS_CLOSE);

    /* Nothing paired at all: no distance was measured, so proximity cannot be
     * claimed and the lights are the honest guess. */
    const int none[GZ_CAL_POINTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const double nz[GZ_CAL_POINTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    med = 999;
    assert(gz_missing_cause(none, nz, GZ_CAL_POINTS, &med) == GZ_MISS_LIGHTS);
    assert(med == 0.0);

    /* The boundary is exclusive, and a paired point carrying a zero distance is
     * the eyeless placeholder rather than a head against the screen. */
    const int one_paired[GZ_CAL_POINTS] = { 0, 0, 0, 1, 0, 0, 0, 0, 0 };
    const double at_bound[GZ_CAL_POINTS] = { 0, 0, 0, GZ_FIT_TOO_CLOSE_MM, 0, 0, 0, 0, 0 };
    assert(gz_missing_cause(one_paired, at_bound, GZ_CAL_POINTS, &med) == GZ_MISS_LIGHTS);
    const double zero_z[GZ_CAL_POINTS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    assert(gz_missing_cause(one_paired, zero_z, GZ_CAL_POINTS, &med) == GZ_MISS_LIGHTS);
    assert(med == 0.0);
}

static void test_correction_file_round_trips(void) {
    const char *p = tmp_path("corr");
    unlink(p);

    struct gz_correction c;
    memset(&c, 0, sizeof c);
    c.gx = 1.1713; c.gy = 1.1624; c.bx = -0.0043; c.by = -0.1487;
    c.area = corr_area();
    c.valid = 1;

    struct gz_fit_report rep;
    memset(&rep, 0, sizeof rep);
    rep.n_used = 9;
    rep.median_resid_mm = 8.5;
    rep.worst_resid_mm = 17.25;

    /* 590.42 mm over 2560 px, the gameplay monitor. */
    const double mm_per_px = 590.42 / 2560.0;
    assert(gz_correction_save_to(p, &c, &rep, 598.0, mm_per_px) == 0);

    struct gz_correction back;
    assert(gz_correction_load_from(p, corr_area(), &back) == 1);
    assert(back.valid == 1);
    assert(fabs(back.gx - c.gx) < 1e-9 && fabs(back.by - c.by) < 1e-9);
    assert(gz_rect_diff(back.area, corr_area(), 1e-6, 1e-6) == 0);

    /* The provenance is on disk and does not disturb the parse. */
    FILE *f = fopen(p, "rb");
    assert(f != NULL);
    char text[2048];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    assert(strstr(text, "fit_utc=") != NULL);
    assert(strstr(text, "fit_points=9") != NULL);
    assert(strstr(text, "fit_eye_z_mm=598.0") != NULL);
    /* Pixels, per spec 4.1: 8.5 mm over 0.2306 mm/px is 36.9 px. */
    assert(strstr(text, "fit_median_resid_px=36.9") != NULL);
    assert(strstr(text, "fit_worst_resid_px=74.8") != NULL);
    assert(strstr(text, "resid_mm") == NULL);

    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    assert(access(tmp, F_OK) != 0);
    unlink(p);
}

static void test_correction_load_gates_on_the_display_area(void) {
    /* Spec 4.2, and the reason the area is in the file at all. Calibration and
     * this fit are both computed in the display-area frame, so a correction
     * fitted against one geometry says nothing about another. */
    const char *p = tmp_path("corrgate");
    unlink(p);

    struct gz_correction c;
    memset(&c, 0, sizeof c);
    c.gx = 1.1713; c.gy = 1.1624; c.bx = -0.0043; c.by = -0.1487;
    c.area = corr_area();
    c.valid = 1;
    assert(gz_correction_save_to(p, &c, NULL, 598.0, 590.42 / 2560.0) == 0);

    struct gz_correction back;
    assert(gz_correction_load_from(p, corr_area(), &back) == 1);

    /* Every field of the rectangle is gated, not just the size. Two areas of
     * identical size in different places are different calibration frames. */
    struct gz_rect other = corr_area();
    other.w_mm += 5.0;
    assert(gz_correction_load_from(p, other, &back) == -1);

    other = corr_area();
    other.ox_mm += 10.0;
    assert(gz_correction_load_from(p, other, &back) == -1);

    other = corr_area();
    other.z_mm = 7.5;                 /* the sign the probe settled */
    assert(gz_correction_load_from(p, other, &back) == -1);

    other = corr_area();
    other.tilt_deg = 5.0;
    assert(gz_correction_load_from(p, other, &back) == -1);

    /* Inside the tolerance the readback gate already uses. */
    other = corr_area();
    other.w_mm += 0.5;
    assert(gz_correction_load_from(p, other, &back) == 1);

    unlink(p);
}

static void test_correction_load_separates_absent_from_broken(void) {
    const char *p = tmp_path("corrbad");
    unlink(p);

    struct gz_correction back;
    /* No file is not an error: a first run has no correction and must still
     * measure. */
    assert(gz_correction_load_from(p, corr_area(), &back) == 0);

    FILE *f = fopen(p, "wb");
    assert(f != NULL);
    fputs("this is not a correction\n", f);
    fclose(f);
    assert(gz_correction_load_from(p, corr_area(), &back) == -1);

    /* Well formed, wrong numbers. Refused rather than applied: a wrong
     * correction is worse than none because downstream it is
     * indistinguishable from a calibration. */
    f = fopen(p, "wb");
    assert(f != NULL);
    fputs("version=1\ngx=2.5\ngy=2.5\nbx=0\nby=0\n"
          "area_w_mm=590.42\narea_h_mm=333.72\narea_ox_mm=-295.21\n"
          "area_oy_mm=5\narea_z_mm=-7.5\narea_tilt_deg=0\n", f);
    fclose(f);
    assert(gz_correction_load_from(p, corr_area(), &back) == -1);

    /* A file too large to be this format is refused rather than truncated.
     * The padding here is COMMENTS after a complete and valid correction, so
     * the first buffer's worth parses cleanly on its own: a loader that read
     * only what fits and asked no further questions would accept this and
     * never mention that it had not seen the whole file. */
    f = fopen(p, "wb");
    assert(f != NULL);
    fputs("version=1\ngx=1.1713\ngy=1.1624\nbx=-0.0043\nby=-0.1487\n"
          "area_w_mm=590.42\narea_h_mm=333.72\narea_ox_mm=-295.21\n"
          "area_oy_mm=5\narea_z_mm=-7.5\narea_tilt_deg=0\n", f);
    for (int i = 0; i < 4000; i++) fputs("# padding that a truncation cannot corrupt\n", f);
    fclose(f);
    assert(gz_correction_load_from(p, corr_area(), &back) == -1);

    unlink(p);
}

int main(void) {
    test_points_land_on_the_right_pixels();
    test_points_are_clamped_and_nan_safe();
    test_the_nine_points_are_the_documented_grid();

    test_crc32_is_the_standard_one();
    test_blob_round_trips();
    test_a_save_that_cannot_start_leaves_the_old_blob_alone();
    test_blob_save_refuses_impossible_lengths();
    test_blob_load_refuses_everything_it_cannot_account_for();

    test_valid_frac_is_over_inspected_frames();
    test_stat_of();
    test_collect_splits_validity_and_records_the_point();
    test_a_burst_is_counted_but_only_the_last_is_inspected();
    test_collect_ignores_a_repeated_frame_counter_across_polls();
    test_collect_ignores_a_repeated_frame_counter_in_one_read();

    test_calibration_sends_the_documented_sequence();
    test_no_gaze_means_no_gap_rather_than_a_zero();
    test_a_device_error_stops_the_run();
    test_usb_busy_is_retried_and_the_run_continues();
    test_a_timeout_stops_rather_than_sending_the_next_command();
    test_a_timeout_reconnects_and_re_gates();
    test_finish_bounds_are_against_the_device_scratch();
    test_a_failed_stimulus_stops_before_the_point_is_sent();
    test_the_eyes_open_gate_retries_then_aborts();

    test_apply_refuses_impossible_lengths_without_sending();
    test_apply_sends_the_blob_verbatim();
    test_apply_reports_a_rejection();

    test_fit_recovers_a_synthetic_gain();
    test_fit_uses_each_points_own_eye();
    test_fit_drops_one_outlier_and_refits();
    test_a_near_perfect_sweep_rejects_nothing();
    test_fit_refuses_two_outliers();
    test_a_second_outlier_on_the_other_axis_still_refuses();
    test_fit_refuses_a_gain_outside_the_measured_envelope();
    test_fit_refuses_input_it_cannot_fit();
    test_fit_residual_is_the_corrected_error();
    test_collect_pairs_the_gaze_with_the_eye_midpoint();
    test_collect_without_a_state_scopes_it_to_the_window();
    test_missing_points_name_proximity_not_the_lights();
    test_correction_file_round_trips();
    test_correction_load_gates_on_the_display_area();
    test_correction_load_separates_absent_from_broken();

    printf("test_calibrate: all passed\n");
    return 0;
}
