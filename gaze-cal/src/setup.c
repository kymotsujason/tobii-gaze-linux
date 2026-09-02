/* gaze-cal/src/setup.c - the setup view's frame loop.
 *
 * The impure half of view.h. Everything here owns a window or a socket, which
 * is why it is a separate file: tests/test_view.c links view.c with no X
 * anywhere, and the pure layout, dwell, state machine and verdict text have to
 * stay reachable from a test binary that never opens a display.
 *
 * The sweeps run hosted. gz_view_run_action closes this view's client, calls
 * the fit or accuracy core (which opens its own connection and prints to the
 * terminal as it always has), then reconnects. Both clients on one socket at
 * once is what the daemon's backpressure timeout punishes, so the order is not
 * optional.
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "client.h"
#include "display.h"
#include "stimulus.h"
#include "view.h"

#define SETUP_POLL_MS   10
/* The device delivers 33.2 Hz, so a redraw budget of 33 ms draws every sample
 * and no more. Without it a 10 ms poll draws up to 100 full-screen frames a
 * second, each carrying several scaled text lines, for nothing new. */
#define SETUP_DRAW_NS   33000000ULL
#define SETUP_RETRY_NS  250000000ULL
#define SETUP_LOG_NS    1000000000ULL

#define GREY   0x888888
#define DIM    0x333333
#define GREEN  0x4caf50
#define RED    0xe53935
#define ORANGE 0xff9800
#define TEXT   0xcccccc

static volatile sig_atomic_t g_setup_stop = 0;
static void on_setup_stop(int sig) { (void)sig; g_setup_stop = 1; }

struct setup_ctx {
    struct gz_stimulus *stim;
    struct gz_client c;
    int connected;
    struct gz_rect panel;
    int have_panel;                 /* gate said OK */
    int gate_mismatch;              /* gate said MISMATCH */
    struct gz_correction corr;
    int have_corr, stale_file;
    char fit_stamp[48];
    const struct gz_setup_opts *o;
    int escaped;                    /* Escape seen during a hosted sweep */
    FILE *log;
};

/* One argument rather than twelve. The loop owns it and the draw only reads
 * it. `sample` is the last one that ARRIVED, not the last poll: a redraw on
 * the 33 ms timer with no new frame must show the same picture rather than
 * blank every dot for one frame. */
struct setup_frame {
    struct gz_gaze_sample sample;
    int have_sample;
    int eye_px[2][2], eye_ok[2];    /* last valid position of each eye */
    struct gz_zfit zf;
    struct gz_dwell dw;
    uint64_t now;
    double hz;
    int no_eyes, reconnecting;
};

/* The sweep's dot, drawn through the back buffer so the box is hidden under
 * it, and Escape checked between dots so a sweep can be abandoned. */
static int setup_stim_show(void *ctx, double nx, double ny) {
    struct setup_ctx *s = ctx;
    if (g_setup_stop) return -1;
    if (gz_stimulus_key(s->stim) == GZ_KEY_ESCAPE) { s->escaped = 1; return -1; }
    const struct gz_screen *scr = gz_stimulus_screen(s->stim);
    int px, py;
    gz_screen_point_px(scr, nx, ny, &px, &py);
    gz_stimulus_clear(s->stim);
    gz_stimulus_disc(s->stim, px - scr->x, py - scr->y, GZ_STIM_DOT_PX / 2, 0xffffff);
    gz_stimulus_disc(s->stim, px - scr->x, py - scr->y, GZ_STIM_INNER_PX / 2, 0x000000);
    gz_stimulus_present(s->stim);
    return 0;
}

static void load_correction(struct setup_ctx *s) {
    s->have_corr = 0;
    s->stale_file = 0;
    s->fit_stamp[0] = '\0';
    if (!s->have_panel) return;
    int r = gz_correction_load(s->panel, &s->corr);
    if (r == 1) {
        s->have_corr = 1;
        char path[512];
        if (gz_correction_path(path, sizeof path) == 0)
            gz_view_fit_stamp(path, s->fit_stamp, sizeof s->fit_stamp);
    } else if (r < 0) {
        s->stale_file = 1;
    }
}

static void io_close(void *ctx) {
    struct setup_ctx *s = ctx;
    if (s->connected) gz_client_close(&s->c);
    s->connected = 0;
}

static int io_reconnect(void *ctx) {
    struct setup_ctx *s = ctx;
    if (gz_client_connect(&s->c, s->o->sock) != 0) return -1;
    s->connected = 1;
    return 0;
}

static int io_run_fit(void *ctx, struct gz_sweep_verdict *out) {
    struct setup_ctx *s = ctx;
    struct gz_stim_ops ops = { s, setup_stim_show };
    return gz_fit_core(s->o->sock, s->o->cfg, &ops, gz_stimulus_screen(s->stim), out);
}

static int io_run_verify(void *ctx, struct gz_sweep_verdict *out) {
    struct setup_ctx *s = ctx;
    struct gz_stim_ops ops = { s, setup_stim_show };
    return gz_accuracy_core(s->o->sock, s->o->cfg, "verify-setup", &ops,
                            gz_stimulus_screen(s->stim), out);
}

static void io_reload(void *ctx) { load_correction(ctx); }

/* The refusal reasons run to 200 characters and the verdict block starts two
 * thirds of the way across the panel, so an unwrapped line runs off the right
 * edge and the half that gets cut is the remedy. Breaks at spaces, and only
 * mid-word when one word is wider than the space. Returns the next baseline. */
static int draw_wrapped(struct gz_stimulus *st, int x, int y, int max_w,
                        const char *text, unsigned long rgb, int lh) {
    char buf[256];
    while (*text != '\0') {
        size_t n = strlen(text);
        if (n > sizeof buf - 1) n = sizeof buf - 1;
        for (;;) {
            memcpy(buf, text, n);
            buf[n] = '\0';
            if (n <= 1 || gz_stimulus_text_width(st, buf) <= max_w) break;
            size_t sp = n;
            while (sp > 1 && buf[sp - 1] != ' ') sp--;
            n = (sp > 1) ? sp - 1 : n - 1;
        }
        gz_stimulus_text(st, x, y, buf, rgb);
        y += lh;
        text += n;
        while (*text == ' ') text++;
    }
    return y;
}

/* Mean over the eyes that count as valid here, so a frame with one eye lost
 * still reports the other eye's distance. Averaging both unconditionally reads
 * 0 mm out of the invalid one and halves the answer. Returns 1 when either eye
 * contributed. */
static int sample_depth(const struct gz_gaze_sample *sm, int lv, int rv,
                        double *box_z, double *z_mm) {
    int n = (lv ? 1 : 0) + (rv ? 1 : 0);
    *box_z = 0;
    *z_mm = 0;
    if (n == 0) return 0;
    if (lv) { *box_z += sm->trackbox_eye_pos_L[2]; *z_mm += fabs(sm->eye_origin_L_mm[2]); }
    if (rv) { *box_z += sm->trackbox_eye_pos_R[2]; *z_mm += fabs(sm->eye_origin_R_mm[2]); }
    *box_z /= n;
    *z_mm /= n;
    return 1;
}

static void draw_frame(struct setup_ctx *s, const struct gz_view_layout *l,
                       const struct gz_view *v, const struct setup_frame *f) {
    struct gz_stimulus *st = s->stim;
    const struct gz_screen *scr = gz_stimulus_screen(st);
    const struct gz_gaze_sample *sm = &f->sample;
    int th = gz_stimulus_text_height(st);

    /* A dropped link freezes the last sample, so without this the eye dots stay
     * green over a dead socket. Spec section 11: they go hollow instead. */
    int live = f->have_sample && !f->reconnecting;
    int lv = live && sm->validity_L == GZ_VALIDITY_VALID;
    int rv = live && sm->validity_R == GZ_VALIDITY_VALID;

    gz_stimulus_clear(st);

    /* the box and its centre lines */
    gz_stimulus_rect(st, l->box_x, l->box_y, l->box_w, l->box_h, GREY, 0);
    gz_stimulus_rect(st, l->box_x + l->box_w / 2, l->box_y, 1, l->box_h, DIM, 1);
    gz_stimulus_rect(st, l->box_x, l->box_y + l->box_h / 2, l->box_w, 1, DIM, 1);

    /* eyes: filled green when valid, a hollow red ring at the last position
     * otherwise, so a lost eye still says where it was lost */
    for (int e = 0; e < 2; e++) {
        int valid = (e == 0) ? lv : rv;
        if (valid) {
            const double *tb = (e == 0) ? sm->trackbox_eye_pos_L : sm->trackbox_eye_pos_R;
            int px, py;
            gz_view_eye_px(l, tb[0], tb[1], &px, &py);
            gz_stimulus_disc(st, px, py, l->eye_r, GREEN);
        } else if (f->eye_ok[e]) {
            gz_stimulus_ring(st, f->eye_px[e][0], f->eye_px[e][1], l->eye_r, 3, RED, 360);
        }
    }

    /* the distance bar, the 520 mm floor and the head marker */
    gz_stimulus_rect(st, l->bar_x, l->bar_y, l->bar_w, l->bar_h, GREY, 0);
    int floor_py = gz_view_bar_py(l, gz_zfit_box_z(&f->zf, GZ_VIEW_FLOOR_MM));
    gz_stimulus_rect(st, l->bar_x - 6, floor_py, l->bar_w + 12, 3, RED, 1);
    gz_stimulus_text(st, l->bar_x + l->bar_w + 12, floor_py + th / 3, "520", RED);
    double bz = 0, z_mm = 0;
    if (sample_depth(sm, lv, rv, &bz, &z_mm))
        gz_stimulus_rect(st, l->bar_x + 1, gz_view_bar_py(l, bz) - 3,
                         l->bar_w - 1, 6, GREEN, 1);

    /* readout */
    char line[256];
    gz_view_readout_text(z_mm, lv, rv, f->hz, s->have_corr ? s->fit_stamp : NULL,
                         s->stale_file, f->no_eyes, f->reconnecting, line, sizeof line);
    gz_stimulus_text(st, l->readout_x, l->readout_y, line, TEXT);

    /* Along the top, not above the readout: the readout sits one line under the
     * box and there is nothing between them. */
    if (s->gate_mismatch)
        gz_stimulus_text(st, l->readout_x, th + 20,
                         "display area mismatch: the sweeps will refuse. Fix it with"
                         " tobiifreed --force-display-area", RED);

    /* gaze: the device's own point in orange, the corrected one in green */
    if (live && gz_sample_any_eye_valid(sm)) {
        int px, py;
        gz_screen_point_px(scr, sm->gaze_point_2d_norm[0], sm->gaze_point_2d_norm[1],
                           &px, &py);
        gz_stimulus_ring(st, px - scr->x, py - scr->y, GZ_STIM_DOT_PX / 2 + 4, 3,
                         ORANGE, 360);
        double cor[2];
        if (s->have_corr && gz_gaze_correct(&s->corr, sm, cor)) {
            gz_screen_point_px(scr, cor[0], cor[1], &px, &py);
            gz_stimulus_ring(st, px - scr->x, py - scr->y, GZ_STIM_DOT_PX / 2 + 4, 3,
                             GREEN, 360);
        }
    }

    /* the target and its dwell ring */
    const char *word = gz_view_target_word(v);
    if (word[0] != '\0') {
        gz_stimulus_disc(st, l->target_cx, l->target_cy, l->target_r, 0x2a2a2a);
        gz_stimulus_ring(st, l->target_cx, l->target_cy, l->target_r, 2, GREY, 360);
        gz_stimulus_ring(st, l->target_cx, l->target_cy, l->target_r - 8, 8, GREEN,
                         gz_dwell_degrees(&f->dw, f->now));
        int tw = gz_stimulus_text_width(st, word);
        gz_stimulus_text(st, l->target_cx - tw / 2, l->target_cy + th / 3, word, TEXT);

        /* Beside the target, not under it. Under it the hint and the readout
         * share the bottom 60 px of the panel and overlap: at 39 px text the
         * readout reaches x 1830 and a hint at the target's left edge starts
         * at 1386. */
        static const char *hint[2] = { "look here 1.5 s, or press Enter", "Escape closes" };
        for (int i = 0; i < 2; i++) {
            int hx = l->target_cx + l->target_r + 24;
            int hw = gz_stimulus_text_width(st, hint[i]);
            if (hx + hw > scr->w) hx = (scr->w > hw) ? scr->w - hw : 0;
            gz_stimulus_text(st, hx, l->target_cy + i * (th + 8), hint[i], GREY);
        }
    }

    /* the verdict block */
    if (v->have_verdict) {
        char text[512];
        gz_view_verdict_text(&v->verdict, text, sizeof text);
        int y = l->verdict_y;
        int lh = th + 6;
        int max_w = scr->w - l->verdict_x - 24;
        for (char *p = text; p != NULL && *p != '\0'; ) {
            char *nl = strchr(p, '\n');
            if (nl != NULL) *nl = '\0';
            y = draw_wrapped(st, l->verdict_x, y, max_w, p,
                             v->verdict.refused ? RED : TEXT, lh);
            p = (nl != NULL) ? nl + 1 : NULL;
        }
    }

    gz_stimulus_present(st);
}

/* The one screen that is not the view: the daemon or the config did not come
 * up, so there is nothing to draw and one key closes it. */
static int fail_screen(struct setup_ctx *s, const struct gz_view_layout *l,
                       const char *what, int rc) {
    const struct gz_screen *scr = gz_stimulus_screen(s->stim);
    int th = gz_stimulus_text_height(s->stim);
    gz_stimulus_clear(s->stim);
    gz_stimulus_text(s->stim, l->readout_x, scr->h / 2, what, RED);
    gz_stimulus_text(s->stim, l->readout_x, scr->h / 2 + th + 12,
                     "the terminal says why. Press any key to close", TEXT);
    while (!g_setup_stop && gz_stimulus_key(s->stim) == GZ_KEY_NONE) {
        /* Re-presented rather than presented once: this window is
         * override_redirect and nothing here handles Expose. */
        gz_stimulus_present(s->stim);
        struct timespec t = { 0, 50 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    gz_client_close(&s->c);
    gz_stimulus_close(s->stim);
    return rc;
}

int gz_cmd_setup(const struct gz_setup_opts *o) {
    struct setup_ctx s;
    memset(&s, 0, sizeof s);
    s.o = o;
    gz_client_init(&s.c);

    s.stim = gz_stimulus_open_input(o->output);
    if (s.stim == NULL) return 3;
    const struct gz_screen *scr = gz_stimulus_screen(s.stim);
    struct gz_view_layout l;
    gz_view_layout(scr, &l);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_setup_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* The gate returns GZ_GATE_UNKNOWN for three different causes and this is
     * the only place that can tell them apart, so the config is resolved here
     * first. The gate below reads it again and stays the authority on the
     * geometry; this read only decides which message and which exit code. */
    char cfgbuf[512];
    const char *cfg = o->cfg;
    if (cfg == NULL && gz_config_path(cfgbuf, sizeof cfgbuf) == 0) cfg = cfgbuf;
    struct gz_rect probe;
    if (cfg == NULL || gz_config_load_rect(cfg, &probe) != 0) {
        fprintf(stderr, "setup: cannot read the display area from %s\n",
                cfg != NULL ? cfg : "$HOME/" GZ_CONFIG_RELPATH);
        return fail_screen(&s, &l, "the daemon's config could not be read", 2);
    }

    /* Gate once, the way the sweeps do, so the correction is loaded against the
     * geometry the device really holds. A mismatch still opens the view: the
     * box needs no geometry, and the sweeps refuse on their own. */
    int g = gz_connect_and_gate(&s.c, o->sock, o->cfg, 1, &s.panel);
    if (g != GZ_GATE_OK && g != GZ_GATE_MISMATCH) {
        /* fd < 0 means the connect never happened, which is the stopped
         * daemon. A connection that came up and then could not produce a
         * display area is the geometry failure, and they get different exit
         * codes because only one of them is fixed by starting a service. */
        if (s.c.fd < 0)
            return fail_screen(&s, &l,
                               "the gaze daemon is not answering."
                               " Start it with: systemctl --user start tobiifreed", 1);
        return fail_screen(&s, &l, "the display area could not be read off the device", 3);
    }
    s.connected = 1;
    s.have_panel = (g == GZ_GATE_OK);
    s.gate_mismatch = (g == GZ_GATE_MISMATCH);
    load_correction(&s);

    char logpath[600];
    if (gz_log_path(logpath, sizeof logpath) == 0) s.log = fopen(logpath, "a");
    if (s.log != NULL) {
        time_t t = time(NULL);
        struct tm tmv;
        char when[64] = "unknown time";
        if (localtime_r(&t, &tmv) != NULL)
            strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
        fprintf(s.log, "\n===== setup view opened  %s =====\n", when);
        fflush(s.log);
        fprintf(stderr, "log: %s\n", logpath);
    }

    struct gz_view v;
    gz_view_init(&v);
    struct gz_view_io io = { &s, io_close, io_reconnect, io_run_fit, io_run_verify,
                             io_reload };

    struct setup_frame f;
    memset(&f, 0, sizeof f);
    gz_zfit_init(&f.zf);
    gz_dwell_init(&f.dw);

    uint64_t last_any_eye = gz_now_ns();
    uint64_t last_log = 0, last_draw = 0, last_retry = 0;
    uint64_t rate_t0 = gz_now_ns();
    unsigned rate_n = 0;

    while (!g_setup_stop && v.state != GZ_VIEW_CLOSED) {
        uint64_t now = gz_now_ns();
        f.now = now;

        /* Keys first, so Escape works while the link is down. */
        int key = gz_stimulus_key(s.stim);
        enum gz_view_event ev = (key == GZ_KEY_ESCAPE) ? GZ_EV_ESCAPE
                              : (key == GZ_KEY_ENTER)  ? GZ_EV_TRIGGER
                                                       : GZ_EV_NONE;

        int fresh = 0;
        if (s.connected) {
            int r = gz_client_poll(&s.c, SETUP_POLL_MS);
            /* Only RECONNECT closes the link. GZ_LINK_STALE means the daemon
             * has told us the tracker is gone, and reconnecting to a healthy
             * daemon cannot bring an unplugged device back: it only churns. */
            if (r == GZ_CLIENT_RECONNECT ||
                gz_client_watchdog(&s.c, now) == GZ_CLIENT_RECONNECT) {
                io_close(&s);
                f.reconnecting = 1;
                last_retry = now;
            } else {
                if (r > 0) rate_n += (unsigned)r;
                if (s.c.have_latest) {
                    f.sample = s.c.latest;
                    s.c.have_latest = 0;
                    f.have_sample = 1;
                    fresh = 1;
                }
            }
        } else {
            struct timespec t = { 0, SETUP_POLL_MS * 1000 * 1000 };
            nanosleep(&t, NULL);
            /* Throttled: a connect against a stopped daemon fails instantly,
             * so an untimed retry is a 100 Hz spin on the socket. */
            if (now - last_retry >= SETUP_RETRY_NS) {
                last_retry = now;
                if (io_reconnect(&s) == 0) f.reconnecting = 0;
            }
        }

        if (now - rate_t0 >= 1000000000ULL) {
            f.hz = rate_n * 1e9 / (double)(now - rate_t0);
            rate_n = 0;
            rate_t0 = now;
        }

        if (fresh) {
            for (int e = 0; e < 2; e++) {
                uint32_t val = (e == 0) ? f.sample.validity_L : f.sample.validity_R;
                if (val != GZ_VALIDITY_VALID) continue;
                const double *tb = (e == 0) ? f.sample.trackbox_eye_pos_L
                                            : f.sample.trackbox_eye_pos_R;
                gz_view_eye_px(&l, tb[0], tb[1], &f.eye_px[e][0], &f.eye_px[e][1]);
                f.eye_ok[e] = 1;
                const double *eye = (e == 0) ? f.sample.eye_origin_L_mm
                                             : f.sample.eye_origin_R_mm;
                gz_zfit_add(&f.zf, tb[2], fabs(eye[2]));
            }
            if (gz_sample_any_eye_valid(&f.sample)) last_any_eye = now;
        }
        f.no_eyes = (now - last_any_eye >= GZ_VIEW_NO_EYES_NS);

        /* dwell on the target, on the corrected point when there is one */
        int inside = 0;
        double cor[2];
        int have_cor = f.have_sample && s.have_corr &&
                       gz_gaze_correct(&s.corr, &f.sample, cor);
        if (fresh && gz_sample_any_eye_valid(&f.sample) && gz_view_target_word(&v)[0]) {
            double gx = have_cor ? cor[0] : f.sample.gaze_point_2d_norm[0];
            double gy = have_cor ? cor[1] : f.sample.gaze_point_2d_norm[1];
            int px, py;
            gz_screen_point_px(scr, gx, gy, &px, &py);
            inside = hypot(px - scr->x - l.target_cx, py - scr->y - l.target_cy)
                     <= l.accept_r;
        }
        if (fresh && gz_dwell_feed(&f.dw, inside, now) && ev == GZ_EV_NONE)
            ev = GZ_EV_TRIGGER;

        if (s.log != NULL && fresh && now - last_log >= SETUP_LOG_NS) {
            last_log = now;
            double lbz = 0, lz = 0;
            sample_depth(&f.sample, f.sample.validity_L == GZ_VALIDITY_VALID,
                         f.sample.validity_R == GZ_VALIDITY_VALID, &lbz, &lz);
            fprintf(s.log,
                    "setup z=%.0f tbL=(%.3f,%.3f,%.3f) tbR=(%.3f,%.3f,%.3f)"
                    " vL=%u vR=%u raw=(%.4f,%.4f) cor=",
                    lz,
                    f.sample.trackbox_eye_pos_L[0], f.sample.trackbox_eye_pos_L[1],
                    f.sample.trackbox_eye_pos_L[2],
                    f.sample.trackbox_eye_pos_R[0], f.sample.trackbox_eye_pos_R[1],
                    f.sample.trackbox_eye_pos_R[2],
                    f.sample.validity_L, f.sample.validity_R,
                    f.sample.gaze_point_2d_norm[0], f.sample.gaze_point_2d_norm[1]);
            if (have_cor) fprintf(s.log, "(%.4f,%.4f)\n", cor[0], cor[1]);
            else          fprintf(s.log, "none\n");
            fflush(s.log);
        }

        enum gz_view_action act = gz_view_step(&v, ev, NULL);
        if (act == GZ_ACT_RUN_FIT || act == GZ_ACT_RUN_VERIFY) {
            s.escaped = 0;
            gz_view_run_action(&v, act, &io);
            gz_dwell_init(&f.dw);
            f.reconnecting = !s.connected;
            last_retry = gz_now_ns();
            /* The rate readout would otherwise report the whole sweep as a
             * second of silence. */
            rate_n = 0;
            rate_t0 = gz_now_ns();
            last_any_eye = gz_now_ns();
            if (s.escaped) gz_view_step(&v, GZ_EV_ESCAPE, NULL);
            continue;
        }

        if (fresh || now - last_draw >= SETUP_DRAW_NS) {
            last_draw = now;
            draw_frame(&s, &l, &v, &f);
        }
    }

    if (s.log != NULL) {
        fprintf(s.log, "===== setup view closed =====\n");
        fclose(s.log);
    }
    io_close(&s);
    gz_stimulus_close(s.stim);     /* releases the keyboard grab */
    return 0;
}
