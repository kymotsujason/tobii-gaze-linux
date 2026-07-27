/* gaze-cal/src/proto.c - see proto.h. No allocation, no I/O, no CLI deps.
 *
 * The gate that refuses a wrong display area prints, and printing is I/O, so
 * the decision lives here as a pure predicate and the printing lives in
 * display.c. That keeps stdio out of Plan 2's OBS filter plugin, which links
 * this file. */
#include <math.h>
#include <string.h>
#include "proto.h"

/* -std=c11 defines __STRICT_ANSI__, which stops glibc from enabling
 * _DEFAULT_SOURCE, so math.h does not declare M_PI. Spelled out rather than
 * reached for with a feature-test macro, because proto.h is also included by a
 * C++ plugin whose own feature macros are not ours to set. */
#define GZ_DEG_PER_RAD 57.29577951308232087680

/* daemon_protocol.zig writes the header length with std.mem.writeInt(.little),
 * so it is little endian by specification and not by host coincidence. The
 * gaze payload is the opposite case: encodeGaze dumps the struct with
 * std.mem.asBytes, so it carries daemon host order, and daemon and client are
 * the same machine by construction (it is a unix socket). Each field is
 * therefore decoded exactly the way it was encoded. */
static uint32_t rd_u32le(const unsigned char *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void wr_u32le(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* The only desync detector this parser has. A type whose shape is fixed gets
 * an exact length; a known type whose shape is not defined by the daemon gets
 * a bounded one; anything else is rejected so a resynced-onto-garbage stream
 * is caught instead of consumed. */
static int expected_len_ok(uint8_t type, uint32_t len) {
    switch (type) {
    case GZ_SRV_GAZE:         return len == sizeof(struct gz_gaze_sample);
    case GZ_SRV_ERR:          return len == 4;

    /* >= GZ_STATUS_SIZE, not ==, and gz_frame_status reads the version at
     * payload offset 2. That pins the one field a future shape change must not
     * move: a daemon that appends to status stays readable, and the client
     * learns to stop from the version rather than from a desync it cannot tell
     * apart from stream corruption. */
    case GZ_SRV_STATUS:       return len >= GZ_STATUS_SIZE;

    /* encodeResponse always writes the cmd_type byte, so len 0 is impossible.
     * The daemon's own ceilings are 1 + 8192 (main.zig MAX_RESPONSE_PAYLOAD)
     * and 1 + 4096 (sendResult), both well under GZ_MAX_PAYLOAD. */
    case GZ_SRV_RESPONSE:     return len >= 1;

    /* Srv.display_area exists in the Zig enum but the daemon never emits it.
     * Measured against a live daemon on 2026-07-26: get_display_area comes
     * back as a response (0x02) with cmd_type 0x02 and a 164-byte raw TTP
     * body, and zero 0x03 frames arrive in 8 s of streaming. So 0x03 has no
     * defined shape here and there is no length to check. The frame is
     * consumed and handed over for the caller to ignore, rather than hard
     * failing a client that a future daemon starts sending it to. */
    case GZ_SRV_DISPLAY_AREA: return 1;

    default:                  return 0;
    }
}

int gz_frame_parse(const unsigned char *buf, size_t len, struct gz_frame *out) {
    if (len < GZ_HEADER_SIZE) return 0;

    uint8_t type = buf[0];
    uint32_t plen = rd_u32le(buf + 1);

    /* Bound before any arithmetic. plen is attacker-influenced after a desync,
     * and GZ_HEADER_SIZE + plen is evaluated in 32-bit unsigned unless plen is
     * widened first, so 0xFFFFFFFB would wrap to 0 and report a complete frame
     * that consumes nothing. Both guards are kept: the bound makes the wrap
     * unreachable, the widening makes it harmless if the bound ever moves. */
    if (plen > GZ_MAX_PAYLOAD) return GZ_ERR_DESYNC;
    if (!expected_len_ok(type, plen)) return GZ_ERR_DESYNC;

    size_t total = (size_t)GZ_HEADER_SIZE + (size_t)plen;
    if (len < total) return 0;

    out->type = type;
    if (type == GZ_SRV_RESPONSE) {
        out->cmd_type = buf[GZ_HEADER_SIZE];
        out->body     = buf + GZ_HEADER_SIZE + 1;
        out->body_len = (size_t)plen - 1;
    } else {
        /* Cleared rather than left alone, so a caller reusing one gz_frame
         * across a stream cannot read a previous response's cmd_type. */
        out->cmd_type = 0;
        out->body     = buf + GZ_HEADER_SIZE;
        out->body_len = plen;
    }
    return (int)total;
}

size_t gz_encode_cmd(unsigned char *buf, size_t cap, uint8_t cmd,
                     const void *payload, size_t payload_len) {
    /* Checked first and against a constant, so neither the cap comparison nor
     * the u32 length field can wrap: cap < GZ_HEADER_SIZE + payload_len is
     * false for payload_len near SIZE_MAX, which would admit a SIZE_MAX
     * memcpy, and (uint32_t)payload_len would silently truncate above 4 GiB. */
    if (payload_len > GZ_MAX_PAYLOAD) return 0;
    if (payload_len > 0 && payload == NULL) return 0;
    if (cap < (size_t)GZ_HEADER_SIZE + payload_len) return 0;

    buf[0] = cmd;
    wr_u32le(buf + 1, (uint32_t)payload_len);
    if (payload_len > 0) memcpy(buf + GZ_HEADER_SIZE, payload, payload_len);
    return (size_t)GZ_HEADER_SIZE + payload_len;
}

int gz_frame_status(const struct gz_frame *f, struct gz_status *out) {
    if (f->type != GZ_SRV_STATUS || f->body_len < GZ_STATUS_SIZE) return 0;
    out->device_present      = f->body[0];
    out->calibration_applied = f->body[1];
    out->protocol_version    = f->body[2];   /* the field that must not move */
    return 1;
}

int gz_frame_err(const struct gz_frame *f, uint32_t *out_code) {
    if (f->type != GZ_SRV_ERR || f->body_len < 4) return 0;
    *out_code = rd_u32le(f->body);
    return 1;
}

int gz_frame_gaze(const struct gz_frame *f, struct gz_gaze_sample *out) {
    if (f->type != GZ_SRV_GAZE || f->body_len != sizeof *out) return 0;
    /* Copied, not cast. body is buf + 5, so it is 8-byte aligned only by
     * accident, and reading f64 members through a misaligned pointer is
     * undefined even where x86-64 tolerates it. */
    memcpy(out, f->body, sizeof *out);
    return 1;
}

int gz_err_retryable(uint32_t code) {
    return code == GZ_ERRCODE_USB_BUSY;
}

int gz_eye_valid(uint32_t validity) {
    return validity == GZ_VALIDITY_VALID;
}

int gz_sample_both_eyes_valid(const struct gz_gaze_sample *s) {
    return gz_eye_valid(s->validity_L) && gz_eye_valid(s->validity_R);
}

int gz_sample_any_eye_valid(const struct gz_gaze_sample *s) {
    return gz_eye_valid(s->validity_L) || gz_eye_valid(s->validity_R);
}

uint32_t gz_frames_dropped(uint32_t prev_counter, uint32_t next_counter) {
    /* Unsigned subtraction wraps by definition, so the counter rolling over
     * 2^32 costs nothing. A delta under one step is a duplicate or a
     * reordering, not a gap. */
    uint32_t delta = next_counter - prev_counter;
    if (delta < GZ_FRAME_COUNTER_STEP) return 0;
    return delta / GZ_FRAME_COUNTER_STEP - 1;
}

/* ---------------- TTP TLV ----------------
 *
 * Big endian here, unlike every length in the daemon framing above. The TLV
 * bytes are the device's own, forwarded untouched, and the device is big
 * endian on the wire. Mixing the two readers is the easy mistake. */
static uint32_t rd_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         |  (uint32_t)p[3];
}

static uint64_t rd_u64be(const unsigned char *p) {
    return ((uint64_t)rd_u32be(p) << 32) | (uint64_t)rd_u32be(p + 4);
}

/* Converting a uint64_t above INT64_MAX to int64_t is implementation-defined,
 * and -fsanitize=undefined is entitled to say so. This is the well-defined
 * spelling: ~v is at most INT64_MAX whenever v is not. */
static int64_t as_i64(uint64_t v) {
    if (v <= (uint64_t)INT64_MAX) return (int64_t)v;
    return -(int64_t)(~v) - 1;
}

void gz_tlv_init(struct gz_tlv *r, const unsigned char *buf, size_t len) {
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

/* struct gz_tlv is public and pos is writable, so len - pos is not safe to
 * compute blind: a caller that seeks past the end would wrap size_t and every
 * bounds check below would then read as "plenty of room left". */
static size_t tlv_avail(const struct gz_tlv *r) {
    return r->pos <= r->len ? r->len - r->pos : 0;
}

/* Reads [type][u32 BE size] and checks both against what the caller expects.
 * The size is validated even though it is implied by the type, because that is
 * the only thing distinguishing a real field from a byte pattern of the right
 * length that happens to start with the right type byte. */
static int tlv_header(struct gz_tlv *r, uint8_t want_type, uint32_t want_size) {
    if (tlv_avail(r) < 5) return 0;
    if (r->buf[r->pos] != want_type) return 0;
    if (rd_u32be(r->buf + r->pos + 1) != want_size) return 0;
    r->pos += 5;
    return 1;
}

int gz_tlv_read_prolog_tag(struct gz_tlv *r, uint32_t *out_tag) {
    if (!tlv_header(r, GZ_TLV_TYPE_PROLOG, 4)) return 0;
    if (tlv_avail(r) < 4) return 0;
    *out_tag = rd_u32be(r->buf + r->pos);
    r->pos += 4;
    return 1;
}

int gz_tlv_read_u32(struct gz_tlv *r, uint32_t *out) {
    if (!tlv_header(r, GZ_TLV_TYPE_U32, 4)) return 0;
    if (tlv_avail(r) < 4) return 0;
    *out = rd_u32be(r->buf + r->pos);
    r->pos += 4;
    return 1;
}

int gz_tlv_read_q42(struct gz_tlv *r, double *out) {
    if (!tlv_header(r, GZ_TLV_TYPE_Q42, 8)) return 0;
    if (tlv_avail(r) < 8) return 0;
    *out = (double)as_i64(rd_u64be(r->buf + r->pos)) / GZ_Q42_SCALE;
    r->pos += 8;
    return 1;
}

int gz_tlv_read_point3d(struct gz_tlv *r, double out[3]) {
    uint32_t tag;
    if (!gz_tlv_read_prolog_tag(r, &tag)) return 0;
    if (tag != GZ_TLV_TAG_POINT3D) return 0;
    /* Into a local first: a partial point must not leave two decoded
     * coordinates and one stale one in the caller's array. */
    double v[3];
    for (int i = 0; i < 3; i++) {
        if (!gz_tlv_read_q42(r, &v[i])) return 0;
    }
    memcpy(out, v, sizeof v);
    return 1;
}

int gz_decode_display_area(const unsigned char *body, size_t len, double out[9]) {
    if (body == NULL || len < GZ_DA_PROLOG_SIZE) return 0;

    struct gz_tlv r;
    gz_tlv_init(&r, body, len);
    r.pos = GZ_DA_PROLOG_SIZE;

    double v[9];
    for (int i = 0; i < 3; i++) {
        if (!gz_tlv_read_point3d(&r, v + i * 3)) return 0;
    }
    /* The 0x010100 trailer is not read. decode_display_area does not read it
     * either, and a device that stopped emitting it would still be sending a
     * complete display area. */
    memcpy(out, v, sizeof v);
    return 1;
}

/* ---------------- display geometry ----------------
 *
 * Exact inverse of Tracker.setDisplayArea, which builds
 *   bl = (ox, oy, z)
 *   tl = (ox, oy + h cos t, z + h sin t)
 *   tr = (ox + w, tl.y, tl.z)
 * so h is the LENGTH of the TL-BL edge, not its y projection, z is the BOTTOM
 * edge's z, and the tilt runs from bottom to top. Reading h off the y axis,
 * taking z from tl, or flipping the atan2 arguments all agree at tilt 0 and
 * all disagree the moment the panel is tilted, which is exactly the case the
 * config's unmeasured tilt makes reachable. */
/* The three constraints setDisplayArea's construction implies, and the only
 * ones: tr.x is free because it carries the width, and bl.y and bl.z are free
 * because they carry the origin. Same !(x <= tol) spelling as gz_rect_diff, so
 * a NaN corner is not rectangular. */
int gz_corners_are_rectangular(const double c[9], double tol_mm) {
    if (!(fabs(c[0] - c[6]) <= tol_mm)) return 0;   /* tl.x vs bl.x */
    if (!(fabs(c[4] - c[1]) <= tol_mm)) return 0;   /* tr.y vs tl.y */
    if (!(fabs(c[5] - c[2]) <= tol_mm)) return 0;   /* tr.z vs tl.z */
    return 1;
}

struct gz_rect gz_corners_to_rect(const double c[9]) {
    struct gz_rect r;
    double dy = c[1] - c[7];        /* tl.y - bl.y */
    double dz = c[2] - c[8];        /* tl.z - bl.z */

    r.w_mm     = fabs(c[3] - c[0]); /* tr.x - tl.x */
    r.h_mm     = hypot(dy, dz);
    r.ox_mm    = c[6];              /* bl.x */
    r.oy_mm    = c[7];              /* bl.y */
    r.z_mm     = c[8];              /* bl.z */
    r.tilt_deg = atan2(dz, dy) * GZ_DEG_PER_RAD;
    return r;
}

void gz_rect_to_corners(struct gz_rect r, double out[9]) {
    double a = r.tilt_deg / GZ_DEG_PER_RAD;
    double tl_y = r.oy_mm + r.h_mm * cos(a);
    double tl_z = r.z_mm  + r.h_mm * sin(a);

    out[0] = r.ox_mm;            out[1] = tl_y; out[2] = tl_z;   /* tl */
    out[3] = r.ox_mm + r.w_mm;   out[4] = tl_y; out[5] = tl_z;   /* tr */
    out[6] = r.ox_mm;            out[7] = r.oy_mm; out[8] = r.z_mm; /* bl */
}

/* Written as !(diff <= tol) rather than (diff > tol) so that a NaN reports a
 * mismatch. Both spellings agree on every real number and disagree on NaN,
 * where the second one would call an undecidable comparison a match, and a
 * gate that passes on garbage is worse than no gate. */
unsigned gz_rect_diff(struct gz_rect got, struct gz_rect want,
                      double tol_mm, double tol_deg) {
    unsigned d = 0;
    if (!(fabs(got.w_mm     - want.w_mm)     <= tol_mm))  d |= GZ_DA_DIFF_W;
    if (!(fabs(got.h_mm     - want.h_mm)     <= tol_mm))  d |= GZ_DA_DIFF_H;
    if (!(fabs(got.ox_mm    - want.ox_mm)    <= tol_mm))  d |= GZ_DA_DIFF_OX;
    if (!(fabs(got.oy_mm    - want.oy_mm)    <= tol_mm))  d |= GZ_DA_DIFF_OY;
    if (!(fabs(got.z_mm     - want.z_mm)     <= tol_mm))  d |= GZ_DA_DIFF_Z;
    if (!(fabs(got.tilt_deg - want.tilt_deg) <= tol_deg)) d |= GZ_DA_DIFF_TILT;
    return d;
}

/* ---------------- host-side gaze correction ---------------- */

void gz_eye_state_init(struct gz_eye_state *e) {
    e->lr_mm[0] = e->lr_mm[1] = e->lr_mm[2] = 0.0;
    e->have_lr = 0;
}

int gz_sample_eye_mid(struct gz_eye_state *e, const struct gz_gaze_sample *s,
                      double out[3]) {
    int have_l = gz_eye_valid(s->validity_L);
    int have_r = gz_eye_valid(s->validity_R);

    /* Into a local first, so a refusal cannot leave the caller with one
     * coordinate from this frame and two from the last one. */
    double v[3];

    if (have_l && have_r) {
        for (int i = 0; i < 3; i++) {
            e->lr_mm[i] = s->eye_origin_R_mm[i] - s->eye_origin_L_mm[i];
            v[i] = (s->eye_origin_L_mm[i] + s->eye_origin_R_mm[i]) * 0.5;
        }
        e->have_lr = 1;
    } else if (!e->have_lr) {
        /* Nothing to reconstruct from yet. Refusing costs one frame at 33.2 Hz;
         * guessing an IPD would cost a lateral error for the whole session. */
        return 0;
    } else if (have_l) {
        for (int i = 0; i < 3; i++) v[i] = s->eye_origin_L_mm[i] + e->lr_mm[i] * 0.5;
    } else if (have_r) {
        for (int i = 0; i < 3; i++) v[i] = s->eye_origin_R_mm[i] - e->lr_mm[i] * 0.5;
    } else {
        return 0;                       /* neither eye: no usable gaze either */
    }

    memcpy(out, v, sizeof v);
    return 1;
}

void gz_eye_proj(struct gz_rect area, const double eye_mm[3], double out[2]) {
    out[0] = (eye_mm[0] - area.ox_mm) / area.w_mm;
    out[1] = (area.oy_mm + area.h_mm - eye_mm[1]) / area.h_mm;
}

int gz_correction_check(const struct gz_correction *c) {
    /* !(x) rather than (!x) throughout, so a NaN parameter fails every test
     * instead of passing the ones phrased as a negation. */
    if (!(c->gx >= GZ_CORR_G_MIN) || !(c->gx <= GZ_CORR_G_MAX)) return 0;
    if (!(c->gy >= GZ_CORR_G_MIN) || !(c->gy <= GZ_CORR_G_MAX)) return 0;
    if (!(fabs(c->gx / c->gy - 1.0) < GZ_CORR_ISO_TOL)) return 0;
    if (!(fabs(c->bx) < 1.0) || !(fabs(c->by) < 1.0)) return 0;
    if (!(c->area.w_mm > 0.0) || !(c->area.h_mm > 0.0)) return 0;
    return 1;
}

/* The spec writes this as E + (r - E - b)/g. Multiplied out it is
 * (r + E*(g-1) - b)/g, which is algebraically the same and better in two ways:
 * it is exact at the identity g = 1, b = 0, where the first form subtracts E
 * and adds it back and need not land on r; and it avoids cancelling r against E
 * when the eye happens to project near the gaze point. Only a reported
 * coordinate of exactly -0.0 fails to round-trip, coming back as +0.0.
 *
 * One spelling, called by both the live path and the offline scoring in
 * calibrate.c, so the two cannot drift apart. */
void gz_correct_point(const struct gz_correction *c, const double eye_proj[2],
                      const double reported[2], double out[2]) {
    double v[2];
    v[0] = (reported[0] + eye_proj[0] * (c->gx - 1.0) - c->bx) / c->gx;
    v[1] = (reported[1] + eye_proj[1] * (c->gy - 1.0) - c->by) / c->gy;
    memcpy(out, v, sizeof v);
}

int gz_gaze_correct(const struct gz_correction *c, struct gz_eye_state *e,
                    const struct gz_gaze_sample *s, double out[2]) {
    /* Both refusals before gz_sample_eye_mid, which writes to e: a sample that
     * cannot be corrected should not silently arm the reconstruction either. */
    if (!c->valid) return 0;
    if (!(c->gx != 0.0) || !(c->gy != 0.0)) return 0;

    double eye[3];
    if (!gz_sample_eye_mid(e, s, eye)) return 0;

    double ep[2];
    gz_eye_proj(c->area, eye, ep);
    gz_correct_point(c, ep, s->gaze_point_2d_norm, out);
    return 1;
}

/* ---------------- locale-independent number text ----------------
 *
 * See the note in proto.h. Nine decimals is far past anything these parameters
 * carry (a gain's ninth decimal is 3e-6 px) and it keeps every value this file
 * holds inside int64 fixed point, so no exponent form is ever produced. */
#define GZ_NUM_DECIMALS 9
#define GZ_NUM_SCALE    1000000000LL
#define GZ_NUM_MAX      1000000000.0

static size_t fmt_num(char *buf, size_t cap, double v) {
    /* Spelled as a positive test so NaN, which loses every comparison, is
     * refused here rather than printed as some platform's idea of "nan". */
    if (!(v > -GZ_NUM_MAX && v < GZ_NUM_MAX)) return 0;

    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }

    int64_t units = (int64_t)(v * (double)GZ_NUM_SCALE + 0.5);
    int64_t ip = units / GZ_NUM_SCALE;
    int64_t fp = units % GZ_NUM_SCALE;

    char frac[GZ_NUM_DECIMALS];
    for (int i = GZ_NUM_DECIMALS - 1; i >= 0; i--) {
        frac[i] = (char)('0' + (int)(fp % 10));
        fp /= 10;
    }
    int nfrac = GZ_NUM_DECIMALS;
    while (nfrac > 0 && frac[nfrac - 1] == '0') nfrac--;

    char intd[24];
    int nint = 0;
    if (ip == 0) intd[nint++] = '0';
    while (ip > 0) { intd[nint++] = (char)('0' + (int)(ip % 10)); ip /= 10; }

    size_t need = (size_t)neg + (size_t)nint + (nfrac > 0 ? 1u + (size_t)nfrac : 0u);
    if (cap < need + 1) return 0;

    size_t n = 0;
    if (neg) buf[n++] = '-';
    for (int i = nint - 1; i >= 0; i--) buf[n++] = intd[i];
    if (nfrac > 0) {
        buf[n++] = '.';
        for (int i = 0; i < nfrac; i++) buf[n++] = frac[i];
    }
    buf[n] = '\0';
    return n;
}

static int is_digit(char ch) { return ch >= '0' && ch <= '9'; }

/* Binary exponentiation, non-negative exponents only. Every power of ten this
 * file can produce is under 10^22, the largest one a double holds exactly, so
 * the result is exact. */
static double pow10i(int e) {
    double r = 1.0, b = 10.0;
    while (e > 0) {
        if (e & 1) r *= b;
        b *= b;
        e >>= 1;
    }
    return r;
}

/* [+-]?digits[.digits][(e|E)[+-]?digits]. Returns 1 and advances *pp past the
 * number, or 0 without touching *out. */
static int parse_num(const char **pp, const char *end, double *out) {
    const char *p = *pp;
    int neg = 0;

    if (p < end && (*p == '+' || *p == '-')) { neg = (*p == '-'); p++; }

    double mant = 0.0;
    int seen = 0, nfrac = 0;
    while (p < end && is_digit(*p)) { mant = mant * 10.0 + (*p - '0'); p++; seen++; }
    if (p < end && *p == '.') {
        p++;
        while (p < end && is_digit(*p)) {
            mant = mant * 10.0 + (*p - '0');
            p++; seen++; nfrac++;
        }
    }
    if (seen == 0) return 0;

    int ex = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *save = p;
        p++;
        int eneg = 0;
        if (p < end && (*p == '+' || *p == '-')) { eneg = (*p == '-'); p++; }
        int edig = 0;
        while (p < end && is_digit(*p)) {
            /* Clamped rather than overflowed: a exponent past this is going to
             * be refused by the bounds check anyway, and signed overflow is
             * undefined. */
            if (ex < 100000) ex = ex * 10 + (*p - '0');
            p++; edig++;
        }
        if (edig == 0) { p = save; ex = 0; }     /* a trailing 'e' is not an exponent */
        else if (eneg) ex = -ex;
    }

    /* Divided rather than multiplied by a reciprocal. 12 * 0.1 is
     * 1.2000000000000002 because 0.1 is not representable, while 12 / 10 is the
     * correctly rounded 1.2, so this is what makes the text round trip. */
    int e10 = ex - nfrac;
    double v = e10 >= 0 ? mant * pow10i(e10) : mant / pow10i(-e10);
    *out = neg ? -v : v;
    *pp = p;
    return 1;
}

size_t gz_correction_format(const struct gz_correction *c, char *buf, size_t cap) {
    static const char *const keys[] = {
        "version", "gx", "gy", "bx", "by",
        "area_w_mm", "area_h_mm", "area_ox_mm", "area_oy_mm",
        "area_z_mm", "area_tilt_deg"
    };
    const double vals[] = {
        (double)GZ_CORRECTION_VERSION, c->gx, c->gy, c->bx, c->by,
        c->area.w_mm, c->area.h_mm, c->area.ox_mm, c->area.oy_mm,
        c->area.z_mm, c->area.tilt_deg
    };
    const size_t nkeys = sizeof vals / sizeof vals[0];

    size_t n = 0;
    for (size_t k = 0; k < nkeys; k++) {
        for (const char *q = keys[k]; *q != '\0'; q++) {
            if (n + 1 >= cap) return 0;
            buf[n++] = *q;
        }
        if (n + 1 >= cap) return 0;
        buf[n++] = '=';

        size_t w = fmt_num(buf + n, cap - n, vals[k]);
        if (w == 0) return 0;
        n += w;

        if (n + 1 >= cap) return 0;
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    return n;
}

/* Every key this format models, in the order the bitmask below counts them. */
enum {
    CK_VERSION, CK_GX, CK_GY, CK_BX, CK_BY,
    CK_W, CK_H, CK_OX, CK_OY, CK_Z, CK_TILT, CK_COUNT
};

static int key_is(const char *k, size_t klen, const char *want) {
    size_t i = 0;
    while (i < klen && want[i] != '\0') {
        if (k[i] != want[i]) return 0;
        i++;
    }
    return i == klen && want[i] == '\0';
}

static int key_index(const char *k, size_t klen) {
    static const char *const names[CK_COUNT] = {
        "version", "gx", "gy", "bx", "by",
        "area_w_mm", "area_h_mm", "area_ox_mm", "area_oy_mm",
        "area_z_mm", "area_tilt_deg"
    };
    for (int i = 0; i < CK_COUNT; i++) {
        if (key_is(k, klen, names[i])) return i;
    }
    return -1;
}

static int is_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

int gz_correction_parse(const char *buf, size_t len, struct gz_correction *out) {
    if (buf == NULL) return GZ_CORR_PARSE_MALFORMED;

    struct gz_correction c;
    memset(&c, 0, sizeof c);
    unsigned seen = 0;
    double version = 0.0;

    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && buf[i] != '\n') i++;
        size_t le = i;
        if (i < len) i++;                      /* step over the newline */

        while (ls < le && is_space(buf[ls])) ls++;
        while (le > ls && is_space(buf[le - 1])) le--;
        if (ls == le || buf[ls] == '#') continue;

        size_t eq = ls;
        while (eq < le && buf[eq] != '=') eq++;
        if (eq == le) return GZ_CORR_PARSE_MALFORMED;   /* a line that is not key=value */

        size_t ke = eq;
        while (ke > ls && is_space(buf[ke - 1])) ke--;
        int idx = key_index(buf + ls, ke - ls);

        size_t vs = eq + 1;
        while (vs < le && is_space(buf[vs])) vs++;

        if (idx < 0) continue;                 /* provenance the CLI appended */

        const char *p = buf + vs;
        double v = 0.0;
        if (!parse_num(&p, buf + le, &v)) return GZ_CORR_PARSE_MALFORMED;
        while (p < buf + le && is_space(*p)) p++;
        if (p != buf + le) return GZ_CORR_PARSE_MALFORMED;  /* trailing junk */

        /* A repeated key is a malformed file rather than a last-one-wins
         * override: two gx lines mean nobody knows which gain is in force. */
        if (seen & (1u << idx)) return GZ_CORR_PARSE_MALFORMED;
        seen |= 1u << idx;

        switch (idx) {
        case CK_VERSION: version       = v; break;
        case CK_GX:      c.gx          = v; break;
        case CK_GY:      c.gy          = v; break;
        case CK_BX:      c.bx          = v; break;
        case CK_BY:      c.by          = v; break;
        case CK_W:       c.area.w_mm   = v; break;
        case CK_H:       c.area.h_mm   = v; break;
        case CK_OX:      c.area.ox_mm  = v; break;
        case CK_OY:      c.area.oy_mm  = v; break;
        case CK_Z:       c.area.z_mm   = v; break;
        case CK_TILT:    c.area.tilt_deg = v; break;
        default: break;
        }
    }

    if (seen != (1u << CK_COUNT) - 1u) return GZ_CORR_PARSE_MALFORMED;
    if (version != (double)GZ_CORRECTION_VERSION) return GZ_CORR_PARSE_MALFORMED;

    if (!gz_correction_check(&c)) {
        c.valid = 0;
        *out = c;
        return GZ_CORR_PARSE_BOUNDS;
    }
    c.valid = 1;
    *out = c;
    return GZ_CORR_PARSE_OK;
}
