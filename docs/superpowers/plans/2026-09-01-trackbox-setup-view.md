# Track box setup view implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `gaze-cal setup` command that shows the device's track box, both eyes, the distance and the raw and corrected gaze on the gameplay monitor, and runs the fit and verify sweeps from that screen with a short verdict drawn in the window.

**Architecture:** The fit and accuracy commands in `calibrate.c` are split into cores that fill a `struct gz_sweep_verdict` and wrappers that keep today's output. `stimulus.c` gains a back buffer, drawing primitives, key polling and a keyboard grab, and stays the only file that includes Xlib. A new `view.c` holds the layout, the dwell detector, the state machine and the sweep sequencing as pure functions over small structs, plus one frame loop that touches the client and the stimulus.

**Tech Stack:** C11, Xlib and XRandR (already linked), the existing `gz_client_*` socket client, `make -C gaze-cal check` (test, ASan and UBSan, C++ include test, mutation harness).

Spec: `docs/superpowers/specs/2026-09-01-trackbox-setup-view-design.md`. Read it before any task.

## Global Constraints

- Output only ASCII characters in code, comments, commit messages and reports.
- Validity 0 means VALID (`GZ_VALIDITY_VALID` is `0u` in `proto.h:77`). Never gate on `present_mask`, which is `0x003fffff` in every frame.
- The device delivers 33.2 Hz. The frame loop polls with a 10 ms timeout and draws one sample per frame.
- The track box fields are `trackbox_eye_pos_L[3]` and `trackbox_eye_pos_R[3]` in `struct gz_gaze_sample`, 0 to 1 with 0.5 on the centre line, exactly zero when that eye is invalid. Measured 2026-09-01, in the spec.
- Layout A: box in the bottom left, one third of the screen tall and 4:3 wide, distance bar beside it, readout under it, target at normalised (0.6, 0.85) with a 150 px drawn radius and a 250 px acceptance radius, dwell of 1.5 s.
- The 520 mm floor is drawn from a running linear fit of eye z in mm against box z, seeded with the measured pairs (box z 0.337 at 567 mm, 0.403 at 598 mm).
- The view closes its client before a sweep and reconnects after, because the daemon evicts a client that stops reading for `BACKPRESSURE_TIMEOUT_MS` (10 s, `patches/0008-daemon-routing-fairness-and-backpressure.patch`).
- Nothing about what the fit computes, the refusals, or the correction file changes. `gz_cmd_fit` and `gz_cmd_accuracy` print exactly what they print today.
- The window is `override_redirect`, so key input needs `XGrabKeyboard`, and the grab must be released on every exit path.
- Xlib stays confined to `stimulus.c`. `view.c` and every test build without X.
- `make -C gaze-cal check` must exit 0 with 0 unexpected mutation survivors after every task. The baseline at `75f433e` is `killed=201 documented_survivors=7 unexpected_survivors=0`, and the killed count must only rise.
- Code quoted in this plan is a draft of the intent. Seven earlier tasks found defects in plan-quoted code, each caught by testing. Implement the intent, test the behaviour, and deviate loudly with the measurement that justifies it.
- Don't run `./scripts/build.sh` and don't touch `vendor/`. `gaze-cal/` is self-contained.
- The daemon `tobiifreed.service` may be running. A task may use it read-only (`status`, `monitor`, a `setup` smoke with no sweep). Never run `fit`, `calibrate`, `accuracy` or a sweep from `setup`; those need the human.

---

## File structure

| File | Responsibility |
|---|---|
| `gaze-cal/src/calibrate.h` | Adds `struct gz_sweep_verdict`, `struct gz_corr_stats`, the fill functions, the cores, `gz_fit_err_text` |
| `gaze-cal/src/calibrate.c` | Cores fill the struct, wrappers unchanged in output |
| `gaze-cal/src/stimulus.h` | Adds the input open, key polling, back buffer primitives |
| `gaze-cal/src/stimulus.c` | Implements them, Xlib only here |
| `gaze-cal/src/view.h` | Layout, z fit, dwell, state machine, sequencing, verdict text, `gz_cmd_setup` |
| `gaze-cal/src/view.c` | Pure half first, then the frame loop at the bottom |
| `gaze-cal/src/main.c` | The `setup` command and its usage lines |
| `gaze-cal/tests/test_calibrate.c` | Tests for the fill functions and the corrected stats |
| `gaze-cal/tests/test_view.c` | Tests for the pure half of `view.c` |
| `gaze-cal/tests/mutate.sh` | Mutations for the new pure code |
| `gaze-cal/Makefile` | `test_view` in `test` and `test-asan`, `record.c` style |
| `scripts/fit-correction.sh` | Header names `gaze-cal setup` as the screen version |

---

### Task 1: Sweep verdict struct and the fit and accuracy cores

**Files:**
- Modify: `gaze-cal/src/calibrate.h` (after `struct gz_fit_report`, around line 270, and the CLI entries block around line 344), Modify: `gaze-cal/src/calibrate.c` (`report_corrected` at 1321, `gz_cmd_accuracy` at 1394, `say_missing_points` at 1498, `fit_err_text` at 1528, `gz_cmd_fit` at 1539), Test: `gaze-cal/tests/test_calibrate.c`

**Interfaces:**
- Consumes: `struct gz_fit_report`, `struct gz_correction`, `struct gz_sweep`, `gz_stat_of`, `gz_correct_point`, `gz_eye_proj`, `gz_missing_cause`, `GZ_ACC_TARGET_PX` (45.0, currently at `calibrate.c:1187`).
- Produces, all in `calibrate.h`:

```c
#define GZ_VERDICT_FIT      1
#define GZ_VERDICT_ACCURACY 2

/* What one sweep decided, in the numbers the setup view draws. The cores fill
 * it and keep printing everything they print today; this is the same result
 * in a form a window can show without a terminal. */
struct gz_sweep_verdict {
    int    kind;                /* GZ_VERDICT_FIT or GZ_VERDICT_ACCURACY */
    int    rc;                  /* the command's exit code, 0 on success */
    int    refused;             /* 1 when nothing usable came out of the sweep */
    int    n_used, n_rejected;
    double median_px, worst_px; /* corrected when a correction applied, else raw */
    int    within_one_degree;   /* median_px <= GZ_ACC_TARGET_PX */
    int    corrected;           /* accuracy: 1 when the figures are corrected ones */
    double gx, gy, bx, by;      /* fit only, zero otherwise */
    double moved_mm;            /* accuracy: head distance from the fitted seat, -1 unknown */
    char   reason[200];         /* plain words on refusal, "" otherwise */
    char   next[120];           /* what to do next, always set */
};

/* Corrected error statistics over one sweep, the numbers report_corrected
 * prints. Returns the number of points that carried both a gaze sample and an
 * eye position, 0 when none did (out is zeroed then). */
struct gz_corr_stats {
    unsigned n;
    double   median_px, worst_px, raw_median_px;
    double   moved_mm;          /* -1 when the fit recorded no seat */
};
unsigned gz_corrected_stats(const struct gz_sweep *sw, const struct gz_screen *scr,
                            const struct gz_correction *corr, struct gz_corr_stats *out);

/* Pure fills, so the words in the window come from the same place as the
 * words on the terminal. */
void gz_fit_verdict_fill(int fit_rc, const struct gz_fit_report *rep,
                         const struct gz_correction *corr, double mm_per_px,
                         struct gz_sweep_verdict *out);
void gz_missing_verdict_fill(const struct gz_sweep *sw, struct gz_sweep_verdict *out);
void gz_accuracy_verdict_fill(int have_corr, const struct gz_corr_stats *cs,
                              double raw_median_px, double raw_worst_px,
                              struct gz_sweep_verdict *out);
const char *gz_fit_err_text(int rc);

/* The cores. Same behaviour and same printed output as gz_cmd_fit and
 * gz_cmd_accuracy, which now call these, plus the filled verdict. `out` is
 * always written, including on a gate failure (rc set, refused 1). */
int gz_fit_core(const char *sock, const char *cfg, const struct gz_stim_ops *stim,
                const struct gz_screen *scr, struct gz_sweep_verdict *out);
int gz_accuracy_core(const char *sock, const char *cfg, const char *label,
                     const struct gz_stim_ops *stim, const struct gz_screen *scr,
                     struct gz_sweep_verdict *out);
```

- [ ] **Step 1: Write the failing tests for the fill functions**

Append to `gaze-cal/tests/test_calibrate.c`, before `main`:

```c
/* ---------- sweep verdicts, the numbers the setup view draws ---------- */

static void test_fit_verdict_carries_the_fit(void) {
    struct gz_fit_report rep;
    memset(&rep, 0, sizeof rep);
    rep.n_in = 9; rep.n_used = 8; rep.n_rejected = 1; rep.refit_outlier = -1;
    rep.median_resid_mm = 8.76;   /* 38 px at 0.2306 mm/px */
    rep.worst_resid_mm = 16.37;   /* 71 px */
    struct gz_correction corr;
    memset(&corr, 0, sizeof corr);
    corr.gx = 1.16947; corr.gy = 1.18752; corr.bx = -0.103394; corr.by = -0.247857;
    corr.valid = 1;

    struct gz_sweep_verdict v;
    gz_fit_verdict_fill(GZ_FIT_OK, &rep, &corr, 0.2306, &v);
    assert(v.kind == GZ_VERDICT_FIT);
    assert(v.rc == 0);
    assert(v.refused == 0);
    assert(v.n_used == 8 && v.n_rejected == 1);
    assert(fabs(v.median_px - 38.0) < 0.6);
    assert(fabs(v.worst_px - 71.0) < 0.6);
    assert(v.within_one_degree == 1);
    assert(fabs(v.gx - 1.16947) < 1e-9 && fabs(v.by + 0.247857) < 1e-9);
    assert(v.reason[0] == '\0');
    assert(strstr(v.next, "verify") != NULL);
}

static void test_fit_verdict_names_the_refit_point(void) {
    struct gz_fit_report rep;
    memset(&rep, 0, sizeof rep);
    rep.n_in = 9; rep.n_used = 8; rep.n_rejected = 1; rep.refit_outlier = 6;
    rep.median_resid_mm = 5.0; rep.worst_resid_mm = 40.0;
    struct gz_correction corr;
    memset(&corr, 0, sizeof corr);

    struct gz_sweep_verdict v;
    gz_fit_verdict_fill(GZ_FIT_ERR_REFIT, &rep, &corr, 0.2306, &v);
    assert(v.rc == 1);
    assert(v.refused == 1);
    assert(strstr(v.reason, "point 7") != NULL);
    assert(strstr(v.next, "re-run") != NULL || strstr(v.next, "Re-run") != NULL);
    assert(v.gx == 0.0);
}

static void test_fit_verdict_reason_matches_the_terminal_text(void) {
    struct gz_fit_report rep;
    memset(&rep, 0, sizeof rep);
    rep.refit_outlier = -1;
    struct gz_correction corr;
    memset(&corr, 0, sizeof corr);
    struct gz_sweep_verdict v;
    gz_fit_verdict_fill(GZ_FIT_ERR_BOUNDS, &rep, &corr, 0.2306, &v);
    assert(strstr(v.reason, gz_fit_err_text(GZ_FIT_ERR_BOUNDS)) != NULL);
    assert(v.refused == 1 && v.rc == 1);
}

static void test_missing_verdict_blames_proximity_when_the_top_row_is_gone(void) {
    struct gz_sweep sw;
    memset(&sw, 0, sizeof sw);
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        sw.pt[i].target[0] = GZ_CAL_PTS[i][0];
        sw.pt[i].target[1] = GZ_CAL_PTS[i][1];
        if (i >= 3) { sw.pt[i].n_fit = 20; sw.pt[i].eye_mm[2] = 480.0; }
    }
    sw.n_fit_ok = 6;
    struct gz_sweep_verdict v;
    gz_missing_verdict_fill(&sw, &v);
    assert(v.kind == GZ_VERDICT_FIT);
    assert(v.refused == 1 && v.rc == 1);
    assert(strstr(v.reason, "6 of 9") != NULL);
    assert(strstr(v.reason, "1, 2, 3") != NULL);
    assert(strstr(v.next, "480 mm") != NULL);
    assert(strstr(v.next, "600 mm") != NULL);
}

static void test_missing_verdict_blames_the_lights_otherwise(void) {
    struct gz_sweep sw;
    memset(&sw, 0, sizeof sw);
    for (int i = 0; i < GZ_CAL_POINTS; i++) {
        sw.pt[i].target[0] = GZ_CAL_PTS[i][0];
        sw.pt[i].target[1] = GZ_CAL_PTS[i][1];
        if (i != 0) { sw.pt[i].n_fit = 20; sw.pt[i].eye_mm[2] = 600.0; }
    }
    sw.n_fit_ok = 8;
    struct gz_sweep_verdict v;
    gz_missing_verdict_fill(&sw, &v);
    assert(strstr(v.next, "lights") != NULL);
}

static void test_corrected_stats_match_the_hand_computed_case(void) {
    /* One point, so the median is the point. Target (0.5, 0.5), raw gaze
     * reported = g*true + b, so the correction lands exactly on target and
     * the corrected error is 0 while the raw error is not. */
    struct gz_correction corr;
    memset(&corr, 0, sizeof corr);
    corr.gx = 1.2; corr.gy = 1.1; corr.bx = 0.01; corr.by = -0.02;
    corr.area.w_mm = 590.42; corr.area.h_mm = 333.72;
    corr.area.ox_mm = -295.21; corr.area.oy_mm = 5.0;
    corr.form = GZ_CORR_FORM_STATIC; corr.valid = 1;
    corr.eye_proj[0] = 0.5; corr.eye_proj[1] = 0.98;

    struct gz_screen scr = { "DP-2", 4000, 1440, 2560, 1440 };
    struct gz_sweep sw;
    memset(&sw, 0, sizeof sw);
    sw.pt[4].target[0] = 0.5; sw.pt[4].target[1] = 0.5;
    sw.pt[4].gaze_mean[0] = 1.2 * 0.5 + 0.01;
    sw.pt[4].gaze_mean[1] = 1.1 * 0.5 - 0.02;
    sw.pt[4].n_fit = 20;
    /* Eye straight in front of the panel centre, 600 mm out, so eye_proj
     * comes back near (0.5, 0.98) and moved_mm is small. */
    sw.pt[4].eye_mm[0] = 0.0; sw.pt[4].eye_mm[1] = 0.0; sw.pt[4].eye_mm[2] = 600.0;

    struct gz_corr_stats cs;
    unsigned n = gz_corrected_stats(&sw, &scr, &corr, &cs);
    assert(n == 1 && cs.n == 1);
    assert(cs.median_px < 0.5 && cs.worst_px < 0.5);
    double raw_px = hypot((sw.pt[4].gaze_mean[0] - 0.5) * 2560,
                          (sw.pt[4].gaze_mean[1] - 0.5) * 1440);
    assert(fabs(cs.raw_median_px - raw_px) < 1e-6);
    assert(cs.moved_mm >= 0.0);
}

static void test_corrected_stats_with_no_pairs_is_zero(void) {
    struct gz_correction corr;
    memset(&corr, 0, sizeof corr);
    corr.gx = 1.0; corr.gy = 1.0; corr.valid = 1;
    struct gz_screen scr = { "DP-2", 0, 0, 2560, 1440 };
    struct gz_sweep sw;
    memset(&sw, 0, sizeof sw);
    struct gz_corr_stats cs;
    assert(gz_corrected_stats(&sw, &scr, &corr, &cs) == 0);
    assert(cs.n == 0 && cs.median_px == 0.0);
}

static void test_accuracy_verdict_bands(void) {
    struct gz_corr_stats cs = { 9, 40.0, 100.0, 260.0, 7.0 };
    struct gz_sweep_verdict v;
    gz_accuracy_verdict_fill(1, &cs, 260.0, 380.0, &v);
    assert(v.kind == GZ_VERDICT_ACCURACY);
    assert(v.corrected == 1 && v.refused == 0 && v.rc == 0);
    assert(fabs(v.median_px - 40.0) < 1e-9 && fabs(v.worst_px - 100.0) < 1e-9);
    assert(v.within_one_degree == 1);
    assert(fabs(v.moved_mm - 7.0) < 1e-9);
    assert(strstr(v.next, "as predicted") != NULL);

    cs.median_px = 65.0; cs.moved_mm = 40.0;
    gz_accuracy_verdict_fill(1, &cs, 260.0, 380.0, &v);
    assert(v.within_one_degree == 0);
    assert(strstr(v.next, "re-fit") != NULL || strstr(v.next, "Re-fit") != NULL);

    cs.median_px = 90.0;
    gz_accuracy_verdict_fill(1, &cs, 260.0, 380.0, &v);
    assert(strstr(v.next, "FALSIFIED") != NULL);

    /* No correction on disk: the raw numbers are the verdict, and next says fit. */
    gz_accuracy_verdict_fill(0, NULL, 260.0, 380.0, &v);
    assert(v.corrected == 0);
    assert(fabs(v.median_px - 260.0) < 1e-9);
    assert(v.within_one_degree == 0);
    assert(strstr(v.next, "fit") != NULL);
}
```

Add the calls at the end of `main`, before `return 0`:

```c
    test_fit_verdict_carries_the_fit();
    test_fit_verdict_names_the_refit_point();
    test_fit_verdict_reason_matches_the_terminal_text();
    test_missing_verdict_blames_proximity_when_the_top_row_is_gone();
    test_missing_verdict_blames_the_lights_otherwise();
    test_corrected_stats_match_the_hand_computed_case();
    test_corrected_stats_with_no_pairs_is_zero();
    test_accuracy_verdict_bands();
```

Check `struct gz_screen`'s initialiser order against `calibrate.h:34` (`name`, `x`, `y`, `w`, `h`) and `struct gz_rect`'s field names in `proto.h` before relying on the positional initialisers above.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C gaze-cal test 2>&1 | tail -5`
Expected: compile error, `gz_fit_verdict_fill` undeclared.

- [ ] **Step 3: Add the declarations to calibrate.h**

Insert the block from Interfaces after `struct gz_fit_report` (around line 270) and the two core prototypes beside `gz_cmd_fit` and `gz_cmd_accuracy` (around line 344). Move `#define GZ_ACC_TARGET_PX 45.0` from `calibrate.c:1187` into `calibrate.h` next to `GZ_CAL_POINTS`, with its comment, since the fill needs it and the view's tests will too.

- [ ] **Step 4: Implement the fills and the stats in calibrate.c**

`gz_fit_err_text` is `fit_err_text` made public (rename, keep the body). Then, near `report_corrected`:

```c
unsigned gz_corrected_stats(const struct gz_sweep *sw, const struct gz_screen *scr,
                            const struct gz_correction *corr, struct gz_corr_stats *out) {
    memset(out, 0, sizeof *out);
    out->moved_mm = -1;

    double raws[GZ_CAL_POINTS], cors[GZ_CAL_POINTS];
    double eye_sum[2] = { 0, 0 };
    unsigned n = 0;
    double worst = 0;
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
```

`gz_stat_of` sorts its input in place (check `calibrate.c:310`), which is why the per-point loop above finishes before either call and why `worst` is taken in the loop.

Rewrite `report_corrected` to call `gz_corrected_stats` and print from it. The per-point lines it prints today need the per-point raw and corrected errors, so keep the loop for printing and take the summary numbers from the struct. The printed text must not change: diff the output of `make -C gaze-cal test` before and after for the `test_calibrate` lines that print a corrected report, and keep the `acceptance:` wording exactly.

```c
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
        if (!paired[i] && at < sizeof list - 4) {
            at += (size_t)snprintf(list + at, sizeof list - at, "%s%d", at ? ", " : "", i + 1);
        }
    }
    snprintf(out->reason, sizeof out->reason,
             "only %d of %d points carried a gaze sample and an eye position (missing %s)",
             sw->n_fit_ok, GZ_CAL_POINTS, list);

    double med = 0;
    int cause = gz_missing_cause(paired, z, GZ_CAL_POINTS, &med);
    if (cause == GZ_MISS_TOO_CLOSE) {
        snprintf(out->next, sizeof out->next,
                 "too close at %.0f mm: the top row is outside the tracker's view. Sit at about 600 mm", med);
    } else if (cause == GZ_MISS_CLOSE) {
        snprintf(out->next, sizeof out->next,
                 "at %.0f mm, closer than the 600 mm playing position. Move back, then the lights", med);
    } else {
        snprintf(out->next, sizeof out->next,
                 "turn the room lights on: with them off this tracker loses the top left point");
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
    if (cs->median_px > 80.0) {
        snprintf(out->next, sizeof out->next, "above 80 px: the affine model is FALSIFIED");
    } else if (cs->median_px > 50.0) {
        snprintf(out->next, sizeof out->next, cs->moved_mm > 15.0
                 ? "50 to 80 px and the head has left the fitted seat: re-fit where you sit"
                 : "50 to 80 px with the head at the fitted seat, so movement does not explain it");
    } else if (cs->median_px >= 35.0) {
        snprintf(out->next, sizeof out->next, "35 to 50 px, as predicted");
    } else {
        snprintf(out->next, sizeof out->next, "under 35 px, better than predicted");
    }
}
```

The band thresholds 80, 50 and 35 are the ones `report_corrected` prints today (`calibrate.c:1381-1391`). Use one set of constants for both so they can't drift.

- [ ] **Step 5: Split the commands into cores**

Rename the body of `gz_cmd_fit` to `gz_fit_core` with the extra `out` parameter, and add fills at each exit:

- gate failure: `memset(out, 0, sizeof *out); out->kind = GZ_VERDICT_FIT; out->rc = g; out->refused = 1; snprintf(out->reason, ..., "the device does not hold the configured display area"); snprintf(out->next, ..., "fix with tobiifreed --force-display-area");` for `GZ_GATE_MISMATCH`, and "the daemon didn't answer" for `GZ_GATE_UNKNOWN`.
- `run_sweep` failure: `rc 1`, reason "the sweep was interrupted", next "re-run the sweep".
- partial sweep: `gz_missing_verdict_fill(&sw, out)`.
- after `gz_correction_fit`: `gz_fit_verdict_fill(rc, &rep, &corr, mm_per_px, out)`, then the existing refusal or save path. A failed save sets `out->rc = 1`, `out->refused = 1`, reason "couldn't write the correction file".

Then:

```c
int gz_cmd_fit(const char *sock, const char *cfg,
               const struct gz_stim_ops *stim, const struct gz_screen *scr) {
    struct gz_sweep_verdict v;
    return gz_fit_core(sock, cfg, stim, scr, &v);
}
```

Do the same for `gz_cmd_accuracy` and `gz_accuracy_core`: the `NO VALID GAZE` exit fills refused with reason "no valid gaze at any point" and next "look at each dot, with the lights on"; the normal exit computes `gz_corrected_stats` when `have_corr` (reusing the struct that `report_corrected` now prints from) and calls `gz_accuracy_verdict_fill(have_corr, &cs, es.median, worst, out)`.

- [ ] **Step 6: Run the tests and the whole check**

Run: `make -C gaze-cal test 2>&1 | tail -3` then `make -C gaze-cal check 2>&1 | tail -2`
Expected: `test_calibrate` passes with the eight new cases, `killed=` at least 201, `unexpected_survivors=0`, exit 0.

- [ ] **Step 7: Add mutations for the fills**

Append to `gaze-cal/tests/mutate.sh` in the second `tests/test_calibrate.c` section (after line 855's `TEST_SRCS`), before the record section:

```bash
run_mutation "fit verdict does not mark a refusal" calibrate.c \
    "        out->refused = 1;
        if (fit_rc == GZ_FIT_ERR_REFIT && rep->refit_outlier >= 0) {" \
    "        out->refused = 0;
        if (fit_rc == GZ_FIT_ERR_REFIT && rep->refit_outlier >= 0) {"

run_mutation "fit verdict names the wrong refit point" calibrate.c \
    "gz_fit_err_text(fit_rc), rep->refit_outlier + 1);" \
    "gz_fit_err_text(fit_rc), rep->refit_outlier);"

run_mutation "missing verdict never blames proximity" calibrate.c \
    "    if (cause == GZ_MISS_TOO_CLOSE) {" \
    "    if (0) {"

run_mutation "accuracy verdict judges one degree against the wrong number" calibrate.c \
    "    out->within_one_degree = cs->median_px <= GZ_ACC_TARGET_PX;" \
    "    out->within_one_degree = cs->median_px <= 2 * GZ_ACC_TARGET_PX;"

run_mutation "corrected stats skip the worst point" calibrate.c \
    "        if (cors[n] > worst) worst = cors[n];" \
    "        if (0) worst = cors[n];"
```

The exact patterns must match the code as written, so paste them from the file rather than from here. Run `make -C gaze-cal mutate 2>&1 | grep -E 'verdict|corrected stats'` and confirm every one reads `KILLED`. Adjust a test if one survives; a survivor means the test is decorative.

- [ ] **Step 8: Commit**

```bash
git add gaze-cal/src/calibrate.h gaze-cal/src/calibrate.c gaze-cal/tests/test_calibrate.c gaze-cal/tests/mutate.sh
git commit -m "feat: sweep verdict struct behind fit and accuracy" -m "- gz_fit_core and gz_accuracy_core fill gz_sweep_verdict
- wrappers keep today's printed output
- corrected stats and the verdict fills are pure and tested"
```

---

### Task 2: Stimulus back buffer, primitives and key input

**Files:**
- Modify: `gaze-cal/src/stimulus.h` (after `gz_stimulus_close`), Modify: `gaze-cal/src/stimulus.c`

**Interfaces:**
- Consumes: the existing `struct gz_stimulus` (file static, one per process), `gz_screen_point_px`.
- Produces, in `stimulus.h`:

```c
#define GZ_KEY_NONE   0
#define GZ_KEY_ENTER  1
#define GZ_KEY_ESCAPE 2
#define GZ_KEY_OTHER  3

/* Like gz_stimulus_open, plus a back buffer, key events and a keyboard grab.
 * The window is override_redirect, so KWin never focuses it: without the grab
 * it receives no key press at all. gz_stimulus_close releases the grab, and so
 * must every exit path, because a held grab locks the keyboard for the whole
 * session. Returns NULL after printing why. */
struct gz_stimulus *gz_stimulus_open_input(const char *output);

/* The next key press, or GZ_KEY_NONE. Never blocks. */
int gz_stimulus_key(struct gz_stimulus *s);

/* Back buffer drawing, window coordinates, colours as 0xRRGGBB. Nothing shows
 * until gz_stimulus_present copies the buffer to the window. The existing
 * gz_stimulus_show draws straight to the window and is unchanged. */
void gz_stimulus_clear(struct gz_stimulus *s);
void gz_stimulus_rect(struct gz_stimulus *s, int x, int y, int w, int h,
                      unsigned long rgb, int filled);
void gz_stimulus_disc(struct gz_stimulus *s, int cx, int cy, int r, unsigned long rgb);
/* An arc of `degrees` out of 360, starting at the top and running clockwise,
 * `width` pixels thick. 360 is a full ring. */
void gz_stimulus_ring(struct gz_stimulus *s, int cx, int cy, int r, int width,
                      unsigned long rgb, int degrees);
/* Left-aligned at (x, y) with y the baseline. Returns the width drawn in px. */
int  gz_stimulus_text(struct gz_stimulus *s, int x, int y, const char *text,
                      unsigned long rgb);
int  gz_stimulus_text_height(const struct gz_stimulus *s);
void gz_stimulus_present(struct gz_stimulus *s);
```

- [ ] **Step 1: Extend the struct and the open**

In `stimulus.c`, add to `struct gz_stimulus`: `Pixmap back; XFontStruct *font; int grabbed; unsigned long cache_rgb[8], cache_px[8]; int cache_n;`.

Factor the body of `gz_stimulus_open` into `static int open_common(const char *output, int input)`. When `input` is set:

```c
    at.event_mask = ExposureMask | KeyPressMask;
    ...
    g_stim.back = XCreatePixmap(g_stim.dpy, g_stim.win,
                                (unsigned)g_stim.screen.w, (unsigned)g_stim.screen.h,
                                (unsigned)DefaultDepth(g_stim.dpy, scr));
    static const char *fonts[] = {
        "-*-helvetica-bold-r-*-*-34-*-*-*-*-*-iso8859-1",
        "-*-*-bold-r-*-*-34-*-*-*-*-*-iso8859-1",
        "-*-*-medium-r-*-*-24-*-*-*-*-*-iso8859-1",
        "fixed", NULL };
    for (int i = 0; fonts[i] != NULL && g_stim.font == NULL; i++)
        g_stim.font = XLoadQueryFont(g_stim.dpy, fonts[i]);
    if (g_stim.font == NULL) { fprintf(stderr, "stimulus: no usable X font\n"); ... fail ... }
    XSetFont(g_stim.dpy, g_stim.gc, g_stim.font->fid);
    /* The grab can fail while another client holds one, or before the map
     * completes; retry a few times over 200 ms rather than run a view that
     * Escape cannot close. */
    for (int i = 0; i < 20 && !g_stim.grabbed; i++) {
        if (XGrabKeyboard(g_stim.dpy, g_stim.win, True, GrabModeAsync, GrabModeAsync,
                          CurrentTime) == GrabSuccess) g_stim.grabbed = 1;
        else { struct timespec t = { 0, 10 * 1000 * 1000 }; nanosleep(&t, NULL); }
    }
    if (!g_stim.grabbed) { fprintf(stderr, "stimulus: could not grab the keyboard\n"); ... fail ... }
```

Print which font was chosen on stderr the way the open prints the output name. `gz_stimulus_open` calls `open_common(output, 0)` and behaves exactly as before. `gz_stimulus_close` ungrabs when `grabbed`, frees the font and the pixmap, then does what it does today.

- [ ] **Step 2: Implement the primitives**

```c
static unsigned long pixel_for(struct gz_stimulus *s, unsigned long rgb) {
    for (int i = 0; i < s->cache_n; i++)
        if (s->cache_rgb[i] == rgb) return s->cache_px[i];
    XColor c;
    c.red   = (unsigned short)(((rgb >> 16) & 0xFF) * 257);
    c.green = (unsigned short)(((rgb >> 8) & 0xFF) * 257);
    c.blue  = (unsigned short)((rgb & 0xFF) * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    unsigned long px = XAllocColor(s->dpy, DefaultColormap(s->dpy, DefaultScreen(s->dpy)), &c)
                       ? c.pixel : s->white;
    if (s->cache_n < 8) { s->cache_rgb[s->cache_n] = rgb; s->cache_px[s->cache_n] = px; s->cache_n++; }
    return px;
}

void gz_stimulus_clear(struct gz_stimulus *s) {
    XSetForeground(s->dpy, s->gc, s->black);
    XFillRectangle(s->dpy, s->back, s->gc, 0, 0, (unsigned)s->screen.w, (unsigned)s->screen.h);
}

void gz_stimulus_rect(struct gz_stimulus *s, int x, int y, int w, int h,
                      unsigned long rgb, int filled) {
    XSetForeground(s->dpy, s->gc, pixel_for(s, rgb));
    if (filled) XFillRectangle(s->dpy, s->back, s->gc, x, y, (unsigned)w, (unsigned)h);
    else        XDrawRectangle(s->dpy, s->back, s->gc, x, y, (unsigned)w, (unsigned)h);
}

void gz_stimulus_disc(struct gz_stimulus *s, int cx, int cy, int r, unsigned long rgb) {
    XSetForeground(s->dpy, s->gc, pixel_for(s, rgb));
    XFillArc(s->dpy, s->back, s->gc, cx - r, cy - r, (unsigned)(2 * r), (unsigned)(2 * r), 0, 360 * 64);
}

void gz_stimulus_ring(struct gz_stimulus *s, int cx, int cy, int r, int width,
                      unsigned long rgb, int degrees) {
    if (degrees <= 0) return;
    if (degrees > 360) degrees = 360;
    XSetForeground(s->dpy, s->gc, pixel_for(s, rgb));
    XSetLineAttributes(s->dpy, s->gc, (unsigned)width, LineSolid, CapButt, JoinMiter);
    /* Xlib angles start at 3 o'clock and run anticlockwise in 64ths of a
     * degree. Start at 12 o'clock and sweep clockwise, which is a negative
     * extent from 90 degrees. */
    XDrawArc(s->dpy, s->back, s->gc, cx - r, cy - r, (unsigned)(2 * r), (unsigned)(2 * r),
             90 * 64, -degrees * 64);
    XSetLineAttributes(s->dpy, s->gc, 1, LineSolid, CapButt, JoinMiter);
}

int gz_stimulus_text(struct gz_stimulus *s, int x, int y, const char *text, unsigned long rgb) {
    int n = (int)strlen(text);
    XSetForeground(s->dpy, s->gc, pixel_for(s, rgb));
    XDrawString(s->dpy, s->back, s->gc, x, y, text, n);
    return XTextWidth(s->font, text, n);
}

int gz_stimulus_text_height(const struct gz_stimulus *s) {
    return s->font->ascent + s->font->descent;
}

void gz_stimulus_present(struct gz_stimulus *s) {
    XCopyArea(s->dpy, s->back, s->win, s->gc, 0, 0,
              (unsigned)s->screen.w, (unsigned)s->screen.h, 0, 0);
    XFlush(s->dpy);
}

int gz_stimulus_key(struct gz_stimulus *s) {
    int key = GZ_KEY_NONE;
    while (XPending(s->dpy) > 0) {
        XEvent ev;
        XNextEvent(s->dpy, &ev);
        if (ev.type != KeyPress) continue;
        KeySym sym = XLookupKeysym(&ev.xkey, 0);
        if (sym == XK_Return || sym == XK_KP_Enter) key = GZ_KEY_ENTER;
        else if (sym == XK_Escape) key = GZ_KEY_ESCAPE;
        else key = GZ_KEY_OTHER;
        if (key == GZ_KEY_ESCAPE) break;
    }
    return key;
}
```

`XK_Return` needs `#include <X11/keysym.h>`. Escape wins over anything else queued in the same poll, on purpose.

- [ ] **Step 3: Smoke it on the real display**

X is available on this machine. Write `/tmp/stim_smoke.c`:

```c
#include <stdio.h>
#include <time.h>
#include "stimulus.h"
int main(void) {
    struct gz_stimulus *s = gz_stimulus_open_input(NULL);
    if (!s) return 1;
    const struct gz_screen *sc = gz_stimulus_screen(s);
    for (int frame = 0; frame < 90; frame++) {
        gz_stimulus_clear(s);
        gz_stimulus_rect(s, 40, sc->h - 520, 640, 480, 0x888888, 0);
        gz_stimulus_disc(s, 300 + frame, sc->h - 280, 12, 0x4caf50);
        gz_stimulus_ring(s, sc->w / 2, sc->h / 2, 150, 8, 0xff9800, frame * 4);
        gz_stimulus_text(s, 40, sc->h - 20, "smoke: press Escape", 0xcccccc);
        gz_stimulus_present(s);
        if (gz_stimulus_key(s) == GZ_KEY_ESCAPE) break;
        struct timespec t = { 0, 33 * 1000 * 1000 }; nanosleep(&t, NULL);
    }
    gz_stimulus_close(s);
    puts("closed, keyboard released");
    return 0;
}
```

Build and run from the repo root:

```bash
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I gaze-cal/src -o /tmp/stim_smoke /tmp/stim_smoke.c gaze-cal/src/stimulus.c gaze-cal/src/calibrate.c gaze-cal/src/display.c gaze-cal/src/client.c gaze-cal/src/proto.c -lm -lX11 -lXrandr && /tmp/stim_smoke
```

Expected: a window on the primary output for three seconds showing a grey box, a moving green disc, a filling orange ring and readable text, with no flicker. Escape closes it early. After it exits, typing in the terminal works (the grab was released). If the display is off or nobody is at the machine, record that the smoke couldn't be judged visually and what the program printed.

- [ ] **Step 4: Build the CLI and the whole check**

Run: `make -C gaze-cal && make -C gaze-cal check 2>&1 | tail -2`
Expected: builds clean under `-Werror`, check exits 0, killed count unchanged from Task 1 (this task adds no testable pure code), 0 unexpected.

- [ ] **Step 5: Commit**

```bash
git add gaze-cal/src/stimulus.h gaze-cal/src/stimulus.c
git commit -m "feat: stimulus back buffer, primitives and keys" -m "- gz_stimulus_open_input grabs the keyboard for the override_redirect window
- rect, disc, ring and text draw into a Pixmap, present copies it
- gz_stimulus_show is unchanged for the sweep commands"
```

---

### Task 3: The pure half of the view

**Files:**
- Create: `gaze-cal/src/view.h`
- Create: `gaze-cal/src/view.c` (pure functions only in this task), Create: `gaze-cal/tests/test_view.c`
- Modify: `gaze-cal/Makefile` (`test` and `test-asan`), Modify: `gaze-cal/tests/mutate.sh` (new section at the end, before the summary)

**Interfaces:**
- Consumes: `struct gz_screen` (`calibrate.h:34`), `struct gz_sweep_verdict` and `GZ_ACC_TARGET_PX` from Task 1.
- Produces, in `view.h`:

```c
#include <stdint.h>
#include <stddef.h>
#include "calibrate.h"

#define GZ_VIEW_FLOOR_MM      520.0
#define GZ_VIEW_TARGET_NX     0.6
#define GZ_VIEW_TARGET_NY     0.85
#define GZ_VIEW_TARGET_R_PX   150
#define GZ_VIEW_ACCEPT_R_PX   250
#define GZ_DWELL_NS           1500000000ULL
#define GZ_DWELL_GAP_NS        250000000ULL
#define GZ_VIEW_NO_EYES_NS    1000000000ULL

struct gz_view_layout {
    int box_x, box_y, box_w, box_h;         /* window coordinates */
    int bar_x, bar_y, bar_w, bar_h;
    int readout_x, readout_y;               /* baseline of the readout line */
    int verdict_x, verdict_y;               /* baseline of the first verdict line */
    int target_cx, target_cy, target_r, accept_r;
    int eye_r;                              /* eye dot radius */
};
void gz_view_layout(const struct gz_screen *scr, struct gz_view_layout *out);

/* Track box value (x across, y down, both 0 to 1) to a pixel inside the box.
 * Clamped to the box, so a value the device reports slightly past 1 stays
 * visible on the edge rather than off the box. */
void gz_view_eye_px(const struct gz_view_layout *l, double box_x, double box_y,
                    int *px, int *py);
/* Box z (0 near, 1 far) to a y pixel on the bar, 0 at the bottom. */
int gz_view_bar_py(const struct gz_view_layout *l, double box_z);

/* Running least squares of eye z (mm) against box z. Seeded with the pairs
 * measured on 2026-09-01 so the floor line is in a sensible place before the
 * first live sample, and refined by every valid sample after. */
struct gz_zfit { double sx, sy, sxx, sxy; unsigned n; };
void   gz_zfit_init(struct gz_zfit *f);
void   gz_zfit_add(struct gz_zfit *f, double box_z, double z_mm);
double gz_zfit_box_z(const struct gz_zfit *f, double z_mm);   /* inverse map */

/* Fires exactly once after GZ_DWELL_NS continuously inside. Leaving, or a gap
 * longer than GZ_DWELL_GAP_NS between feeds, resets it. */
struct gz_dwell { uint64_t since_ns, last_ns; int fired; };
void gz_dwell_init(struct gz_dwell *d);
int  gz_dwell_feed(struct gz_dwell *d, int inside, uint64_t now_ns);
/* 0 to 360, how much of the ring to draw. */
int  gz_dwell_degrees(const struct gz_dwell *d, uint64_t now_ns);

enum gz_view_state {
    GZ_VIEW_IDLE, GZ_VIEW_FIT_SWEEP, GZ_VIEW_FIT_VERDICT,
    GZ_VIEW_VERIFY_SWEEP, GZ_VIEW_VERIFY_VERDICT, GZ_VIEW_CLOSED
};
enum gz_view_event  { GZ_EV_NONE, GZ_EV_TRIGGER, GZ_EV_ESCAPE, GZ_EV_SWEEP_DONE };
enum gz_view_action { GZ_ACT_NONE, GZ_ACT_RUN_FIT, GZ_ACT_RUN_VERIFY, GZ_ACT_CLOSE };

struct gz_view {
    enum gz_view_state state;
    struct gz_sweep_verdict verdict;
    int have_verdict;
};
void gz_view_init(struct gz_view *v);
/* `result` is read only for GZ_EV_SWEEP_DONE. */
enum gz_view_action gz_view_step(struct gz_view *v, enum gz_view_event ev,
                                 const struct gz_sweep_verdict *result);
/* "fit", "verify", "try again", "fit again", or "" while a sweep runs. */
const char *gz_view_target_word(const struct gz_view *v);
int gz_view_in_sweep(const struct gz_view *v);

/* The verdict block, up to four lines separated by '\n'. Returns the length. */
size_t gz_view_verdict_text(const struct gz_sweep_verdict *v, char *buf, size_t cap);

/* The readout line. `fit_stamp` is the correction's fit_utc or NULL. */
size_t gz_view_readout_text(double z_mm, int l_valid, int r_valid, double hz,
                            const char *fit_stamp, int stale_file, int no_eyes,
                            int reconnecting, char *buf, size_t cap);

/* Sweep sequencing, with I/O behind callbacks so the order is testable: close
 * the client, run the sweep, reload the correction after a successful fit,
 * reconnect, then feed the result to the state machine. Returns the action
 * the state machine wants next, normally GZ_ACT_NONE. */
struct gz_view_io {
    void *ctx;
    void (*close_client)(void *ctx);
    int  (*reconnect_client)(void *ctx);
    int  (*run_fit)(void *ctx, struct gz_sweep_verdict *out);
    int  (*run_verify)(void *ctx, struct gz_sweep_verdict *out);
    void (*reload_correction)(void *ctx);
};
enum gz_view_action gz_view_run_action(struct gz_view *v, enum gz_view_action act,
                                       const struct gz_view_io *io);

/* Reads the fit_utc line out of a correction file into `buf`, "" when the
 * file or the key is absent. Returns 1 when found. */
int gz_view_fit_stamp(const char *path, char *buf, size_t cap);
```

- [ ] **Step 1: Write the failing tests**

`gaze-cal/tests/test_view.c`:

```c
/* gaze-cal/tests/test_view.c
 *
 * The setup view's pure half: where things go on the screen, when the dwell
 * fires, what the state machine does, what the words say, and the order the
 * sweep sequencing calls its I/O in. None of it needs a display or a tracker.
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

static const struct gz_screen DP2 = { "DP-2", 4000, 1440, 2560, 1440 };

static void test_layout_is_a_third_tall_and_four_by_three(void) {
    struct gz_view_layout l;
    gz_view_layout(&DP2, &l);
    assert(l.box_h == 480);
    assert(l.box_w == 640);
    assert(l.box_x >= 0 && l.box_y + l.box_h <= 1440);
    assert(l.box_y > 720);                              /* bottom half */
    assert(l.bar_x > l.box_x + l.box_w);               /* beside, to the right */
    assert(l.bar_h == l.box_h && l.bar_y == l.box_y);
    assert(l.readout_y > l.box_y + l.box_h);           /* under the box */
    assert(l.readout_y <= 1440);
    assert(l.target_cx == 1536);                        /* 0.6 * 2560 */
    assert(l.target_cy == 1224);                        /* 0.85 * 1440 */
    assert(l.target_r == GZ_VIEW_TARGET_R_PX);
    assert(l.accept_r == GZ_VIEW_ACCEPT_R_PX);
    assert(l.verdict_y < l.target_cy - l.target_r);    /* above the target */
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
    gz_view_eye_px(&l, 0.0, 0.0, &px, &py);
    assert(px == l.box_x && py == l.box_y);
    gz_view_eye_px(&l, 1.0, 1.0, &px, &py);
    assert(px == l.box_x + l.box_w && py == l.box_y + l.box_h);
    gz_view_eye_px(&l, 0.5, 0.5, &px, &py);
    assert(px == l.box_x + l.box_w / 2 && py == l.box_y + l.box_h / 2);
    gz_view_eye_px(&l, 1.3, -0.2, &px, &py);          /* clamped */
    assert(px == l.box_x + l.box_w && py == l.box_y);
}

static void test_bar_maps_near_to_the_bottom(void) {
    struct gz_view_layout l;
    gz_view_layout(&DP2, &l);
    assert(gz_view_bar_py(&l, 0.0) == l.bar_y + l.bar_h);
    assert(gz_view_bar_py(&l, 1.0) == l.bar_y);
    assert(gz_view_bar_py(&l, 0.5) == l.bar_y + l.bar_h / 2);
}

static void test_zfit_seed_puts_the_floor_near_a_quarter(void) {
    struct gz_zfit f;
    gz_zfit_init(&f);
    double bz = gz_zfit_box_z(&f, GZ_VIEW_FLOOR_MM);
    assert(bz > 0.20 && bz < 0.27);                    /* 0.236 from the seed pairs */
    /* Adding the seed pairs again changes nothing. */
    gz_zfit_add(&f, 0.337, 567.0);
    gz_zfit_add(&f, 0.403, 598.0);
    assert(fabs(gz_zfit_box_z(&f, GZ_VIEW_FLOOR_MM) - bz) < 1e-6);
}

static void test_zfit_follows_new_pairs(void) {
    struct gz_zfit f;
    gz_zfit_init(&f);
    /* A thousand samples on a different line: box z = (mm - 400) / 500. */
    for (int i = 0; i < 1000; i++) {
        double mm = 450.0 + (i % 400);
        gz_zfit_add(&f, (mm - 400.0) / 500.0, mm);
    }
    assert(fabs(gz_zfit_box_z(&f, 520.0) - 0.24) < 0.01);
}

static void test_dwell_fires_once_at_the_threshold(void) {
    struct gz_dwell d;
    gz_dwell_init(&d);
    uint64_t t = 1000000000ULL;
    assert(gz_dwell_feed(&d, 1, t) == 0);
    assert(gz_dwell_feed(&d, 1, t + GZ_DWELL_NS - 1) == 0);
    assert(gz_dwell_degrees(&d, t + GZ_DWELL_NS / 2) == 180);
    assert(gz_dwell_feed(&d, 1, t + GZ_DWELL_NS) == 1);
    assert(gz_dwell_feed(&d, 1, t + GZ_DWELL_NS + 30000000ULL) == 0);   /* no second fire */
    assert(gz_dwell_degrees(&d, t + 2 * GZ_DWELL_NS) == 360);
}

static void test_dwell_resets_on_leave_and_on_a_gap(void) {
    struct gz_dwell d;
    gz_dwell_init(&d);
    uint64_t t = 5000000000ULL;
    gz_dwell_feed(&d, 1, t);
    gz_dwell_feed(&d, 0, t + 500000000ULL);                       /* left */
    assert(gz_dwell_degrees(&d, t + 600000000ULL) == 0);
    gz_dwell_feed(&d, 1, t + 600000000ULL);                       /* back in */
    assert(gz_dwell_feed(&d, 1, t + 600000000ULL + GZ_DWELL_NS - 1) == 0);
    assert(gz_dwell_feed(&d, 1, t + 600000000ULL + GZ_DWELL_NS) == 1);

    gz_dwell_init(&d);
    gz_dwell_feed(&d, 1, t);
    gz_dwell_feed(&d, 1, t + 1000000000ULL);
    /* A gap longer than GZ_DWELL_GAP_NS with no feed at all: the next feed
     * starts over, even though it is inside and 1.5 s have passed. */
    assert(gz_dwell_feed(&d, 1, t + 1000000000ULL + GZ_DWELL_GAP_NS + 1 + 600000000ULL) == 0);
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
    assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_NONE);   /* no double request */
    assert(gz_view_step(&v, GZ_EV_SWEEP_DONE, &bad) == GZ_ACT_NONE);
    assert(v.state == GZ_VIEW_FIT_VERDICT && v.have_verdict && v.verdict.refused);
    assert(strcmp(gz_view_target_word(&v), "try again") == 0);
    assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_RUN_FIT);
    assert(gz_view_step(&v, GZ_EV_SWEEP_DONE, &ok) == GZ_ACT_NONE);
    assert(strcmp(gz_view_target_word(&v), "verify") == 0);
    assert(gz_view_step(&v, GZ_EV_TRIGGER, NULL) == GZ_ACT_RUN_VERIFY);
    assert(v.state == GZ_VIEW_VERIFY_SWEEP);
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
    assert(strstr(buf, "now verify") != NULL);

    memset(&v, 0, sizeof v);
    v.kind = GZ_VERDICT_FIT; v.refused = 1;
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
    assert(strstr(buf, "FALSIFIED") != NULL);
    assert(strstr(buf, "fitted seat") == NULL);

    /* A tiny buffer never overflows and always terminates. */
    char tiny[8];
    size_t n = gz_view_verdict_text(&v, tiny, sizeof tiny);
    assert(n < sizeof tiny && tiny[n] == '\0');
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

static void test_run_action_ignores_none_and_close(void) {
    struct fake_io f = { "", 0, 0, 0 };
    struct gz_view_io io = { &f, f_close, f_reconnect, f_fit, f_verify, f_reload };
    struct gz_view v;
    gz_view_init(&v);
    assert(gz_view_run_action(&v, GZ_ACT_NONE, &io) == GZ_ACT_NONE);
    assert(gz_view_run_action(&v, GZ_ACT_CLOSE, &io) == GZ_ACT_CLOSE);
    assert(f.order[0] == '\0');
}

static void test_fit_stamp_reads_the_key(void) {
    char path[] = "/tmp/gz_view_stamp_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    fputs("version=2\ngx=1.1\nfit_utc=2026-09-01T14:35:06Z\nfit_points=9\n", fp);
    fclose(fp);
    char buf[64];
    assert(gz_view_fit_stamp(path, buf, sizeof buf) == 1);
    assert(strcmp(buf, "2026-09-01T14:35:06Z") == 0);
    unlink(path);
    assert(gz_view_fit_stamp(path, buf, sizeof buf) == 0);
    assert(buf[0] == '\0');
}

int main(void) {
    test_layout_is_a_third_tall_and_four_by_three();
    test_layout_scales_with_the_screen();
    test_eye_px_maps_the_box_edges_and_centre();
    test_bar_maps_near_to_the_bottom();
    test_zfit_seed_puts_the_floor_near_a_quarter();
    test_zfit_follows_new_pairs();
    test_dwell_fires_once_at_the_threshold();
    test_dwell_resets_on_leave_and_on_a_gap();
    test_state_machine_walks_the_diagram();
    test_escape_closes_from_every_state();
    test_verdict_text_for_each_outcome();
    test_readout_text();
    test_run_action_closes_before_and_reconnects_after();
    test_run_action_does_not_reload_after_a_refused_fit();
    test_run_action_ignores_none_and_close();
    test_fit_stamp_reads_the_key();
    puts("test_view: ok");
    return 0;
}
```

- [ ] **Step 2: Wire the Makefile and run to see it fail**

Add to `test`:

```make
	$(CC) $(CFLAGS) -o $(BUILD)/test_view tests/test_view.c src/view.c src/calibrate.c src/display.c src/client.c src/proto.c $(LDLIBS) && $(BUILD)/test_view
```

and the matching `test-asan` block following the `test_record_asan` lines exactly. Run `make -C gaze-cal test 2>&1 | tail -3`. Expected: `src/view.c: No such file`.

- [ ] **Step 3: Implement view.h and the pure half of view.c**

`view.h` is the Interfaces block with the usual include guard and `extern "C"` guards. `view.c`:

```c
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
```

Check the numbers against the layout test by hand before running: on 1440, margin 48, box 640 x 480 at (48, 1392 - 48 - 480 = 864), readout baseline 1392, bar at x 712, target (1536, 1224), verdict baseline 1224 - 150 - 192 = 882. The test asserts the verdict baseline is above the target and the box is in the bottom half, both hold. If a value collides (the verdict block above the target overlaps the box's right edge at 688 px), it is the drawing that will show it in Task 4, not these tests, so keep the verdict text left aligned at `verdict_x` 1386.

```c
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void gz_view_eye_px(const struct gz_view_layout *l, double box_x, double box_y,
                    int *px, int *py) {
    if (!(box_x >= 0.0)) box_x = 0.0;
    if (!(box_y >= 0.0)) box_y = 0.0;
    if (box_x > 1.0) box_x = 1.0;
    if (box_y > 1.0) box_y = 1.0;
    *px = l->box_x + (int)lround(box_x * l->box_w);
    *py = l->box_y + (int)lround(box_y * l->box_h);
    *px = clampi(*px, l->box_x, l->box_x + l->box_w);
    *py = clampi(*py, l->box_y, l->box_y + l->box_h);
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
```

The seed test expects 0.236 within (0.20, 0.27): slope (598 - 567) / (0.403 - 0.337) = 469.7, intercept 567 - 0.337 * 469.7 = 408.7, so 520 maps to 0.237. The "adding the seed again changes nothing" assertion holds because two identical points on a line don't move a least squares fit.

```c
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
```

Walk the gap test by hand: feeds at t and t+1 s, then a feed at t + 1 s + 250 ms + 1 ns + 600 ms. The gap is 850 ms, past 250 ms, so `since` restarts at that feed and nothing fires, which is what the test asserts. Note a gap after a fire also resets `fired`, so a user who looks away and comes back can trigger again, which is the behaviour the target words rely on.

```c
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
```

- [ ] **Step 4: Run the tests**

Run: `make -C gaze-cal test 2>&1 | tail -3`
Expected: `test_view: ok` and every other suite unchanged. Fix any assertion that fails by first deciding whether the test or the code is wrong, and say which in the report.

- [ ] **Step 5: Add the mutation section**

Append to `mutate.sh` before the final `echo`, following the record section's shape:

```bash
echo "== view mutations =="
TEST_MAIN="tests/test_view.c"
TEST_SRCS="src/view.c src/calibrate.c src/display.c src/client.c src/proto.c"

run_mutation "the box is not a third of the screen" view.c \
    "    out->box_h = scr->h / 3;" "    out->box_h = scr->h / 4;"

run_mutation "eye dots not clamped to the box" view.c \
    "    if (box_x > 1.0) box_x = 1.0;" "    if (0) box_x = 1.0;"

run_mutation "bar drawn upside down" view.c \
    "    return l->bar_y + l->bar_h - (int)lround(box_z * l->bar_h);" \
    "    return l->bar_y + (int)lround(box_z * l->bar_h);"

run_mutation "z fit ignores live samples" view.c \
    "    f->sx += box_z; f->sy += z_mm; f->sxx += box_z * box_z; f->sxy += box_z * z_mm;" \
    "    if (f->n >= 2) return;
    f->sx += box_z; f->sy += z_mm; f->sxx += box_z * box_z; f->sxy += box_z * z_mm;"

run_mutation "dwell fires every frame after the threshold" view.c \
    "    if (!d->fired && now_ns - d->since_ns >= GZ_DWELL_NS) {" \
    "    if (now_ns - d->since_ns >= GZ_DWELL_NS) {"

run_mutation "dwell fires early" view.c \
    "    if (!d->fired && now_ns - d->since_ns >= GZ_DWELL_NS) {" \
    "    if (!d->fired && now_ns - d->since_ns >= GZ_DWELL_NS / 2) {"

run_mutation "dwell does not reset on leave" view.c \
    "    if (!inside || gap) {" "    if (gap) {"

run_mutation "dwell does not reset on a gap" view.c \
    "    if (!inside || gap) {" "    if (!inside) {"

run_mutation "a refused fit leads to verify" view.c \
    "            if (v->verdict.refused) { v->state = GZ_VIEW_FIT_SWEEP; return GZ_ACT_RUN_FIT; }" \
    "            if (0) { v->state = GZ_VIEW_FIT_SWEEP; return GZ_ACT_RUN_FIT; }"

run_mutation "a trigger during a sweep starts another" view.c \
    "    case GZ_VIEW_FIT_SWEEP:
        if (ev == GZ_EV_SWEEP_DONE && result != NULL) {" \
    "    case GZ_VIEW_FIT_SWEEP:
        if (ev == GZ_EV_TRIGGER) return GZ_ACT_RUN_FIT;
        if (ev == GZ_EV_SWEEP_DONE && result != NULL) {"

run_mutation "escape does not close" view.c \
    "    if (ev == GZ_EV_ESCAPE) { v->state = GZ_VIEW_CLOSED; return GZ_ACT_CLOSE; }" \
    "    if (ev == GZ_EV_ESCAPE) { return GZ_ACT_CLOSE; }"

run_mutation "the client is not closed before a sweep" view.c \
    "    io->close_client(io->ctx);
    if (act == GZ_ACT_RUN_FIT) {" \
    "    if (act == GZ_ACT_RUN_FIT) {"

run_mutation "the client is not reconnected after a sweep" view.c \
    "    io->reconnect_client(io->ctx);
    return gz_view_step(v, GZ_EV_SWEEP_DONE, &result);" \
    "    return gz_view_step(v, GZ_EV_SWEEP_DONE, &result);"

run_mutation "the correction is reloaded after a refused fit" view.c \
    "        if (!result.refused && result.rc == 0) io->reload_correction(io->ctx);" \
    "        io->reload_correction(io->ctx);"

run_mutation "the verdict text drops the one degree call" view.c \
    "                     v->within_one_degree ? \"WITHIN ONE DEGREE\" : \"OUTSIDE ONE DEGREE\",
                     v->gx, v->gy, v->next);" \
    "                     \"\",
                     v->gx, v->gy, v->next);"

run_mutation "fit_utc read from the wrong key" view.c \
    "        if (strncmp(line, \"fit_utc=\", 8) != 0) continue;" \
    "        if (strncmp(line, \"fit_pts=\", 8) != 0) continue;"
```

Also add `view.c`, `view.h` and `test_view.c` to `fresh_copy`. Run `make -C gaze-cal mutate 2>&1 | grep -A40 'view mutations'` and confirm every line reads `KILLED`. Any `STALE pattern` means the plan's text and your code differ; fix the pattern, not the code.

- [ ] **Step 6: Run the whole check and commit**

Run: `make -C gaze-cal check 2>&1 | tail -2`
Expected: exit 0, killed at least 16 above Task 1's count, 0 unexpected.

```bash
git add gaze-cal/src/view.h gaze-cal/src/view.c gaze-cal/tests/test_view.c gaze-cal/Makefile gaze-cal/tests/mutate.sh
git commit -m "feat: setup view layout, dwell and state machine" -m "- pure half of view.c with test_view under test and test-asan
- sweep sequencing closes the client first and reconnects after
- sixteen view mutations, all killed"
```

---

### Task 4: The setup command and its frame loop

**Files:**
- Modify: `gaze-cal/src/view.h` (add `gz_cmd_setup` and `struct gz_setup_opts`), Modify: `gaze-cal/src/view.c` (the frame loop at the bottom), Modify: `gaze-cal/src/main.c` (usage at 201, dispatch after the `record` block), Modify: `gaze-cal/src/calibrate.h` and `calibrate.c` (export `gz_log_path`), Modify: `scripts/fit-correction.sh` (header comment), Modify: `docs/RESUME-phase1.md` (the human steps)

**Interfaces:**
- Consumes: `gz_stimulus_open_input`, `gz_stimulus_key`, the primitives and `gz_stimulus_present` (Task 2); `gz_fit_core`, `gz_accuracy_core` (Task 1); everything in `view.h` (Task 3); `gz_connect_and_gate`, `gz_correction_load`, `gz_correction_path`, `gz_client_poll`, `gz_client_reconnect`, `gz_client_close`, `gz_client_watchdog`, `gz_now_ns`, `gz_gaze_correct`.
- Produces:

```c
struct gz_setup_opts {
    const char *sock;       /* daemon socket path */
    const char *cfg;        /* tobii.json or NULL */
    const char *output;     /* RandR output name or NULL for the primary */
};
/* Opens the view and runs it until Escape. Exit codes match the stimulus
 * commands: 0 closed by the user, 1 the daemon was unreachable, 3 the
 * geometry could not be read, 2 usage. */
int gz_cmd_setup(const struct gz_setup_opts *o);
```

and in `calibrate.h`: `int gz_log_path(char *buf, size_t cap);` returning the path of `gaze-cal.log` in the data dir (the same directory `data_dir` at `calibrate.c:47` resolves).

- [ ] **Step 1: Export the log path**

In `calibrate.c` add beside `gz_correction_path`:

```c
int gz_log_path(char *buf, size_t cap) {
    char dir[512];
    if (data_dir(dir, sizeof dir) != 0) return -1;
    int n = snprintf(buf, cap, "%s/gaze-cal.log", dir);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}
```

Check what `log_open` at `calibrate.c:58` builds the path from and use the same expression, so the view appends to the file the sweeps write.

- [ ] **Step 2: Write the frame loop**

Append to `view.c`. The stimulus callback for the hosted sweeps draws through the back buffer and polls Escape:

```c
#include <signal.h>
#include <time.h>
#include "client.h"
#include "display.h"
#include "stimulus.h"

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

/* The sweep's dot, drawn through the back buffer so the box can be hidden
 * under it, and Escape checked between dots so a sweep can be abandoned. */
static int setup_stim_show(void *ctx, double nx, double ny) {
    struct setup_ctx *s = ctx;
    if (g_setup_stop) return -1;
    if (gz_stimulus_key(s->stim) == GZ_KEY_ESCAPE) { s->escaped = 1; return -1; }
    const struct gz_screen *scr = gz_stimulus_screen(s->stim);
    int px, py;
    gz_screen_point_px(scr, nx, ny, &px, &py);
    int wx = px - scr->x, wy = py - scr->y;
    gz_stimulus_clear(s->stim);
    gz_stimulus_disc(s->stim, wx, wy, GZ_STIM_DOT_PX / 2, 0xffffff);
    gz_stimulus_disc(s->stim, wx, wy, GZ_STIM_INNER_PX / 2, 0x000000);
    gz_stimulus_present(s->stim);
    return 0;
}

static void load_correction(struct setup_ctx *s) {
    s->have_corr = 0; s->stale_file = 0; s->fit_stamp[0] = '\0';
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
```

The draw of one frame, then the loop:

```c
#define SETUP_POLL_MS 10
#define GREY 0x888888
#define DIM 0x333333
#define GREEN 0x4caf50
#define RED 0xe53935
#define ORANGE 0xff9800
#define TEXT 0xcccccc

static void draw_frame(struct setup_ctx *s, const struct gz_view_layout *l,
                       const struct gz_view *v, const struct gz_gaze_sample *sample,
                       int have_sample, const int last_eye_px[2][2], const int last_eye_ok[2],
                       const struct gz_zfit *zf, const struct gz_dwell *dw,
                       uint64_t now, double hz, int no_eyes, int reconnecting) {
    struct gz_stimulus *st = s->stim;
    gz_stimulus_clear(st);

    /* the box and its centre lines */
    gz_stimulus_rect(st, l->box_x, l->box_y, l->box_w, l->box_h, GREY, 0);
    gz_stimulus_rect(st, l->box_x + l->box_w / 2, l->box_y, 1, l->box_h, DIM, 1);
    gz_stimulus_rect(st, l->box_x, l->box_y + l->box_h / 2, l->box_w, 1, DIM, 1);

    /* eyes: filled green when valid, hollow red at the last position otherwise */
    for (int e = 0; e < 2; e++) {
        int valid = have_sample && (e == 0 ? sample->validity_L : sample->validity_R) == GZ_VALIDITY_VALID;
        if (valid) {
            const double *tb = e == 0 ? sample->trackbox_eye_pos_L : sample->trackbox_eye_pos_R;
            int px, py;
            gz_view_eye_px(l, tb[0], tb[1], &px, &py);
            gz_stimulus_disc(st, px, py, l->eye_r, GREEN);
        } else if (last_eye_ok[e]) {
            gz_stimulus_ring(st, last_eye_px[e][0], last_eye_px[e][1], l->eye_r, 3, RED, 360);
        }
    }

    /* the distance bar, the floor and the marker */
    gz_stimulus_rect(st, l->bar_x, l->bar_y, l->bar_w, l->bar_h, GREY, 0);
    int floor_py = gz_view_bar_py(l, gz_zfit_box_z(zf, GZ_VIEW_FLOOR_MM));
    gz_stimulus_rect(st, l->bar_x - 6, floor_py, l->bar_w + 12, 3, RED, 1);
    gz_stimulus_text(st, l->bar_x + l->bar_w + 12, floor_py + 8, "520", RED);
    double z_mm = 0;
    if (have_sample && gz_sample_any_eye_valid(sample)) {
        int nl = sample->validity_L == GZ_VALIDITY_VALID, nr = sample->validity_R == GZ_VALIDITY_VALID;
        double bz = 0;
        if (nl) { bz += sample->trackbox_eye_pos_L[2]; z_mm += sample->eye_origin_L_mm[2]; }
        if (nr) { bz += sample->trackbox_eye_pos_R[2]; z_mm += sample->eye_origin_R_mm[2]; }
        bz /= (nl + nr); z_mm = fabs(z_mm / (nl + nr));
        int py = gz_view_bar_py(l, bz);
        gz_stimulus_rect(st, l->bar_x + 1, py - 3, l->bar_w - 1, 6, GREEN, 1);
    }

    /* readout */
    char line[256];
    gz_view_readout_text(z_mm,
                         have_sample && sample->validity_L == GZ_VALIDITY_VALID,
                         have_sample && sample->validity_R == GZ_VALIDITY_VALID,
                         hz, s->have_corr ? s->fit_stamp : NULL, s->stale_file,
                         no_eyes, reconnecting, line, sizeof line);
    gz_stimulus_text(st, l->readout_x, l->readout_y, line, TEXT);
    if (s->gate_mismatch)
        gz_stimulus_text(st, l->readout_x, l->readout_y - gz_stimulus_text_height(st) - 4,
                         "display area mismatch: fix with tobiifreed --force-display-area", RED);

    /* gaze rings */
    const struct gz_screen *scr = gz_stimulus_screen(st);
    if (have_sample && gz_sample_any_eye_valid(sample)) {
        int px, py;
        gz_screen_point_px(scr, sample->gaze_point_2d_norm[0], sample->gaze_point_2d_norm[1], &px, &py);
        gz_stimulus_ring(st, px - scr->x, py - scr->y, GZ_STIM_DOT_PX / 2 + 4, 3, ORANGE, 360);
        double cor[2];
        if (s->have_corr && gz_gaze_correct(&s->corr, sample, cor)) {
            gz_screen_point_px(scr, cor[0], cor[1], &px, &py);
            gz_stimulus_ring(st, px - scr->x, py - scr->y, GZ_STIM_DOT_PX / 2 + 4, 3, GREEN, 360);
        }
    }

    /* target and its dwell ring */
    const char *word = gz_view_target_word(v);
    if (word[0] != '\0') {
        gz_stimulus_disc(st, l->target_cx, l->target_cy, l->target_r, 0x2a2a2a);
        gz_stimulus_ring(st, l->target_cx, l->target_cy, l->target_r, 2, GREY, 360);
        gz_stimulus_ring(st, l->target_cx, l->target_cy, l->target_r - 8, 8, GREEN,
                         gz_dwell_degrees(dw, now));
        int tw = gz_stimulus_text(st, -10000, -10000, word, TEXT);   /* measure only */
        gz_stimulus_text(st, l->target_cx - tw / 2, l->target_cy + gz_stimulus_text_height(st) / 3,
                         word, TEXT);
        gz_stimulus_text(st, l->target_cx - l->target_r, l->target_cy + l->target_r + 40,
                         "look here 1.5 s, or press Enter. Escape closes", DIM);
    }

    /* verdict block */
    if (v->have_verdict) {
        char text[512];
        gz_view_verdict_text(&v->verdict, text, sizeof text);
        int y = l->verdict_y;
        int lh = gz_stimulus_text_height(st) + 6;
        for (char *p = text; p != NULL && *p; ) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            gz_stimulus_text(st, l->verdict_x, y, p, v->verdict.refused ? RED : TEXT);
            y += lh;
            p = nl ? nl + 1 : NULL;
        }
    }

    gz_stimulus_present(st);
}
```

Measuring text by drawing it off screen is a cheap trick; if the reviewer prefers, split `gz_stimulus_text_width` out in `stimulus.c` and call that. Either way the target word must be centred.

```c
int gz_cmd_setup(const struct gz_setup_opts *o) {
    struct setup_ctx s;
    memset(&s, 0, sizeof s);
    s.o = o;

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

    /* Gate once, the way the sweeps do, so the correction is loaded against
     * the geometry the device really holds. A mismatch still opens the view:
     * the box needs no geometry, and the sweeps will refuse on their own. */
    int g = gz_connect_and_gate(&s.c, o->sock, o->cfg, 1, &s.panel);
    if (g == GZ_GATE_UNKNOWN || s.c.fd < 0) {
        gz_stimulus_clear(s.stim);
        gz_stimulus_text(s.stim, l.readout_x, scr->h / 2,
                         "the gaze daemon is not answering. Start it with: systemctl --user start tobiifreed",
                         RED);
        gz_stimulus_text(s.stim, l.readout_x, scr->h / 2 + 50, "press any key to close", TEXT);
        gz_stimulus_present(s.stim);
        while (!g_setup_stop && gz_stimulus_key(s.stim) == GZ_KEY_NONE) {
            struct timespec t = { 0, 50 * 1000 * 1000 }; nanosleep(&t, NULL);
        }
        gz_client_close(&s.c);
        gz_stimulus_close(s.stim);
        return g == GZ_GATE_UNKNOWN ? 3 : 1;
    }
    s.connected = 1;
    s.have_panel = (g == GZ_GATE_OK);
    s.gate_mismatch = (g == GZ_GATE_MISMATCH);
    load_correction(&s);

    char logpath[512];
    if (gz_log_path(logpath, sizeof logpath) == 0) s.log = fopen(logpath, "a");
    if (s.log) fprintf(s.log, "\n=== setup view opened ===\n");

    struct gz_view v;
    gz_view_init(&v);
    struct gz_zfit zf;
    gz_zfit_init(&zf);
    struct gz_dwell dw;
    gz_dwell_init(&dw);
    struct gz_view_io io = { &s, io_close, io_reconnect, io_run_fit, io_run_verify, io_reload };

    int last_eye_px[2][2] = { { 0, 0 }, { 0, 0 } };
    int last_eye_ok[2] = { 0, 0 };
    uint64_t last_any_eye = gz_now_ns(), last_log = 0, rate_t0 = gz_now_ns();
    unsigned rate_n = 0;
    double hz = 0;
    int reconnecting = 0;
    int rc = 0;

    while (!g_setup_stop && v.state != GZ_VIEW_CLOSED) {
        uint64_t now = gz_now_ns();

        /* keys first, so Escape works even while the link is down */
        int key = gz_stimulus_key(s.stim);
        enum gz_view_event ev = key == GZ_KEY_ESCAPE ? GZ_EV_ESCAPE
                              : key == GZ_KEY_ENTER  ? GZ_EV_TRIGGER : GZ_EV_NONE;

        int have_sample = 0;
        struct gz_gaze_sample sample;
        if (s.connected) {
            int r = gz_client_poll(&s.c, SETUP_POLL_MS);
            if (r == GZ_CLIENT_RECONNECT || gz_client_watchdog(&s.c, now) != 0) {
                io_close(&s);
                reconnecting = 1;
            } else if (s.c.have_latest) {
                sample = s.c.latest;
                s.c.have_latest = 0;
                have_sample = 1;
                rate_n++;
            }
        } else {
            struct timespec t = { 0, SETUP_POLL_MS * 1000 * 1000 }; nanosleep(&t, NULL);
            if (io_reconnect(&s) == 0) reconnecting = 0;
        }

        if (now - rate_t0 >= 1000000000ULL) {
            hz = rate_n * 1e9 / (double)(now - rate_t0);
            rate_n = 0; rate_t0 = now;
        }

        /* last known eye positions, for the hollow red disc */
        if (have_sample) {
            for (int e = 0; e < 2; e++) {
                int valid = (e == 0 ? sample.validity_L : sample.validity_R) == GZ_VALIDITY_VALID;
                if (!valid) continue;
                const double *tb = e == 0 ? sample.trackbox_eye_pos_L : sample.trackbox_eye_pos_R;
                gz_view_eye_px(&l, tb[0], tb[1], &last_eye_px[e][0], &last_eye_px[e][1]);
                last_eye_ok[e] = 1;
                const double *eye = e == 0 ? sample.eye_origin_L_mm : sample.eye_origin_R_mm;
                gz_zfit_add(&zf, tb[2], fabs(eye[2]));
            }
            if (gz_sample_any_eye_valid(&sample)) last_any_eye = now;
        }
        int no_eyes = now - last_any_eye >= GZ_VIEW_NO_EYES_NS;

        /* dwell on the target, corrected gaze when there is one */
        int inside = 0;
        if (have_sample && gz_sample_any_eye_valid(&sample) && gz_view_target_word(&v)[0]) {
            double g2[2] = { sample.gaze_point_2d_norm[0], sample.gaze_point_2d_norm[1] };
            double cor[2];
            if (s.have_corr && gz_gaze_correct(&s.corr, &sample, cor)) { g2[0] = cor[0]; g2[1] = cor[1]; }
            int px, py;
            gz_screen_point_px(scr, g2[0], g2[1], &px, &py);
            inside = hypot(px - scr->x - l.target_cx, py - scr->y - l.target_cy) <= l.accept_r;
        }
        if (have_sample && gz_dwell_feed(&dw, inside, now) && ev == GZ_EV_NONE) ev = GZ_EV_TRIGGER;

        if (s.log && have_sample && now - last_log >= 1000000000ULL) {
            last_log = now;
            fprintf(s.log, "setup z=%.0f tbL=(%.3f,%.3f,%.3f) tbR=(%.3f,%.3f,%.3f) vL=%u vR=%u raw=(%.4f,%.4f)\n",
                    fabs(sample.eye_origin_L_mm[2]),
                    sample.trackbox_eye_pos_L[0], sample.trackbox_eye_pos_L[1], sample.trackbox_eye_pos_L[2],
                    sample.trackbox_eye_pos_R[0], sample.trackbox_eye_pos_R[1], sample.trackbox_eye_pos_R[2],
                    sample.validity_L, sample.validity_R,
                    sample.gaze_point_2d_norm[0], sample.gaze_point_2d_norm[1]);
            fflush(s.log);
        }

        enum gz_view_action act = gz_view_step(&v, ev, NULL);
        if (act == GZ_ACT_RUN_FIT || act == GZ_ACT_RUN_VERIFY) {
            s.escaped = 0;
            gz_view_run_action(&v, act, &io);
            gz_dwell_init(&dw);
            reconnecting = !s.connected;
            if (s.escaped) { gz_view_step(&v, GZ_EV_ESCAPE, NULL); }
            continue;
        }

        draw_frame(&s, &l, &v, &sample, have_sample, last_eye_px, last_eye_ok, &zf, &dw,
                   now, hz, no_eyes, reconnecting);
    }

    if (s.log) { fprintf(s.log, "=== setup view closed ===\n"); fclose(s.log); }
    io_close(&s);
    gz_stimulus_close(s.stim);     /* releases the keyboard grab */
    return rc;
}
```

Things to check while writing this, since the plan's code is a draft:

- `gz_connect_and_gate`'s behaviour on a connect failure: read it at `calibrate.c` and confirm what `c.fd` and the return code look like when the socket is absent, so the "daemon not answering" branch really fires then and the exit code is 1 for that case and 3 for a geometry read failure, matching `usage()`.
- `gz_client_watchdog`'s return contract (`client.h:214`), so a healthy stream isn't treated as lost.
- The hosted sweep runs while the view's own client is closed, and `gz_fit_core` prints to the terminal during it. That is intended.
- After a sweep the view's client reconnects and subscribes (`gz_client_connect` does that), and the sample rate readout restarts from zero for a second.
- The frame loop draws only when it has time: with `SETUP_POLL_MS` at 10 and a 33 Hz stream, a frame is drawn on every poll return, which is up to 100 Hz of `XCopyArea` on a 2560 x 1440 pixmap. If CPU shows above a few percent in `top`, draw only when `have_sample` or every 33 ms, whichever comes first, and say so in the report.

- [ ] **Step 3: Wire main.c**

Usage, after the `record` line:

```c
            "  setup [--output NAME] [--config PATH]\n"
            "                             fullscreen view of the device's track box,\n"
            "                             both eyes, distance and gaze, with the fit\n"
            "                             and verify sweeps run from the screen\n"
```

and add `setup` to the exit code sentence. Dispatch, after the `record` block:

```c
    if (strcmp(argv[i], "setup") == 0) {
        struct gz_setup_opts o = { path, NULL, NULL };
        if (parse_common(argc, argv, i + 1, &o.output, &o.cfg, NULL) != 0) {
            usage();
            return 2;
        }
        return gz_cmd_setup(&o);
    }
```

`#include "view.h"` at the top with the others.

- [ ] **Step 4: Build and smoke without a sweep**

Run: `make -C gaze-cal && ./gaze-cal/build/gaze-cal setup`

If the daemon is running and someone is at the screen: the box, eye dots, distance bar, readout and orange gaze ring appear, the fit target reads "fit", Escape closes and the terminal accepts typing afterwards. Don't press Enter and don't dwell on the target, since the sweep needs the human. If nobody is at the screen: the box shows no eye dots, after a second the readout says to check the lights, Escape closes. If the daemon is stopped: the "not answering" screen shows and a key closes it with exit 1 or 3. Record which of these you could observe and paste the terminal output. Also `tail -3 ~/.local/share/tobii-gaze/gaze-cal.log` and paste the `setup` lines.

- [ ] **Step 5: Whole check, docs, commit**

Run: `make -C gaze-cal check 2>&1 | tail -2`. Expected: exit 0, 0 unexpected, killed unchanged from Task 3.

`scripts/fit-correction.sh`: add to the header comment, after the three sweeps list:

```
# `gaze-cal setup` does the same from a fullscreen screen that shows the
# tracker's view of both eyes and the distance first, and is the better way
# for a first fit. This script remains for a terminal-only session.
```

`docs/RESUME-phase1.md`, in "What the human still has to do", item 1: name `gaze-cal setup` as the way to fit, with the script as the alternative, and keep the lights and 600 mm requirements.

```bash
git add gaze-cal/src/view.h gaze-cal/src/view.c gaze-cal/src/main.c gaze-cal/src/calibrate.h gaze-cal/src/calibrate.c scripts/fit-correction.sh docs/RESUME-phase1.md
git commit -m "feat: gaze-cal setup, the track box view" -m "- frame loop draws the box, eyes, distance, gaze and the dwell target
- fit and verify run from the screen through the cores
- one log line per second to gaze-cal.log"
```

---

### Task 5: Live check with the human

Not a subagent task. The controller asks the human for one session in front of `gaze-cal setup`:

1. Lights on, seated where they play. Open `./gaze-cal/build/gaze-cal setup`. Both eye dots green, distance near 600 mm on the bar above the red floor line.
2. Lean left until one eye dot goes hollow red. Lean in until the marker crosses the floor line. Cover one eye with a hand and confirm that eye alone goes red.
3. Look at the target for 1.5 s. The fit sweep runs. The verdict block appears. Compare the median and worst px in the window against the terminal's `median residual` line: they must match to the pixel.
4. Look at the target again for the verify sweep without moving. Expected 35 to 50 px within one degree, matching the terminal's `CORRECTED median` line.
5. Escape. The terminal accepts typing.

Record every number in the ledger. Any mismatch between the window and the terminal is a defect in Task 1's fills and goes back through the fix loop.
