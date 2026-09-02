/* gaze-cal/src/view.c - the setup view. See view.h.
 *
 * The top of this file is pure: layout, dwell, state machine, words. The
 * frame loop at the bottom is the only part that touches a socket or a
 * window, and it is the only part the tests do not reach. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "view.h"

void gz_view_layout(const struct gz_screen *scr, struct gz_view_layout *out) {
    memset(out, 0, sizeof *out);
    int margin = scr->h / 30;                     /* 48 px on 1440 */
    out->box_h = scr->h / 3;
    out->box_w = out->box_h * 4 / 3;
    out->box_x = margin;
    out->readout_y = scr->h - margin;
    out->readout_x = margin;
    out->box_y = out->readout_y - margin - out->box_h;
    out->bar_w = margin / 2;
    out->bar_x = out->box_x + out->box_w + margin / 2;
    out->bar_y = out->box_y;
    out->bar_h = out->box_h;
    out->target_cx = (int)lround(GZ_VIEW_TARGET_NX * scr->w);
    out->target_cy = (int)lround(GZ_VIEW_TARGET_NY * scr->h);
    out->target_r = GZ_VIEW_TARGET_R_PX;
    out->accept_r = GZ_VIEW_ACCEPT_R_PX;
    out->verdict_x = out->target_cx - out->target_r;
    out->verdict_y = out->target_cy - out->target_r - 4 * margin;
    out->eye_r = scr->h / 120;                    /* 12 px on 1440 */
}

/* The two comparisons are written negated so a nan lands at 0 rather than
 * falling through both and reaching lround with a value it cannot represent.
 * A lost eye is exactly where that arrives. */
void gz_view_eye_px(const struct gz_view_layout *l, double box_x, double box_y,
                    int *px, int *py) {
    if (!(box_x >= 0.0)) box_x = 0.0;
    if (!(box_y >= 0.0)) box_y = 0.0;
    if (box_x > 1.0) box_x = 1.0;
    if (box_y > 1.0) box_y = 1.0;
    *px = l->box_x + (int)lround(box_x * l->box_w);
    *py = l->box_y + (int)lround(box_y * l->box_h);
}

int gz_view_bar_py(const struct gz_view_layout *l, double box_z) {
    if (!(box_z >= 0.0)) box_z = 0.0;
    if (box_z > 1.0) box_z = 1.0;
    return l->bar_y + l->bar_h - (int)lround(box_z * l->bar_h);
}

/* Seed pairs from the 2026-09-01 measurement, in the spec. */
void gz_zfit_init(struct gz_zfit *f) {
    memset(f, 0, sizeof *f);
    gz_zfit_add(f, 0.337, 567.0);
    gz_zfit_add(f, 0.403, 598.0);
}

/* A frame with no eye reports a plain 0.0 in both, so a pair with a zero in it
 * is thrown away rather than dragged into the fit. Negated comparisons again,
 * for the nan. */
void gz_zfit_add(struct gz_zfit *f, double box_z, double z_mm) {
    if (!(box_z > 0.0) || !(z_mm > 0.0)) return;
    f->sx += box_z; f->sy += z_mm; f->sxx += box_z * box_z; f->sxy += box_z * z_mm;
    f->n++;
}

double gz_zfit_box_z(const struct gz_zfit *f, double z_mm) {
    double n = (double)f->n;
    double den = n * f->sxx - f->sx * f->sx;
    if (f->n < 2 || fabs(den) < 1e-12) return 0.25;
    double slope = (n * f->sxy - f->sx * f->sy) / den;      /* mm per unit of box z */
    double icept = (f->sy - slope * f->sx) / n;
    if (fabs(slope) < 1e-9) return 0.25;
    return (z_mm - icept) / slope;
}

void gz_dwell_init(struct gz_dwell *d) { memset(d, 0, sizeof *d); }

int gz_dwell_feed(struct gz_dwell *d, int inside, uint64_t now_ns) {
    int gap = d->last_ns != 0 && now_ns > d->last_ns && now_ns - d->last_ns > GZ_DWELL_GAP_NS;
    d->last_ns = now_ns;
    if (!inside || gap) {
        d->since_ns = 0;
        d->fired = 0;
        if (!inside) return 0;
    }
    if (d->since_ns == 0) d->since_ns = now_ns;
    if (!d->fired && now_ns - d->since_ns >= GZ_DWELL_NS) {
        d->fired = 1;
        return 1;
    }
    return 0;
}

int gz_dwell_degrees(const struct gz_dwell *d, uint64_t now_ns) {
    if (d->since_ns == 0 || now_ns < d->since_ns) return 0;
    uint64_t held = now_ns - d->since_ns;
    if (held >= GZ_DWELL_NS) return 360;
    return (int)(held * 360 / GZ_DWELL_NS);
}

void gz_view_init(struct gz_view *v) { memset(v, 0, sizeof *v); v->state = GZ_VIEW_IDLE; }

int gz_view_in_sweep(const struct gz_view *v) {
    return v->state == GZ_VIEW_FIT_SWEEP || v->state == GZ_VIEW_VERIFY_SWEEP;
}

enum gz_view_action gz_view_step(struct gz_view *v, enum gz_view_event ev,
                                 const struct gz_sweep_verdict *result) {
    if (v->state == GZ_VIEW_CLOSED) return GZ_ACT_NONE;
    if (ev == GZ_EV_ESCAPE) { v->state = GZ_VIEW_CLOSED; return GZ_ACT_CLOSE; }
    switch (v->state) {
    case GZ_VIEW_IDLE:
        if (ev == GZ_EV_TRIGGER) { v->state = GZ_VIEW_FIT_SWEEP; return GZ_ACT_RUN_FIT; }
        return GZ_ACT_NONE;
    case GZ_VIEW_FIT_SWEEP:
        if (ev == GZ_EV_SWEEP_DONE && result != NULL) {
            v->verdict = *result; v->have_verdict = 1; v->state = GZ_VIEW_FIT_VERDICT;
        }
        return GZ_ACT_NONE;
    case GZ_VIEW_FIT_VERDICT:
        if (ev == GZ_EV_TRIGGER) {
            if (v->verdict.refused) { v->state = GZ_VIEW_FIT_SWEEP; return GZ_ACT_RUN_FIT; }
            v->state = GZ_VIEW_VERIFY_SWEEP; return GZ_ACT_RUN_VERIFY;
        }
        return GZ_ACT_NONE;
    case GZ_VIEW_VERIFY_SWEEP:
        if (ev == GZ_EV_SWEEP_DONE && result != NULL) {
            v->verdict = *result; v->have_verdict = 1; v->state = GZ_VIEW_VERIFY_VERDICT;
        }
        return GZ_ACT_NONE;
    case GZ_VIEW_VERIFY_VERDICT:
        if (ev == GZ_EV_TRIGGER) { v->state = GZ_VIEW_FIT_SWEEP; return GZ_ACT_RUN_FIT; }
        return GZ_ACT_NONE;
    default:
        return GZ_ACT_NONE;
    }
}

const char *gz_view_target_word(const struct gz_view *v) {
    switch (v->state) {
    case GZ_VIEW_IDLE:           return "fit";
    case GZ_VIEW_FIT_VERDICT:    return v->verdict.refused ? "try again" : "verify";
    case GZ_VIEW_VERIFY_VERDICT: return "fit again";
    default:                     return "";
    }
}

enum gz_view_action gz_view_run_action(struct gz_view *v, enum gz_view_action act,
                                       const struct gz_view_io *io) {
    if (act != GZ_ACT_RUN_FIT && act != GZ_ACT_RUN_VERIFY) return act;
    struct gz_sweep_verdict result;
    memset(&result, 0, sizeof result);
    io->close_client(io->ctx);
    if (act == GZ_ACT_RUN_FIT) {
        io->run_fit(io->ctx, &result);
        if (!result.refused && result.rc == 0) io->reload_correction(io->ctx);
    } else {
        io->run_verify(io->ctx, &result);
    }
    io->reconnect_client(io->ctx);
    return gz_view_step(v, GZ_EV_SWEEP_DONE, &result);
}

size_t gz_view_verdict_text(const struct gz_sweep_verdict *v, char *buf, size_t cap) {
    if (cap == 0) return 0;
    int n;
    if (v->kind == GZ_VERDICT_FIT && v->refused) {
        n = snprintf(buf, cap, "FIT REFUSED: %s\n%s", v->reason, v->next);
    } else if (v->kind == GZ_VERDICT_FIT) {
        n = snprintf(buf, cap, "FIT DONE: median %.0f px, worst %.0f px, %s\ngx %.4f gy %.4f\n%s",
                     v->median_px, v->worst_px,
                     v->within_one_degree ? "WITHIN ONE DEGREE" : "OUTSIDE ONE DEGREE",
                     v->gx, v->gy, v->next);
    } else if (v->refused) {
        n = snprintf(buf, cap, "VERIFY FAILED: %s\n%s", v->reason, v->next);
    } else if (v->moved_mm >= 0.0) {
        n = snprintf(buf, cap, "VERIFY: %smedian %.0f px, worst %.0f px, %s\nhead %.0f mm from the fitted seat\n%s",
                     v->corrected ? "corrected " : "raw ", v->median_px, v->worst_px,
                     v->within_one_degree ? "WITHIN ONE DEGREE" : "OUTSIDE ONE DEGREE",
                     v->moved_mm, v->next);
    } else {
        n = snprintf(buf, cap, "VERIFY: %smedian %.0f px, worst %.0f px, %s\n%s",
                     v->corrected ? "corrected " : "raw ", v->median_px, v->worst_px,
                     v->within_one_degree ? "WITHIN ONE DEGREE" : "OUTSIDE ONE DEGREE",
                     v->next);
    }
    if (n < 0) { buf[0] = '\0'; return 0; }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t gz_view_readout_text(double z_mm, int l_valid, int r_valid, double hz,
                            const char *fit_stamp, int stale_file, int no_eyes,
                            int reconnecting, char *buf, size_t cap) {
    if (cap == 0) return 0;
    int n;
    if (reconnecting) {
        n = snprintf(buf, cap, "reconnecting to the daemon");
    } else if (no_eyes) {
        n = snprintf(buf, cap, "no eyes seen: check the room lights and sit about 600 mm back   %s",
                     stale_file ? "no fit on disk (stale file refused)"
                   : fit_stamp ? "fit: form S" : "no fit on disk");
    } else {
        n = snprintf(buf, cap, "%.0f mm   L %s  R %s   %.0f Hz   %s%s",
                     z_mm, l_valid ? "ok" : "lost", r_valid ? "ok" : "lost", hz,
                     stale_file ? "no fit on disk (stale file refused)"
                   : fit_stamp ? "fit: form S " : "no fit on disk",
                     (fit_stamp && !stale_file) ? fit_stamp : "");
    }
    if (n < 0) { buf[0] = '\0'; return 0; }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

int gz_view_fit_stamp(const char *path, char *buf, size_t cap) {
    if (cap == 0) return 0;
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "fit_utc=", 8) != 0) continue;
        size_t n = strcspn(line + 8, "\r\n");
        if (n >= cap) n = cap - 1;
        memcpy(buf, line + 8, n);
        buf[n] = '\0';
        found = 1;
        break;
    }
    fclose(f);
    return found;
}
