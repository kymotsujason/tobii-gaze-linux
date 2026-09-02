/* gaze-cal/tests/test_view.c
 *
 * The setup view's pure half: where things go on the screen, when the dwell
 * fires, what the state machine does, what the words say, and the order the
 * sweep sequencing calls its I/O in. None of it needs a display or a tracker.
 *
 * The dwell cases feed at 30 ms, the measured 33.2 Hz frame period, rather
 * than jumping straight to the threshold. GZ_DWELL_GAP_NS is 250 ms, so a
 * test that fed twice 1.5 s apart would be measuring the gap reset and not
 * the dwell at all.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/view.h"
#ifdef NDEBUG
#error "test_view.c relies on assert(); do not build it with NDEBUG"
#endif

#define FRAME_NS 30000000ULL

static const struct gz_screen DP2 = { "DP-2", 4000, 1440, 2560, 1440 };

static void test_layout_is_a_third_tall_and_four_by_three(void) {
    struct gz_view_layout l;
    gz_view_layout(&DP2, &l);
    assert(l.box_h == 480);
    assert(l.box_w == 640);
    assert(l.box_x >= 0 && l.box_y + l.box_h <= 1440);
    assert(l.box_y > 720);                              /* bottom half */
    assert(l.bar_x > l.box_x + l.box_w);                /* beside, to the right */
    assert(l.bar_h == l.box_h && l.bar_y == l.box_y);
    assert(l.readout_y > l.box_y + l.box_h);            /* under the box */
    assert(l.readout_y <= 1440);
    assert(l.target_cx == 1536);                        /* 0.6 * 2560 */
    assert(l.target_cy == 1224);                        /* 0.85 * 1440 */
    assert(l.target_r == GZ_VIEW_TARGET_R_PX);
    assert(l.accept_r == GZ_VIEW_ACCEPT_R_PX);
    assert(l.verdict_y < l.target_cy - l.target_r);     /* above the target */
    assert(l.eye_r > 0 && l.eye_r < l.box_h / 8);
}

static void test_layout_scales_with_the_screen(void) {
    struct gz_screen small = { "X", 0, 0, 1920, 1080 };
    struct gz_view_layout l;
    gz_view_layout(&small, &l);
    assert(l.box_h == 360 && l.box_w == 480);
    assert(l.target_cx == 1152 && l.target_cy == 918);
}

static void test_eye_px_maps_the_box_edges_and_centre(void) {
    struct gz_view_layout l;
    gz_view_layout(&DP2, &l);
    int px, py;
    /* Box x is mirrored and box y is not, so 0 lands on the RIGHT edge and the
     * top, and 1 on the left edge and the bottom. */
    gz_view_eye_px(&l, 0.0, 0.0, &px, &py);
    assert(px == l.box_x + l.box_w && py == l.box_y);
    gz_view_eye_px(&l, 1.0, 1.0, &px, &py);
    assert(px == l.box_x && py == l.box_y + l.box_h);
    gz_view_eye_px(&l, 0.5, 0.5, &px, &py);
    assert(px == l.box_x + l.box_w / 2 && py == l.box_y + l.box_h / 2);
    gz_view_eye_px(&l, 1.3, -0.2, &px, &py);            /* clamped */
    assert(px == l.box_x && py == l.box_y);
    gz_view_eye_px(&l, -0.4, 1.9, &px, &py);            /* the other way */
    assert(px == l.box_x + l.box_w && py == l.box_y + l.box_h);
    gz_view_eye_px(&l, nan(""), nan(""), &px, &py);     /* a lost eye */
    assert(px == l.box_x + l.box_w && py == l.box_y);
}

/* The measurement the mirror comes from, on 2026-09-01: the RIGHT eye sat at
 * tracker x +46 mm and read box x 0.39, while the LEFT eye at -17 mm read 0.54.
 * The device counts box x towards the user's left, so the right eye has to draw
 * to the right of the left one or the dots follow the head backwards. */
static void test_eye_px_mirrors_the_box_x_axis(void) {
    struct gz_view_layout l;
    gz_view_layout(&DP2, &l);
    int rx, ry, lx, ly;
    gz_view_eye_px(&l, 0.39, 0.463, &rx, &ry);
    gz_view_eye_px(&l, 0.54, 0.462, &lx, &ly);
    assert(rx > lx);
    assert(ry < l.box_y + l.box_h / 2 + 4 && ry > l.box_y + l.box_h / 2 - 20);
}

static void test_bar_maps_near_to_the_bottom(void) {
    struct gz_view_layout l;
    gz_view_layout(&DP2, &l);
    assert(gz_view_bar_py(&l, 0.0) == l.bar_y + l.bar_h);
    assert(gz_view_bar_py(&l, 1.0) == l.bar_y);
    assert(gz_view_bar_py(&l, 0.5) == l.bar_y + l.bar_h / 2);
    assert(gz_view_bar_py(&l, 4.0) == l.bar_y);
    assert(gz_view_bar_py(&l, -4.0) == l.bar_y + l.bar_h);
}

static void test_zfit_seed_puts_the_floor_near_a_quarter(void) {
    struct gz_zfit f;
    gz_zfit_init(&f);
    double bz = gz_zfit_box_z(&f, GZ_VIEW_FLOOR_MM);
    assert(bz > 0.20 && bz < 0.27);                     /* 0.2369 from the seed pairs */
    /* Adding the seed pairs again changes nothing. */
    gz_zfit_add(&f, 0.337, 567.0);
    gz_zfit_add(&f, 0.403, 598.0);
    assert(fabs(gz_zfit_box_z(&f, GZ_VIEW_FLOOR_MM) - bz) < 1e-6);
    /* The seeded line runs through both measured pairs. */
    assert(fabs(gz_zfit_box_z(&f, 567.0) - 0.337) < 1e-6);
    assert(fabs(gz_zfit_box_z(&f, 598.0) - 0.403) < 1e-6);
}

static void test_zfit_follows_new_pairs(void) {
    struct gz_zfit f;
    gz_zfit_init(&f);
    /* A thousand samples on a line far from the seed one: box z = (mm - 200)
     * / 800, which puts the floor at 0.40 rather than the seeds' 0.24. Far
     * enough that a fit which quietly ignored them could not land here. */
    for (int i = 0; i < 1000; i++) {
        double mm = 450.0 + (i % 400);
        gz_zfit_add(&f, (mm - 200.0) / 800.0, mm);
    }
    assert(f.n == 1002);
    assert(fabs(gz_zfit_box_z(&f, GZ_VIEW_FLOOR_MM) - 0.40) < 0.01);
    assert(fabs(gz_zfit_box_z(&f, 600.0) - 0.50) < 0.01);
}

static void test_zfit_ignores_junk_samples(void) {
    struct gz_zfit f;
    gz_zfit_init(&f);
    double bz = gz_zfit_box_z(&f, GZ_VIEW_FLOOR_MM);
    gz_zfit_add(&f, 0.0, 0.0);          /* a frame with no eye */
    gz_zfit_add(&f, 0.5, 0.0);
    gz_zfit_add(&f, 0.0, 600.0);
    gz_zfit_add(&f, nan(""), 600.0);
    assert(f.n == 2);
    assert(fabs(gz_zfit_box_z(&f, GZ_VIEW_FLOOR_MM) - bz) < 1e-9);
}

static void test_dwell_fires_once_at_the_threshold(void) {
    struct gz_dwell d;
    gz_dwell_init(&d);
    uint64_t t = 1000000000ULL;
    assert(gz_dwell_feed(&d, 1, t) == 0);
    int fires = 0;
    for (int i = 1; i <= 25; i++) fires += gz_dwell_feed(&d, 1, t + (uint64_t)i * FRAME_NS);
    assert(fires == 0);
    assert(gz_dwell_degrees(&d, t + GZ_DWELL_NS / 2) == 180);   /* 25 frames is 750 ms */
    for (int i = 26; i <= 49; i++) fires += gz_dwell_feed(&d, 1, t + (uint64_t)i * FRAME_NS);
    assert(fires == 0);                                          /* 1470 ms, still short */
    assert(gz_dwell_feed(&d, 1, t + GZ_DWELL_NS - 1) == 0);
    assert(gz_dwell_feed(&d, 1, t + GZ_DWELL_NS) == 1);
    assert(gz_dwell_feed(&d, 1, t + GZ_DWELL_NS + FRAME_NS) == 0);   /* no second fire */
    assert(gz_dwell_degrees(&d, t + 2 * GZ_DWELL_NS) == 360);
}

static void test_dwell_never_fires_while_outside(void) {
    struct gz_dwell d;
    gz_dwell_init(&d);
    uint64_t t = 2000000000ULL;
    assert(gz_dwell_feed(&d, 1, t) == 0);
    for (int i = 1; i <= 60; i++) {                              /* 1.8 s outside */
        assert(gz_dwell_feed(&d, 0, t + (uint64_t)i * FRAME_NS) == 0);
        assert(gz_dwell_degrees(&d, t + (uint64_t)i * FRAME_NS) == 0);
    }
}

static void test_dwell_resets_on_leaving_the_target(void) {
    struct gz_dwell d;
    gz_dwell_init(&d);
    uint64_t t = 5000000000ULL;
    gz_dwell_feed(&d, 1, t);
    gz_dwell_feed(&d, 1, t + FRAME_NS);
    gz_dwell_feed(&d, 0, t + 2 * FRAME_NS);                      /* left */
    assert(gz_dwell_degrees(&d, t + 3 * FRAME_NS) == 0);
    uint64_t back = t + 3 * FRAME_NS;                            /* back in */
    int fires = 0;
    for (int i = 0; i <= 49; i++) fires += gz_dwell_feed(&d, 1, back + (uint64_t)i * FRAME_NS);
    assert(fires == 0);                                          /* the earlier hold is gone */
    assert(gz_dwell_feed(&d, 1, back + GZ_DWELL_NS) == 1);
}

static void test_dwell_resets_on_a_gap_between_feeds(void) {
    struct gz_dwell d;
    gz_dwell_init(&d);
    uint64_t t = 5000000000ULL;
    assert(gz_dwell_feed(&d, 1, t) == 0);
    /* A stall longer than GZ_DWELL_GAP_NS with no feed at all. The dwell may
     * not credit time it never watched, so the next feed starts over. */
    uint64_t after = t + GZ_DWELL_GAP_NS + 1;
    assert(gz_dwell_feed(&d, 1, after) == 0);
    assert(gz_dwell_degrees(&d, after) == 0);
    int fires = 0;
    for (int i = 1; i <= 49; i++) fires += gz_dwell_feed(&d, 1, after + (uint64_t)i * FRAME_NS);
    assert(fires == 0);                             /* 2.0 s since t, 1.47 s since the stall */
    assert(gz_dwell_feed(&d, 1, after + GZ_DWELL_NS) == 1);
}

static void test_state_machine_walks_the_diagram(void) {
    struct gz_view v;
    gz_view_init(&v);
    struct gz_sweep_verdict ok, bad;
    memset(&ok, 0, sizeof ok);
    memset(&bad, 0, sizeof bad);
    bad.refused = 1; bad.rc = 1;

    assert(v.state == GZ_VIEW_IDLE);
    assert(strcmp(gz_view_target_word(&v), "fit") == 0);
    assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_RUN_FIT);
    assert(v.state == GZ_VIEW_FIT_SWEEP && gz_view_in_sweep(&v));
    assert(strcmp(gz_view_target_word(&v), "") == 0);
    assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_NONE);   /* no double request */
    assert(v.state == GZ_VIEW_FIT_SWEEP);
    assert(gz_view_step(&v, GZ_EV_SWEEP_DONE, &bad) == GZ_ACT_NONE);
    assert(v.state == GZ_VIEW_FIT_VERDICT && v.have_verdict && v.verdict.refused);
    assert(!gz_view_in_sweep(&v));
    assert(strcmp(gz_view_target_word(&v), "try again") == 0);
    assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_RUN_FIT);
    assert(gz_view_step(&v, GZ_EV_SWEEP_DONE, &ok) == GZ_ACT_NONE);
    assert(strcmp(gz_view_target_word(&v), "verify") == 0);
    assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_RUN_VERIFY);
    assert(v.state == GZ_VIEW_VERIFY_SWEEP && gz_view_in_sweep(&v));
    assert(gz_view_step(&v, GZ_EV_SWEEP_DONE, &ok) == GZ_ACT_NONE);
    assert(v.state == GZ_VIEW_VERIFY_VERDICT);
    assert(strcmp(gz_view_target_word(&v), "fit again") == 0);
    assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_RUN_FIT);
}

static void test_escape_closes_from_every_state(void) {
    enum gz_view_state states[] = { GZ_VIEW_IDLE, GZ_VIEW_FIT_SWEEP, GZ_VIEW_FIT_VERDICT,
                                    GZ_VIEW_VERIFY_SWEEP, GZ_VIEW_VERIFY_VERDICT };
    for (size_t i = 0; i < sizeof states / sizeof states[0]; i++) {
        struct gz_view v;
        gz_view_init(&v);
        v.state = states[i];
        assert(gz_view_step(&v, GZ_EV_ESCAPE, NULL) == GZ_ACT_CLOSE);
        assert(v.state == GZ_VIEW_CLOSED);
        assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_NONE);
        assert(v.state == GZ_VIEW_CLOSED);
    }
}

static void test_verdict_text_for_each_outcome(void) {
    char buf[512];
    struct gz_sweep_verdict v;
    memset(&v, 0, sizeof v);
    v.kind = GZ_VERDICT_FIT; v.median_px = 38.2; v.worst_px = 71.4;
    v.within_one_degree = 1; v.gx = 1.16947; v.gy = 1.18752;
    snprintf(v.next, sizeof v.next, "now verify, without moving");
    gz_view_verdict_text(&v, buf, sizeof buf);
    assert(strstr(buf, "FIT DONE") != NULL);
    assert(strstr(buf, "median 38 px") != NULL && strstr(buf, "worst 71 px") != NULL);
    assert(strstr(buf, "WITHIN ONE DEGREE") != NULL);
    assert(strstr(buf, "gx 1.1695") != NULL);
    assert(strstr(buf, "gy 1.1875") != NULL);
    assert(strstr(buf, "now verify") != NULL);

    memset(&v, 0, sizeof v);
    v.kind = GZ_VERDICT_FIT; v.refused = 1;
    /* A failed save carries the gains it could not write. They must not be
     * drawn as though the correction were live. */
    v.gx = 1.16947; v.gy = 1.18752;
    snprintf(v.reason, sizeof v.reason, "only 6 of 9 points");
    snprintf(v.next, sizeof v.next, "too close at 480 mm");
    gz_view_verdict_text(&v, buf, sizeof buf);
    assert(strstr(buf, "FIT REFUSED") != NULL);
    assert(strstr(buf, "only 6 of 9") != NULL && strstr(buf, "480 mm") != NULL);
    assert(strstr(buf, "gx") == NULL);

    memset(&v, 0, sizeof v);
    v.kind = GZ_VERDICT_ACCURACY; v.corrected = 1; v.median_px = 33.0; v.worst_px = 108.0;
    v.within_one_degree = 1; v.moved_mm = 7.0;
    snprintf(v.next, sizeof v.next, "under 35 px, better than predicted");
    gz_view_verdict_text(&v, buf, sizeof buf);
    assert(strstr(buf, "VERIFY") != NULL);
    assert(strstr(buf, "corrected median 33 px") != NULL);
    assert(strstr(buf, "head 7 mm from the fitted seat") != NULL);

    memset(&v, 0, sizeof v);
    v.kind = GZ_VERDICT_ACCURACY; v.median_px = 90.0; v.worst_px = 150.0; v.moved_mm = -1;
    snprintf(v.next, sizeof v.next, "above 80 px: the affine model is FALSIFIED");
    gz_view_verdict_text(&v, buf, sizeof buf);
    assert(strstr(buf, "OUTSIDE ONE DEGREE") != NULL);
    assert(strstr(buf, "raw median 90 px") != NULL);
    assert(strstr(buf, "FALSIFIED") != NULL);
    assert(strstr(buf, "fitted seat") == NULL);

    memset(&v, 0, sizeof v);
    v.kind = GZ_VERDICT_ACCURACY; v.refused = 1; v.moved_mm = -1;
    snprintf(v.reason, sizeof v.reason, "no point resolved");
    snprintf(v.next, sizeof v.next, "check the room lights");
    gz_view_verdict_text(&v, buf, sizeof buf);
    assert(strstr(buf, "VERIFY FAILED") != NULL);
    assert(strstr(buf, "no point resolved") != NULL);
    assert(strstr(buf, "check the room lights") != NULL);
    assert(strstr(buf, "median") == NULL);

    /* A tiny buffer never overflows and always terminates. */
    char tiny[8];
    size_t n = gz_view_verdict_text(&v, tiny, sizeof tiny);
    assert(n < sizeof tiny && tiny[n] == '\0');
    assert(gz_view_verdict_text(&v, tiny, 0) == 0);
}

static void test_readout_text(void) {
    char buf[256];
    gz_view_readout_text(575.4, 1, 1, 33.1, "2026-09-01T14:35:06Z", 0, 0, 0, buf, sizeof buf);
    assert(strstr(buf, "575 mm") != NULL);
    assert(strstr(buf, "L ok") != NULL && strstr(buf, "R ok") != NULL);
    assert(strstr(buf, "33 Hz") != NULL);
    assert(strstr(buf, "fit: form S 2026-09-01T14:35:06Z") != NULL);

    gz_view_readout_text(0.0, 0, 1, 33.0, NULL, 0, 0, 0, buf, sizeof buf);
    assert(strstr(buf, "L lost") != NULL && strstr(buf, "R ok") != NULL);
    assert(strstr(buf, "no fit on disk") != NULL);

    gz_view_readout_text(0.0, 0, 0, 33.0, NULL, 1, 1, 0, buf, sizeof buf);
    assert(strstr(buf, "stale file refused") != NULL);
    assert(strstr(buf, "check the room lights") != NULL);
    assert(strstr(buf, "600 mm") != NULL);

    gz_view_readout_text(0.0, 0, 0, 0.0, NULL, 0, 0, 1, buf, sizeof buf);
    assert(strstr(buf, "reconnecting") != NULL);

    /* A stale file is never dressed up as a live fit, whatever the stamp. */
    gz_view_readout_text(575.0, 1, 1, 33.0, "2026-09-01T14:35:06Z", 1, 0, 0, buf, sizeof buf);
    assert(strstr(buf, "stale file refused") != NULL);
    assert(strstr(buf, "2026-09-01") == NULL);

    char tiny[8];
    size_t n = gz_view_readout_text(575.4, 1, 1, 33.1, NULL, 0, 0, 0, tiny, sizeof tiny);
    assert(n < sizeof tiny && tiny[n] == '\0');
    assert(gz_view_readout_text(575.4, 1, 1, 33.1, NULL, 0, 0, 0, tiny, 0) == 0);
}

/* ---------- sequencing, with the I/O faked ---------- */

struct fake_io {
    char order[64];
    int fit_rc, verify_rc, refused;
};
static void f_close(void *c) { strcat(((struct fake_io *)c)->order, "C"); }
static int  f_reconnect(void *c) { strcat(((struct fake_io *)c)->order, "R"); return 0; }
static int  f_fit(void *c, struct gz_sweep_verdict *o) {
    struct fake_io *f = c; strcat(f->order, "F");
    memset(o, 0, sizeof *o); o->kind = GZ_VERDICT_FIT; o->rc = f->fit_rc; o->refused = f->refused;
    return f->fit_rc;
}
static int  f_verify(void *c, struct gz_sweep_verdict *o) {
    struct fake_io *f = c; strcat(f->order, "V");
    memset(o, 0, sizeof *o); o->kind = GZ_VERDICT_ACCURACY; o->rc = f->verify_rc;
    return f->verify_rc;
}
static void f_reload(void *c) { strcat(((struct fake_io *)c)->order, "L"); }

static void test_run_action_closes_before_and_reconnects_after(void) {
    struct fake_io f = { "", 0, 0, 0 };
    struct gz_view_io io = { &f, f_close, f_reconnect, f_fit, f_verify, f_reload };
    struct gz_view v;
    gz_view_init(&v);
    enum gz_view_action a = gz_view_step(&v, GZ_EV_TRIGGER, NULL);
    assert(a == GZ_ACT_RUN_FIT);
    assert(gz_view_run_action(&v, a, &io) == GZ_ACT_NONE);
    assert(strcmp(f.order, "CFLR") == 0);               /* close, fit, reload, reconnect */
    assert(v.state == GZ_VIEW_FIT_VERDICT && v.verdict.kind == GZ_VERDICT_FIT);

    f.order[0] = '\0';
    a = gz_view_step(&v, GZ_EV_TRIGGER, NULL);
    assert(a == GZ_ACT_RUN_VERIFY);
    gz_view_run_action(&v, a, &io);
    assert(strcmp(f.order, "CVR") == 0);                /* no reload after a verify */
    assert(v.state == GZ_VIEW_VERIFY_VERDICT);
    assert(v.verdict.kind == GZ_VERDICT_ACCURACY);
}

static void test_run_action_does_not_reload_after_a_refused_fit(void) {
    struct fake_io f = { "", 1, 0, 1 };
    struct gz_view_io io = { &f, f_close, f_reconnect, f_fit, f_verify, f_reload };
    struct gz_view v;
    gz_view_init(&v);
    gz_view_run_action(&v, gz_view_step(&v, GZ_EV_TRIGGER, NULL), &io);
    assert(strcmp(f.order, "CFR") == 0);
    assert(v.verdict.refused == 1);
    assert(strcmp(gz_view_target_word(&v), "try again") == 0);
}

static void test_run_action_does_not_reload_after_a_failed_fit(void) {
    /* rc set without refused: the sweep never produced a correction, so
     * there is nothing new on disk to reload either. */
    struct fake_io f = { "", 2, 0, 0 };
    struct gz_view_io io = { &f, f_close, f_reconnect, f_fit, f_verify, f_reload };
    struct gz_view v;
    gz_view_init(&v);
    gz_view_run_action(&v, gz_view_step(&v, GZ_EV_TRIGGER, NULL), &io);
    assert(strcmp(f.order, "CFR") == 0);
    assert(v.state == GZ_VIEW_FIT_VERDICT);
}

static void test_run_action_ignores_none_and_close(void) {
    struct fake_io f = { "", 0, 0, 0 };
    struct gz_view_io io = { &f, f_close, f_reconnect, f_fit, f_verify, f_reload };
    struct gz_view v;
    gz_view_init(&v);
    assert(gz_view_run_action(&v, GZ_ACT_NONE, &io) == GZ_ACT_NONE);
    assert(gz_view_run_action(&v, GZ_ACT_CLOSE, &io) == GZ_ACT_CLOSE);
    assert(f.order[0] == '\0');
    assert(v.state == GZ_VIEW_IDLE);
}

static void test_fit_stamp_reads_the_key(void) {
    char path[] = "/tmp/gz_view_stamp_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    assert(fp != NULL);
    fputs("version=2\ngx=1.1\nfit_utc=2026-09-01T14:35:06Z\nfit_points=9\n", fp);
    fclose(fp);
    char buf[64];
    assert(gz_view_fit_stamp(path, buf, sizeof buf) == 1);
    assert(strcmp(buf, "2026-09-01T14:35:06Z") == 0);

    /* A short buffer truncates rather than overflowing. */
    char small[6];
    assert(gz_view_fit_stamp(path, small, sizeof small) == 1);
    assert(strcmp(small, "2026-") == 0);

    unlink(path);
    assert(gz_view_fit_stamp(path, buf, sizeof buf) == 0);
    assert(buf[0] == '\0');
}

static void test_fit_stamp_without_the_key(void) {
    char path[] = "/tmp/gz_view_nostamp_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    assert(fp != NULL);
    /* fit_utc_extra must not match, and a key that only ends in fit_utc= must
     * not either. */
    fputs("version=2\nfit_points=9\nold_fit_utc=1970-01-01T00:00:00Z\n", fp);
    fclose(fp);
    char buf[64];
    assert(gz_view_fit_stamp(path, buf, sizeof buf) == 0);
    assert(buf[0] == '\0');
    unlink(path);
}

int main(void) {
    test_layout_is_a_third_tall_and_four_by_three();
    test_layout_scales_with_the_screen();
    test_eye_px_maps_the_box_edges_and_centre();
    test_eye_px_mirrors_the_box_x_axis();
    test_bar_maps_near_to_the_bottom();
    test_zfit_seed_puts_the_floor_near_a_quarter();
    test_zfit_follows_new_pairs();
    test_zfit_ignores_junk_samples();
    test_dwell_fires_once_at_the_threshold();
    test_dwell_never_fires_while_outside();
    test_dwell_resets_on_leaving_the_target();
    test_dwell_resets_on_a_gap_between_feeds();
    test_state_machine_walks_the_diagram();
    test_escape_closes_from_every_state();
    test_verdict_text_for_each_outcome();
    test_readout_text();
    test_run_action_closes_before_and_reconnects_after();
    test_run_action_does_not_reload_after_a_refused_fit();
    test_run_action_does_not_reload_after_a_failed_fit();
    test_run_action_ignores_none_and_close();
    test_fit_stamp_reads_the_key();
    test_fit_stamp_without_the_key();
    puts("test_view: ok");
    return 0;
}
