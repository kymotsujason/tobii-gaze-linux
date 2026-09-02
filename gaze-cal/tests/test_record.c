/* gaze-cal/tests/test_record.c
 *
 * The trace recorder's pure half. Everything that decides what a row SAYS is
 * here, because a trace is only worth recording if the numbers in it can be
 * trusted, and a five-minute osu session is an expensive way to find out that
 * a column was wrong.
 *
 * The corrected columns are the reason this file exists. A trace of raw device
 * output is useless to Task 16: the device carries an isotropic gain of about
 * 1.18 that the host-side form S correction removes, so a filter fitted
 * against raw gaze is fitted against the wrong signal. Every case below that
 * checks a corr_* field is checking that the trace carries the corrected
 * number rather than a copy of the raw one.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/record.h"

#ifdef NDEBUG
#error "test_record.c relies on assert(); do not build it with NDEBUG"
#endif

/* ---------- helpers ---------- */

/* Splits a row into its comma-separated fields, dropping the trailing newline.
 * Returns the field count. Comparing field by field rather than against one
 * long string is what makes a failure name the column that moved. */
#define MAX_FIELDS 64

static int split_row(char *row, char *out[MAX_FIELDS]) {
    size_t n = strlen(row);
    assert(n > 0);
    assert(row[n - 1] == '\n');
    row[n - 1] = '\0';

    int k = 0;
    char *p = row;
    out[k++] = p;
    while (*p != '\0') {
        if (*p == ',') {
            *p = '\0';
            assert(k < MAX_FIELDS);
            out[k++] = p + 1;
        }
        p++;
    }
    return k;
}

/* A sample with both eyes valid and every recorded field distinct, so a row
 * that reads the wrong member cannot accidentally match. */
static void base_sample(struct gz_gaze_sample *s) {
    memset(s, 0, sizeof *s);
    s->present_mask = 0x003fffffu;
    s->frame_counter = 1000;
    s->validity_L = 0;                /* 0 means VALID. Do not invert. */
    s->validity_R = 0;
    s->timestamp_us = 42;
    s->pupil_L_mm = 3.21;
    s->pupil_R_mm = 4.56;
    s->gaze_point_2d_L_norm[0] = 0.5;
    s->gaze_point_2d_L_norm[1] = 0.4;
    s->gaze_point_2d_R_norm[0] = 0.6;
    s->gaze_point_2d_R_norm[1] = 0.45;
    s->gaze_point_2d_norm[0] = 0.55;
    s->gaze_point_2d_norm[1] = 0.425;
    s->gaze_point_2d_unfiltered[0] = 0.56;
    s->gaze_point_2d_unfiltered[1] = 0.43;
    /* Tracker millimetres. Distinct on every axis so a row that reads the
     * wrong member cannot accidentally match, and a plausible seat: the eyes
     * about 65 mm apart and about 675 mm out, which is where the measured
     * sweeps sat. */
    s->eye_origin_L_mm[0] = -32.780;
    s->eye_origin_L_mm[1] = 12.500;
    s->eye_origin_L_mm[2] = -675.250;
    s->eye_origin_R_mm[0] = 32.560;
    s->eye_origin_R_mm[1] = 13.125;
    s->eye_origin_R_mm[2] = -674.500;
}

/* gx=1.2, gy=1.1, bx=0.01, by=-0.02, the numbers Task 15's expected values
 * below are worked out by hand from. Deliberately NOT isotropic to
 * GZ_CORR_ISO_TOL, so gz_correction_check would refuse it: valid is set by
 * hand because the row formatter's job is to apply what it is given, and the
 * refusing is gz_correction_load's. */
static void base_correction(struct gz_correction *c) {
    memset(c, 0, sizeof *c);
    c->gx = 1.2;
    c->gy = 1.1;
    c->bx = 0.01;
    c->by = -0.02;
    c->area.w_mm = 590.42;
    c->area.h_mm = 333.72;
    c->form = GZ_CORR_FORM_STATIC;
    c->valid = 1;
}

/* ---------- the header ---------- */

static void test_header_names_the_brief_columns_then_the_corrected_six(void) {
    char hdr[512];
    snprintf(hdr, sizeof hdr, "%s", GZ_RECORD_HEADER);

    char *f[MAX_FIELDS];
    int n = split_row(hdr, f);

    static const char *want[] = {
        "host_ns", "device_us", "frame_counter", "present_mask",
        "validity_L", "validity_R",
        "lx", "ly", "rx", "ry",
        "combined_x", "combined_y",
        "unfiltered_x", "unfiltered_y",
        "pupil_L", "pupil_R",
        "corr_lx", "corr_ly", "corr_rx", "corr_ry", "corr_cx", "corr_cy",
        "eye_lx", "eye_ly", "eye_lz", "eye_rx", "eye_ry", "eye_rz"
    };
    assert(n == (int)(sizeof want / sizeof want[0]));
    for (int i = 0; i < n; i++) assert(strcmp(f[i], want[i]) == 0);
}

static void test_a_row_has_as_many_fields_as_the_header(void) {
    char hdr[512], row[GZ_RECORD_ROW_MAX];
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);

    snprintf(hdr, sizeof hdr, "%s", GZ_RECORD_HEADER);
    assert(gz_record_row(row, sizeof row, &s, &c, 123456789ULL) > 0);

    char *hf[MAX_FIELDS], *rf[MAX_FIELDS];
    int hn = split_row(hdr, hf);
    int rn = split_row(row, rf);
    assert(hn == rn);
}

/* ---------- the numbers ---------- */

static void test_a_valid_sample_yields_the_hand_computed_row(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);

    char row[GZ_RECORD_ROW_MAX];
    size_t n = gz_record_row(row, sizeof row, &s, &c, 123456789ULL);
    assert(n > 0);
    assert(n == strlen(row));
    assert(row[n - 1] == '\n');

    /* Worked out by hand from (reported - b) / g:
     *   corr_lx = (0.50 - 0.01) / 1.2  = 0.49  / 1.2 = 0.408333
     *   corr_ly = (0.40 + 0.02) / 1.1  = 0.42  / 1.1 = 0.381818
     *   corr_rx = (0.60 - 0.01) / 1.2  = 0.59  / 1.2 = 0.491667
     *   corr_ry = (0.45 + 0.02) / 1.1  = 0.47  / 1.1 = 0.427273
     *   corr_cx = (0.55 - 0.01) / 1.2  = 0.54  / 1.2 = 0.450000
     *   corr_cy = (0.425 + 0.02) / 1.1 = 0.445 / 1.1 = 0.404545 */
    static const char *want =
        "123456789,42,1000,4194303,0,0,"
        "0.500000,0.400000,0.600000,0.450000,0.550000,0.425000,"
        "0.560000,0.430000,3.210,4.560,"
        "0.408333,0.381818,0.491667,0.427273,0.450000,0.404545,"
        "-32.780,12.500,-675.250,32.560,13.125,-674.500\n";
    assert(strcmp(row, want) == 0);
}

/* The corrected columns must not be a copy of the raw ones. With a gain of
 * 1.2 the difference is large, and this is the single defect that would make
 * the whole trace silently useless to Task 16. */
static void test_the_corrected_columns_are_not_the_raw_ones(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 1) > 0);

    char *f[MAX_FIELDS];
    int n = split_row(row, f);
    assert(n == 28);
    for (int i = 0; i < 6; i++) assert(strcmp(f[6 + i], f[16 + i]) != 0);
}

/* The identity correction is the one case where corrected and raw agree, so it
 * pins that the corrected fields really come from the eye-specific points
 * rather than from the combined one. */
static void test_the_identity_correction_reproduces_each_point(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    c.gx = 1.0;
    c.gy = 1.0;
    c.bx = 0.0;
    c.by = 0.0;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 1) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    assert(strcmp(f[16], "0.500000") == 0);   /* corr_lx == lx */
    assert(strcmp(f[17], "0.400000") == 0);
    assert(strcmp(f[18], "0.600000") == 0);   /* corr_rx == rx */
    assert(strcmp(f[19], "0.450000") == 0);
    assert(strcmp(f[20], "0.550000") == 0);   /* corr_cx == combined_x */
    assert(strcmp(f[21], "0.425000") == 0);
}

/* ---------- what an invalid eye does ---------- */

static void test_an_invalid_left_eye_nans_only_its_own_columns(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    s.validity_L = 1;                 /* nonzero means INVALID */

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 7) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    assert(strcmp(f[16], "nan") == 0);        /* corr_lx */
    assert(strcmp(f[17], "nan") == 0);        /* corr_ly */
    assert(strcmp(f[18], "0.491667") == 0);   /* corr_rx, unaffected */
    assert(strcmp(f[19], "0.427273") == 0);
    /* One eye is enough for gz_gaze_correct, so the combined point survives. */
    assert(strcmp(f[20], "0.450000") == 0);
    assert(strcmp(f[21], "0.404545") == 0);
}

static void test_an_invalid_right_eye_nans_only_its_own_columns(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    s.validity_R = 3;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 7) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    assert(strcmp(f[16], "0.408333") == 0);
    assert(strcmp(f[17], "0.381818") == 0);
    assert(strcmp(f[18], "nan") == 0);
    assert(strcmp(f[19], "nan") == 0);
    assert(strcmp(f[20], "0.450000") == 0);
}

/* A frame with no eyes at all still has a gaze_point_2d_norm and it is not a
 * measurement of anything, which is why gz_gaze_correct refuses it. */
static void test_neither_eye_valid_nans_all_six(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    s.validity_L = 1;
    s.validity_R = 1;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 7) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    for (int i = 16; i < 22; i++) assert(strcmp(f[i], "nan") == 0);
}

/* The raw half is the device's own record and is written whatever validity
 * says, so an analysis can still see what the device claimed during a blink. */
static void test_the_raw_columns_survive_an_invalid_sample(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    s.validity_L = 1;
    s.validity_R = 1;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 7) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    assert(strcmp(f[4], "1") == 0);           /* validity_L, recorded as sent */
    assert(strcmp(f[5], "1") == 0);
    assert(strcmp(f[6], "0.500000") == 0);    /* lx */
    assert(strcmp(f[11], "0.425000") == 0);   /* combined_y */
    assert(strcmp(f[14], "3.210") == 0);      /* pupil_L */
}

/* ---------- no correction ---------- */

static void test_a_null_correction_nans_all_six(void) {
    struct gz_gaze_sample s;
    base_sample(&s);

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, NULL, 99) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    for (int i = 0; i < 16; i++) assert(strcmp(f[i], "nan") != 0);
    for (int i = 16; i < 22; i++) assert(strcmp(f[i], "nan") == 0);
    /* The eye origins are raw, so no correction is needed to write them. */
    for (int i = 22; i < 28; i++) assert(strcmp(f[i], "nan") != 0);
}

/* A struct that was zeroed and never loaded has valid == 0 and gains of 0.0.
 * Treating it as usable would divide by zero and write inf into the trace. */
static void test_an_unloaded_correction_is_treated_as_none(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    memset(&c, 0, sizeof c);

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 99) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    for (int i = 16; i < 22; i++) assert(strcmp(f[i], "nan") == 0);
}

/* ---------- the host clock ---------- */

static void test_the_host_stamp_is_written_as_given(void) {
    struct gz_gaze_sample s;
    base_sample(&s);

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, NULL, 18446744073709551615ULL) > 0);

    char *f[MAX_FIELDS];
    split_row(row, f);
    assert(strcmp(f[0], "18446744073709551615") == 0);
}

/* The device clock is signed in the wire struct, and printing it unsigned
 * would turn a negative stamp into 1.8e19 rather than an obvious error. */
static void test_a_negative_device_stamp_stays_negative(void) {
    struct gz_gaze_sample s;
    base_sample(&s);
    s.timestamp_us = -5;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, NULL, 1) > 0);

    char *f[MAX_FIELDS];
    split_row(row, f);
    assert(strcmp(f[1], "-5") == 0);
}

/* ---------- the buffer guard ---------- */

static void test_a_row_that_does_not_fit_is_refused(void) {
    struct gz_gaze_sample s;
    base_sample(&s);

    char row[16];
    memset(row, 'X', sizeof row);
    assert(gz_record_row(row, sizeof row, &s, NULL, 1) == 0);

    char one[1];
    assert(gz_record_row(one, sizeof one, &s, NULL, 1) == 0);
    assert(gz_record_row(row, 0, &s, NULL, 1) == 0);
}

/* A device field wide enough to blow the row buffer must be refused rather
 * than truncated, because a truncated line is a parse error in every row that
 * follows it. GZ_RECORD_ROW_MAX covers every plausible sample; this is the
 * implausible one. */
static void absurd_sample(struct gz_gaze_sample *s) {
    base_sample(s);
    /* %.6f of 1e300 is 308 characters, and there are eight such fields. */
    for (int i = 0; i < 2; i++) {
        s->gaze_point_2d_L_norm[i] = 1e300;
        s->gaze_point_2d_R_norm[i] = 1e300;
        s->gaze_point_2d_norm[i] = 1e300;
        s->gaze_point_2d_unfiltered[i] = 1e300;
    }
    for (int i = 0; i < 3; i++) {
        s->eye_origin_L_mm[i] = 1e300;
        s->eye_origin_R_mm[i] = 1e300;
    }
}

static void test_an_absurd_field_is_refused_rather_than_truncated(void) {
    struct gz_gaze_sample s;
    absurd_sample(&s);

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, NULL, 1) == 0);
}

/* The caller's cap is not the only bound. A buffer BIGGER than
 * GZ_RECORD_ROW_MAX would pass the cap check while the internal scratch had
 * already truncated, so the row has to be refused on its own length too. This
 * is the case where dropping that guard copies past the scratch buffer.
 *
 * The buffer has to clear the UNTRUNCATED row, not just GZ_RECORD_ROW_MAX. A
 * %.6f of 1e300 is 308 characters and there are eight of those fields, so the
 * row wants about 2500 and a 2048-byte buffer would hit the cap check first
 * and prove nothing. That is measured: at 4x the mutation survived. */
static void test_an_absurd_field_is_refused_with_room_to_spare(void) {
    struct gz_gaze_sample s;
    absurd_sample(&s);

    char big[16 * GZ_RECORD_ROW_MAX];
    memset(big, 'X', sizeof big);
    assert(gz_record_row(big, sizeof big, &s, NULL, 1) == 0);
    /* Refused means untouched, not half written. */
    for (size_t i = 0; i < sizeof big; i++) assert(big[i] == 'X');
}

/* The corrected fields are formatted into their own scratch before the row is
 * assembled, and snprintf truncates in silence. A corrected coordinate too
 * wide for that scratch has to sink the whole row, because a truncated
 * "8.5e299" written as its first 31 digits reads back as a real measurement
 * rather than as an error. */
static void test_an_absurd_corrected_value_is_refused(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    absurd_sample(&s);
    base_correction(&c);

    char big[4 * GZ_RECORD_ROW_MAX];
    memset(big, 'X', sizeof big);
    assert(gz_record_row(big, sizeof big, &s, &c, 1) == 0);
    for (size_t i = 0; i < sizeof big; i++) assert(big[i] == 'X');
}

/* A NaN out of the device prints as text float() reads back, not as an empty
 * field that shifts every later column left. */
static void test_a_nan_from_the_device_still_produces_a_full_row(void) {
    struct gz_gaze_sample s;
    base_sample(&s);
    s.gaze_point_2d_norm[0] = NAN;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, NULL, 1) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    assert(strcmp(f[10], "nan") == 0 || strcmp(f[10], "-nan") == 0);
}

/* ---------- the eye origins ---------- */

/* Form S is scoped per seating position and costs about
 * GZ_CORR_DEGRADE_PX_PER_MM per mm of head movement in the screen plane, so
 * where the head was is the one thing a later analysis can't recover from a
 * trace that omits it. Tracker millimetres, uncorrected, as the device sent
 * them. */
static void test_the_eye_origins_are_written_in_tracker_mm(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 1) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    assert(strcmp(f[22], "-32.780") == 0);    /* eye_lx */
    assert(strcmp(f[23], "12.500") == 0);     /* eye_ly */
    assert(strcmp(f[24], "-675.250") == 0);   /* eye_lz */
    assert(strcmp(f[25], "32.560") == 0);     /* eye_rx */
    assert(strcmp(f[26], "13.125") == 0);     /* eye_ry */
    assert(strcmp(f[27], "-674.500") == 0);   /* eye_rz */
}

/* The correction never touches them, so the same sample under two different
 * fits produces the same six numbers. A gain applied here would be silently
 * wrong, since these are millimetres rather than normalised coordinates. */
static void test_the_eye_origins_are_not_corrected(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);

    char withCorr[GZ_RECORD_ROW_MAX], withNone[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(withCorr, sizeof withCorr, &s, &c, 1) > 0);
    assert(gz_record_row(withNone, sizeof withNone, &s, NULL, 1) > 0);

    char *a[MAX_FIELDS], *b[MAX_FIELDS];
    assert(split_row(withCorr, a) == 28);
    assert(split_row(withNone, b) == 28);
    for (int i = 22; i < 28; i++) assert(strcmp(a[i], b[i]) == 0);
}

/* This firmware writes a plain 0.0 into the eye-origin fields on a frame with
 * no eyes. That zero is recorded as 0.000 rather than as nan, because
 * validity_L and validity_R already say which eye is real and substituting nan
 * would lose the difference between "no eye" and "a value we declined to
 * write". */
static void test_an_eyeless_frame_writes_zero_not_nan(void) {
    struct gz_gaze_sample s;
    base_sample(&s);
    s.validity_L = 1;
    s.validity_R = 1;
    for (int i = 0; i < 3; i++) {
        s.eye_origin_L_mm[i] = 0.0;
        s.eye_origin_R_mm[i] = 0.0;
    }

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, NULL, 1) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    for (int i = 22; i < 28; i++) assert(strcmp(f[i], "0.000") == 0);
}

/* ---------- the zero-gain gate ---------- */

/* gz_correct_point divides without checking, so before this the per-eye path
 * and the combined one disagreed: a correction with valid = 1 and gx = 0 wrote
 * inf into corr_lx while gz_gaze_correct's own zero-gain test (proto.c:408)
 * made corr_cx come out nan. One row cannot say both. */
/* gz_correction_parse writes the parsed numbers out with valid left at 0 when
 * it returns GZ_CORR_PARSE_BOUNDS, so a file the loader refused reaches a
 * caller as a struct with real-looking gains. The valid flag is the only thing
 * standing between that and a corrected column nobody accepted. */
static void test_a_parsed_but_refused_correction_is_not_applied(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    c.valid = 0;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 1) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    for (int i = 16; i < 22; i++) assert(strcmp(f[i], "nan") == 0);
}

static void test_a_zero_gain_nans_every_corrected_column(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    c.gx = 0.0;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 1) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    for (int i = 16; i < 22; i++) assert(strcmp(f[i], "nan") == 0);
}

static void test_a_zero_y_gain_nans_every_corrected_column(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    c.gy = 0.0;

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 1) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    for (int i = 16; i < 22; i++) assert(strcmp(f[i], "nan") == 0);
}

/* The formatter applies what it is given and does not re-run the loader's
 * bounds. base_correction is not isotropic to GZ_CORR_ISO_TOL and so fails
 * gz_correction_check, yet its numbers are still written: deciding which files
 * may be used is gz_correction_load's job, and doing it twice would silently
 * nan a fit the loader had already accepted. */
static void test_the_formatter_does_not_re_apply_the_loaders_bounds(void) {
    struct gz_gaze_sample s;
    struct gz_correction c;
    base_sample(&s);
    base_correction(&c);
    assert(!gz_correction_check(&c));

    char row[GZ_RECORD_ROW_MAX];
    assert(gz_record_row(row, sizeof row, &s, &c, 1) > 0);

    char *f[MAX_FIELDS];
    assert(split_row(row, f) == 28);
    assert(strcmp(f[16], "0.408333") == 0);
    assert(strcmp(f[20], "0.450000") == 0);
}

/* ---------- provenance beside the trace ---------- */

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fputs(text, f) != EOF);
    assert(fclose(f) == 0);
}

static int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static void test_the_provenance_path_sits_beside_the_trace(void) {
    char buf[64];
    assert(gz_record_provenance_path(buf, sizeof buf, "/tmp/t.csv") == 0);
    assert(strcmp(buf, "/tmp/t.csv.correction.conf") == 0);

    /* Refused rather than truncated, since a truncated name would remove or
     * write the wrong file. */
    char tight[10];
    assert(gz_record_provenance_path(tight, sizeof tight, "/tmp/t.csv") == -1);
    assert(gz_record_provenance_path(buf, 0, "/tmp/t.csv") == -1);
}

/* The Important finding of review round 1. A --raw recording over a path that
 * once held a corrected trace left the old correction file in place, so an
 * uncorrected trace sat beside a file claiming a fit produced it, which is
 * exactly what traces/README.md tells the reader cannot happen. */
static void test_clearing_removes_a_stale_provenance_file(void) {
    char trace[256], prov[300];
    snprintf(trace, sizeof trace, "/tmp/gz_rec_%d_stale.csv", (int)getpid());
    assert(gz_record_provenance_path(prov, sizeof prov, trace) == 0);

    write_file(prov, "version=2\nform=1\n");
    assert(file_exists(prov));

    assert(gz_record_clear_provenance(trace) == 0);
    assert(!file_exists(prov));

    remove(trace);
}

/* Absent is the normal case, not a failure: most recordings go to a fresh
 * path and must not be refused for it. */
static void test_clearing_a_path_with_no_provenance_succeeds(void) {
    char trace[256], prov[300];
    snprintf(trace, sizeof trace, "/tmp/gz_rec_%d_absent.csv", (int)getpid());
    assert(gz_record_provenance_path(prov, sizeof prov, trace) == 0);
    remove(prov);
    assert(!file_exists(prov));

    assert(gz_record_clear_provenance(trace) == 0);
    assert(gz_record_clear_provenance(trace) == 0);
    assert(!file_exists(prov));
}

/* It removes the file beside THIS trace and nothing else. A clear that walked
 * a directory would take out a neighbouring trace's provenance. */
static void test_clearing_leaves_a_neighbours_provenance_alone(void) {
    char mine[256], other[256], mineProv[300], otherProv[300];
    snprintf(mine, sizeof mine, "/tmp/gz_rec_%d_a.csv", (int)getpid());
    snprintf(other, sizeof other, "/tmp/gz_rec_%d_b.csv", (int)getpid());
    assert(gz_record_provenance_path(mineProv, sizeof mineProv, mine) == 0);
    assert(gz_record_provenance_path(otherProv, sizeof otherProv, other) == 0);

    write_file(mineProv, "mine\n");
    write_file(otherProv, "theirs\n");

    assert(gz_record_clear_provenance(mine) == 0);
    assert(!file_exists(mineProv));
    assert(file_exists(otherProv));

    remove(otherProv);
}

/* A name too long to build is refused rather than acted on, so nothing is
 * removed on a guess. */
static void test_clearing_refuses_an_unbuildable_name(void) {
    char huge[900];
    memset(huge, 'x', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';
    assert(gz_record_clear_provenance(huge) == -1);
}

/* ---------- the refusal decision ---------- */

/* A trace of raw device output cannot be used to fit the overlay's filter, so
 * a missing or unusable correction stops the recording rather than producing a
 * file that looks fine and is not. --raw is the deliberate escape hatch. */
static void test_a_missing_correction_refuses_the_recording(void) {
    assert(gz_record_decide(0, 0) == GZ_REC_REFUSE);
}

static void test_an_unusable_correction_refuses_the_recording(void) {
    assert(gz_record_decide(-1, 0) == GZ_REC_REFUSE);
}

static void test_a_loaded_correction_records_corrected(void) {
    assert(gz_record_decide(1, 0) == GZ_REC_CORRECTED);
}

static void test_raw_overrides_every_load_result(void) {
    assert(gz_record_decide(0, 1) == GZ_REC_RAW);
    assert(gz_record_decide(-1, 1) == GZ_REC_RAW);
    assert(gz_record_decide(1, 1) == GZ_REC_RAW);
}

int main(void) {
    test_header_names_the_brief_columns_then_the_corrected_six();
    test_a_row_has_as_many_fields_as_the_header();

    test_a_valid_sample_yields_the_hand_computed_row();
    test_the_corrected_columns_are_not_the_raw_ones();
    test_the_identity_correction_reproduces_each_point();

    test_an_invalid_left_eye_nans_only_its_own_columns();
    test_an_invalid_right_eye_nans_only_its_own_columns();
    test_neither_eye_valid_nans_all_six();
    test_the_raw_columns_survive_an_invalid_sample();

    test_a_null_correction_nans_all_six();
    test_an_unloaded_correction_is_treated_as_none();

    test_the_host_stamp_is_written_as_given();
    test_a_negative_device_stamp_stays_negative();

    test_a_row_that_does_not_fit_is_refused();
    test_an_absurd_field_is_refused_rather_than_truncated();
    test_an_absurd_field_is_refused_with_room_to_spare();
    test_an_absurd_corrected_value_is_refused();
    test_a_nan_from_the_device_still_produces_a_full_row();

    test_the_eye_origins_are_written_in_tracker_mm();
    test_the_eye_origins_are_not_corrected();
    test_an_eyeless_frame_writes_zero_not_nan();

    test_a_parsed_but_refused_correction_is_not_applied();
    test_a_zero_gain_nans_every_corrected_column();
    test_a_zero_y_gain_nans_every_corrected_column();
    test_the_formatter_does_not_re_apply_the_loaders_bounds();

    test_the_provenance_path_sits_beside_the_trace();
    test_clearing_removes_a_stale_provenance_file();
    test_clearing_a_path_with_no_provenance_succeeds();
    test_clearing_leaves_a_neighbours_provenance_alone();
    test_clearing_refuses_an_unbuildable_name();

    test_a_missing_correction_refuses_the_recording();
    test_an_unusable_correction_refuses_the_recording();
    test_a_loaded_correction_records_corrected();
    test_raw_overrides_every_load_result();

    printf("all record tests passed\n");
    return 0;
}
