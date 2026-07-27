/* gaze-cal/tests/test_proto.c
 *
 * Every expectation here is traceable to
 * vendor/tobiifree/driver/src/daemon_protocol.zig or to
 * vendor/tobiifree/driver/src/tobiifree_core.zig. ARCHITECTURE.md is wrong
 * about the opcodes and about the gaze payload size, so it is never cited.
 *
 * Runs without hardware and without a daemon.
 */
#include <assert.h>
#include <locale.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/proto.h"

/* -std=c11 sets __STRICT_ANSI__, so glibc does not declare M_PI. */
#define TEST_PI 3.14159265358979323846

/* GZ_CAL_POINTS lives in calibrate.h, which proto.c must not depend on. */
#define GZ_CAL_POINTS_IN_TEST 9

#ifdef NDEBUG
#error "test_proto.c relies on assert(); do not build it with NDEBUG"
#endif

/* ---------- layout: must match the Zig extern struct byte for byte ---------- */

static void test_struct_size_matches_wire(void) {
    assert(sizeof(struct gz_gaze_sample) == 392);
}

static void test_field_offsets(void) {
    /* A total-size assert cannot catch reordered fields. Pin each one. */
    assert(offsetof(struct gz_gaze_sample, present_mask)        == 0);
    assert(offsetof(struct gz_gaze_sample, frame_counter)       == 4);
    assert(offsetof(struct gz_gaze_sample, validity_L)          == 8);
    assert(offsetof(struct gz_gaze_sample, validity_R)          == 12);
    assert(offsetof(struct gz_gaze_sample, timestamp_us)        == 16);
    assert(offsetof(struct gz_gaze_sample, gaze_point_2d_norm)  == 40);
    assert(offsetof(struct gz_gaze_sample, gaze_point_2d_L_norm)== 56);
    assert(offsetof(struct gz_gaze_sample, gaze_point_2d_R_norm)== 72);
}

static void test_remaining_field_offsets(void) {
    /* The rest of the Zig field order, walked to the end so that inserting or
     * dropping a [3]f64 anywhere is caught rather than absorbed by the total. */
    assert(offsetof(struct gz_gaze_sample, pupil_L_mm)                 == 24);
    assert(offsetof(struct gz_gaze_sample, pupil_R_mm)                 == 32);
    assert(offsetof(struct gz_gaze_sample, eye_origin_L_mm)            == 88);
    assert(offsetof(struct gz_gaze_sample, eye_origin_R_mm)            == 112);
    assert(offsetof(struct gz_gaze_sample, trackbox_eye_pos_L)         == 136);
    assert(offsetof(struct gz_gaze_sample, trackbox_eye_pos_R)         == 160);
    assert(offsetof(struct gz_gaze_sample, gaze_point_3d_L_mm)         == 184);
    assert(offsetof(struct gz_gaze_sample, gaze_point_3d_R_mm)         == 208);
    assert(offsetof(struct gz_gaze_sample, eye_origin_L_display_mm)    == 232);
    assert(offsetof(struct gz_gaze_sample, eye_origin_R_display_mm)    == 256);
    assert(offsetof(struct gz_gaze_sample, trackbox_eye_pos_L_display) == 280);
    assert(offsetof(struct gz_gaze_sample, trackbox_eye_pos_R_display) == 304);
    assert(offsetof(struct gz_gaze_sample, eye_origin_raw_L_mm)        == 328);
    assert(offsetof(struct gz_gaze_sample, eye_origin_raw_R_mm)        == 352);
    assert(offsetof(struct gz_gaze_sample, gaze_point_2d_unfiltered)   == 376);
}

/* ---------- encode ---------- */

static void test_encode_subscribe(void) {
    unsigned char buf[16];
    size_t n = gz_encode_cmd(buf, sizeof buf, GZ_CMD_SUBSCRIBE, NULL, 0);
    assert(n == 5);
    assert(buf[0] == 0x01);
    assert(buf[1] == 0 && buf[2] == 0 && buf[3] == 0 && buf[4] == 0);
}

static void test_encode_payload_is_little_endian(void) {
    /* encodeHeader writes the length with .little, independent of host order. */
    unsigned char payload[260];
    unsigned char buf[512];
    memset(payload, 0xAB, sizeof payload);
    size_t n = gz_encode_cmd(buf, sizeof buf, GZ_CMD_CAL_APPLY, payload, sizeof payload);
    assert(n == 5 + sizeof payload);
    assert(buf[0] == 0x23);
    assert(buf[1] == 0x04 && buf[2] == 0x01 && buf[3] == 0x00 && buf[4] == 0x00); /* 260 LE */
    assert(memcmp(buf + 5, payload, sizeof payload) == 0);
}

static void test_encode_rejects_short_buffer(void) {
    unsigned char buf[8];
    unsigned char payload[8] = {0};
    assert(gz_encode_cmd(buf, sizeof buf, GZ_CMD_SUBSCRIBE, payload, sizeof payload) == 0);
    assert(gz_encode_cmd(buf, 4, GZ_CMD_SUBSCRIBE, NULL, 0) == 0);
    assert(gz_encode_cmd(buf, 5, GZ_CMD_SUBSCRIBE, NULL, 0) == 5); /* exact fit is fine */
}

static void test_encode_rejects_huge_payload(void) {
    /* cap < GZ_HEADER_SIZE + payload_len wraps to a tiny value when payload_len
     * is near SIZE_MAX, so a naive bound admits a SIZE_MAX memcpy. */
    /* Initialised only to keep -Wmaybe-uninitialized quiet: it is passed as the
     * payload as well as the destination, and the refusal happens before either
     * is read. */
    unsigned char buf[64] = { 0 };
    assert(gz_encode_cmd(buf, sizeof buf, GZ_CMD_CAL_APPLY, buf, SIZE_MAX) == 0);
    assert(gz_encode_cmd(buf, sizeof buf, GZ_CMD_CAL_APPLY, buf, SIZE_MAX - 4) == 0);
    assert(gz_encode_cmd(buf, sizeof buf, GZ_CMD_CAL_APPLY, buf, GZ_MAX_PAYLOAD + 1) == 0);
    /* The buffer is untouched: nothing was written before the refusal. */
    unsigned char probe[64];
    memset(buf, 0x5A, sizeof buf);
    memcpy(probe, buf, sizeof buf);
    assert(gz_encode_cmd(buf, sizeof buf, GZ_CMD_CAL_APPLY, buf, SIZE_MAX) == 0);
    assert(memcmp(buf, probe, sizeof buf) == 0);
}

static void test_encode_rejects_null_payload_with_nonzero_length(void) {
    /* memcpy from NULL is undefined even for a length the buffer could hold.
     * A caller that lost track of its blob pointer must get a refusal. */
    unsigned char buf[64];
    assert(gz_encode_cmd(buf, sizeof buf, GZ_CMD_CAL_APPLY, NULL, 8) == 0);
    assert(gz_encode_cmd(buf, sizeof buf, GZ_CMD_CAL_APPLY, NULL, 1) == 0);
    /* NULL with length 0 stays legal: that is how subscribe is built. */
    assert(gz_encode_cmd(buf, sizeof buf, GZ_CMD_SUBSCRIBE, NULL, 0) == 5);
}

static void test_frame_ceiling_covers_every_real_frame(void) {
    /* GZ_MAX_PAYLOAD is the daemon's true ceiling, not a round number, so the
     * frames it can actually emit must be checked to still fit. */
    assert(GZ_MAX_RESPONSE_PAYLOAD == 8192);
    assert(GZ_MAX_PAYLOAD == 8193);              /* 1 cmd_type + 8192 */
    assert(GZ_MAX_FRAME == 8198);                /* 5 header + 8193 */
    assert(GZ_MAX_FRAME == GZ_HEADER_SIZE + GZ_MAX_PAYLOAD);

    assert(GZ_HEADER_SIZE + 392 <= GZ_MAX_FRAME);              /* gaze */
    assert(GZ_HEADER_SIZE + GZ_STATUS_SIZE <= GZ_MAX_FRAME);   /* status */
    assert(GZ_HEADER_SIZE + 4 <= GZ_MAX_FRAME);                /* err */
    /* finish_calibration returns the blob: header + cmd_type + 4096. */
    assert(GZ_HEADER_SIZE + 1 + GZ_CAL_BLOB_MAX <= GZ_MAX_FRAME);

    /* A full-size calibration blob must still encode as a cal_apply command. */
    static unsigned char blob[GZ_CAL_BLOB_MAX];
    static unsigned char out[GZ_MAX_FRAME];
    memset(blob, 0x5A, sizeof blob);
    size_t n = gz_encode_cmd(out, sizeof out, GZ_CMD_CAL_APPLY, blob, sizeof blob);
    assert(n == GZ_HEADER_SIZE + GZ_CAL_BLOB_MAX);
    assert(memcmp(out + GZ_HEADER_SIZE, blob, sizeof blob) == 0);

    /* A frame at exactly the ceiling parses; one byte past it is desync. */
    static unsigned char big[GZ_MAX_FRAME];
    memset(big, 0, sizeof big);
    big[0] = GZ_SRV_RESPONSE;
    big[1] = (unsigned char)(GZ_MAX_PAYLOAD & 0xFF);
    big[2] = (unsigned char)((GZ_MAX_PAYLOAD >> 8) & 0xFF);
    struct gz_frame f;
    assert(gz_frame_parse(big, sizeof big, &f) == GZ_MAX_FRAME);
    assert(f.body_len == GZ_MAX_PAYLOAD - 1);

    uint32_t over = GZ_MAX_PAYLOAD + 1;
    big[1] = (unsigned char)(over & 0xFF);
    big[2] = (unsigned char)((over >> 8) & 0xFF);
    assert(gz_frame_parse(big, sizeof big, &f) == GZ_ERR_DESYNC);
}

/* ---------- response: body starts at 6, not 5 ---------- */

static void test_response_body_starts_at_offset_six(void) {
    /* response (0x02) prepends a one-byte cmd_type. Parsing from 5 misaligns every f64. */
    unsigned char wire[5 + 1 + 8] = {0x02, 9,0,0,0, 0x02};
    double v = 1.5; memcpy(wire + 6, &v, 8);
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == (int)sizeof wire);
    assert(f.type == 0x02);
    assert(f.cmd_type == 0x02);
    assert(f.body_len == 8);
    double out; memcpy(&out, f.body, 8);
    assert(out == 1.5);
}

static void test_response_offset_five_would_be_a_different_number(void) {
    /* Sharpens the test above: an offset-5 read here yields a finite, plausible
     * double rather than a NaN, so an off-by-one would not announce itself. */
    unsigned char wire[5 + 1 + 8] = {0x02, 9,0,0,0, GZ_CMD_FINISH_CAL};
    double v = 1.5; memcpy(wire + 6, &v, 8);

    double misread;
    memcpy(&misread, wire + 5, 8);
    assert(misread != v);   /* the two readings really do differ */

    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == (int)sizeof wire);
    assert(f.body == wire + 6);
    double out; memcpy(&out, f.body, 8);
    assert(out == v);
    assert(out != misread);
}

static void test_response_empty_body(void) {
    /* sendResult() emits cmd_type with no payload for start_calibration. */
    unsigned char wire[5 + 1] = {0x02, 1,0,0,0, GZ_CMD_START_CAL};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 6);
    assert(f.cmd_type == GZ_CMD_START_CAL);
    assert(f.body_len == 0);
}

static void test_response_with_zero_length_is_desync(void) {
    /* encodeResponse always writes at least the cmd_type byte, so len 0 is not
     * a frame this daemon can produce. */
    unsigned char wire[5] = {0x02, 0,0,0,0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
}

static void test_non_response_leaves_cmd_type_clear(void) {
    unsigned char wire[5 + 3] = {0x04, 3,0,0,0, 1, 1, GZ_PROTOCOL_VERSION};
    struct gz_frame f;
    memset(&f, 0xEE, sizeof f);
    assert(gz_frame_parse(wire, sizeof wire, &f) == 8);
    assert(f.cmd_type == 0);   /* not stale garbage from a previous frame */
    assert(f.body == wire + 5);
}

/* ---------- length validation and desync ---------- */

static void test_rejects_absurd_length(void) {
    /* After a desync the length field is not a length. Bound it or the reader hangs. */
    unsigned char wire[5] = {0x01, 0xFF,0xFF,0xFF,0xFF};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
}

static void test_rejects_wrong_length_for_type(void) {
    unsigned char wire[5 + 4] = {0x01, 4,0,0,0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
}

static void test_gaze_length_must_be_exact_not_minimum(void) {
    /* The check is len == 392. A >= would accept an overlong gaze frame and
     * then hand gz_frame_gaze a body it would copy only the first 392 bytes
     * of, silently resynchronising onto the wrong offset. */
    static const uint32_t plens[] = {393u, 400u, 784u, 391u, 0u};
    for (size_t i = 0; i < sizeof plens / sizeof plens[0]; i++) {
        unsigned char wire[5 + 800];
        memset(wire, 0, sizeof wire);
        wire[0] = GZ_SRV_GAZE;
        wire[1] = (unsigned char)(plens[i]);
        wire[2] = (unsigned char)(plens[i] >> 8);
        struct gz_frame f;
        assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
    }
    /* ... and 392 exactly is still accepted. */
    unsigned char ok[5 + 392];
    memset(ok, 0, sizeof ok);
    ok[0] = GZ_SRV_GAZE;
    ok[1] = (unsigned char)(392u & 0xFF);
    ok[2] = (unsigned char)(392u >> 8);
    struct gz_frame f;
    assert(gz_frame_parse(ok, sizeof ok, &f) == 397);
}

static void test_four_byte_header_is_not_over_read(void) {
    /* A 4-byte prefix is one short of a header. Reading the length field from
     * it touches a fifth byte that does not exist. Heap-allocated to exactly 4
     * so ASan traps the over-read rather than finding harmless stack padding. */
    for (size_t len = 0; len < GZ_HEADER_SIZE; len++) {
        unsigned char *heap = malloc(len ? len : 1);
        assert(heap != NULL);
        memset(heap, 0xFF, len ? len : 1);
        struct gz_frame f;
        assert(gz_frame_parse(heap, len, &f) == 0);
        free(heap);
    }
}

static void test_incomplete_frame_returns_zero(void) {
    unsigned char wire[3] = {0x01, 0, 0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 0);
}

static void test_rejects_unknown_type(void) {
    unsigned char wire[5] = {0x77, 0,0,0,0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
}

static void test_length_near_ceiling_never_wraps(void) {
    /* GZ_HEADER_SIZE + plen is uint32 arithmetic if plen is not widened first:
     * 5 + 0xFFFFFFFB == 0 and 5 + 0xFFFFFFFF == 4, either of which turns a
     * hostile header into "complete frame, consume 0 or 4 bytes". */
    static const uint32_t hostile[] = {
        0xFFFFFFFFu, 0xFFFFFFFBu, 0xFFFFFFFCu, 0x80000000u,
        GZ_MAX_PAYLOAD + 1u, 0x7FFFFFFFu
    };
    static const uint8_t types[] = {
        GZ_SRV_GAZE, GZ_SRV_RESPONSE, GZ_SRV_DISPLAY_AREA, GZ_SRV_STATUS, GZ_SRV_ERR
    };
    for (size_t t = 0; t < sizeof types / sizeof types[0]; t++) {
        for (size_t i = 0; i < sizeof hostile / sizeof hostile[0]; i++) {
            unsigned char wire[5 + 16];
            memset(wire, 0, sizeof wire);
            wire[0] = types[t];
            wire[1] = (unsigned char)(hostile[i]);
            wire[2] = (unsigned char)(hostile[i] >> 8);
            wire[3] = (unsigned char)(hostile[i] >> 16);
            wire[4] = (unsigned char)(hostile[i] >> 24);
            struct gz_frame f;
            int r = gz_frame_parse(wire, sizeof wire, &f);
            assert(r == GZ_ERR_DESYNC);   /* never 0, never a short positive */
        }
    }
}

static void test_consumed_never_exceeds_available(void) {
    /* The reader advances by the return value. A count larger than what it fed
     * in walks the cursor past the end of its own buffer. */
    unsigned char wire[5 + 392];
    memset(wire, 0, sizeof wire);
    wire[0] = GZ_SRV_GAZE;
    wire[1] = (unsigned char)(392 & 0xFF);
    wire[2] = (unsigned char)(392 >> 8);
    for (size_t len = 0; len <= sizeof wire; len++) {
        struct gz_frame f;
        int r = gz_frame_parse(wire, len, &f);
        assert(r >= 0);
        assert((size_t)r <= len);
    }
}

/* ---------- resumability: a frame split across reads ---------- */

static void build_gaze_frame(unsigned char *wire, uint32_t frame_counter) {
    struct gz_gaze_sample s;
    memset(&s, 0, sizeof s);
    s.present_mask = GZ_BIT_GAZE_2D | GZ_BIT_FRAME_COUNTER;
    s.frame_counter = frame_counter;
    s.validity_L = GZ_VALIDITY_VALID;
    s.validity_R = GZ_VALIDITY_VALID;
    s.timestamp_us = 123456789;
    s.gaze_point_2d_norm[0] = 0.25;
    s.gaze_point_2d_norm[1] = 0.75;
    wire[0] = GZ_SRV_GAZE;
    wire[1] = (unsigned char)(392u & 0xFF);
    wire[2] = (unsigned char)((392u >> 8) & 0xFF);
    wire[3] = 0;
    wire[4] = 0;
    memcpy(wire + 5, &s, sizeof s);
}

static void test_every_split_point_returns_incomplete(void) {
    /* Covers mid-header (1..4), the header/payload seam (5), and mid-payload
     * (6..396) exhaustively, which is what a stream socket actually does. */
    unsigned char wire[5 + 392];
    build_gaze_frame(wire, 4);
    for (size_t prefix = 0; prefix < sizeof wire; prefix++) {
        struct gz_frame f;
        assert(gz_frame_parse(wire, prefix, &f) == 0);
    }
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == (int)sizeof wire);
    assert(f.type == GZ_SRV_GAZE);
    assert(f.body_len == 392);
}

static void test_reader_loop_over_concatenated_frames(void) {
    /* The daemon queues whole frames back to back, so one read can deliver
     * several plus a partial tail. Drive the parser the way a reader would. */
    unsigned char stream[3 * (5 + 392)];
    build_gaze_frame(stream, 4);
    build_gaze_frame(stream + 397, 8);
    build_gaze_frame(stream + 794, 12);

    /* Feed one byte at a time, consuming frames as they complete. */
    unsigned char acc[2 * (5 + 392)];
    size_t have = 0;
    uint32_t expected[3] = {4, 8, 12};
    size_t got = 0;
    for (size_t i = 0; i < sizeof stream; i++) {
        acc[have++] = stream[i];
        for (;;) {
            struct gz_frame f;
            int r = gz_frame_parse(acc, have, &f);
            assert(r != GZ_ERR_DESYNC);
            if (r == 0) break;
            struct gz_gaze_sample s;
            assert(gz_frame_gaze(&f, &s) == 1);
            assert(s.frame_counter == expected[got]);
            got++;
            memmove(acc, acc + r, have - (size_t)r);
            have -= (size_t)r;
        }
    }
    assert(got == 3);
    assert(have == 0);
}

static void test_two_frames_then_partial_third(void) {
    unsigned char stream[2 * (5 + 392) + 7];
    build_gaze_frame(stream, 4);
    build_gaze_frame(stream + 397, 8);
    memset(stream + 794, 0, 7);
    stream[794] = GZ_SRV_GAZE;
    stream[795] = (unsigned char)(392u & 0xFF);
    stream[796] = (unsigned char)((392u >> 8) & 0xFF);

    struct gz_frame f;
    size_t off = 0;
    int r = gz_frame_parse(stream + off, sizeof stream - off, &f);
    assert(r == 397);
    off += (size_t)r;
    r = gz_frame_parse(stream + off, sizeof stream - off, &f);
    assert(r == 397);
    off += (size_t)r;
    r = gz_frame_parse(stream + off, sizeof stream - off, &f);
    assert(r == 0);   /* header parsed, payload still outstanding */
}

/* ---------- no over-read past the caller's buffer ---------- */

static void test_exact_fit_buffer_is_not_over_read(void) {
    /* Heap-allocated to the exact frame size so ASan traps any byte read past
     * the end. A hostile length that survived validation would land here. */
    const size_t n = 5 + 392;
    unsigned char *heap = malloc(n);
    assert(heap != NULL);
    build_gaze_frame(heap, 16);
    struct gz_frame f;
    assert(gz_frame_parse(heap, n, &f) == (int)n);
    struct gz_gaze_sample s;
    assert(gz_frame_gaze(&f, &s) == 1);
    assert(s.frame_counter == 16);
    free(heap);
}

static void test_hostile_header_at_end_of_buffer(void) {
    /* Only the 5 header bytes exist. Every validation path must decide from
     * those alone and must not touch heap[5]. */
    static const uint32_t plens[] = {392u, 0xFFFFFFFFu, 65537u, 3u};
    for (size_t i = 0; i < sizeof plens / sizeof plens[0]; i++) {
        unsigned char *heap = malloc(5);
        assert(heap != NULL);
        heap[0] = GZ_SRV_GAZE;
        heap[1] = (unsigned char)(plens[i]);
        heap[2] = (unsigned char)(plens[i] >> 8);
        heap[3] = (unsigned char)(plens[i] >> 16);
        heap[4] = (unsigned char)(plens[i] >> 24);
        struct gz_frame f;
        int r = gz_frame_parse(heap, 5, &f);
        assert(r == 0 || r == GZ_ERR_DESYNC);
        free(heap);
    }
}

/* ---------- status ---------- */

static void test_status_fields(void) {
    /* encodeStatus: [u8 0x04][u32 LE 3][u8 present][u8 cal][u8 version] */
    unsigned char wire[5 + 3] = {0x04, 3,0,0,0, 1, 0, GZ_PROTOCOL_VERSION};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 8);
    assert(f.type == GZ_SRV_STATUS);
    struct gz_status st;
    assert(gz_frame_status(&f, &st) == 1);
    assert(st.device_present == 1);
    assert(st.calibration_applied == 0);
    assert(st.protocol_version == GZ_PROTOCOL_VERSION);
}

static void test_status_version_survives_a_longer_payload(void) {
    /* The one rule: require payload_len >= 3 and read the version at payload
     * offset 2. A daemon that appends fields must stay readable, because the
     * version is what tells a client to stop. == 3 would desync instead. */
    unsigned char wire[5 + 6] = {0x04, 6,0,0,0, 1, 1, 2, 0xAA, 0xBB, 0xCC};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 11);
    struct gz_status st;
    assert(gz_frame_status(&f, &st) == 1);
    assert(st.device_present == 1);
    assert(st.calibration_applied == 1);
    assert(st.protocol_version == 2);   /* still at offset 2 */
}

static void test_status_shorter_than_three_is_desync(void) {
    unsigned char wire[5 + 2] = {0x04, 2,0,0,0, 1, 1};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
}

static void test_status_accessor_rejects_other_types(void) {
    unsigned char wire[5 + 4] = {0xFF, 4,0,0,0, 1,0,0,0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 9);
    struct gz_status st;
    assert(gz_frame_status(&f, &st) == 0);
}

/* ---------- error frames ---------- */

static void test_err_code_decoding(void) {
    unsigned char wire[5 + 4] = {0xFF, 4,0,0,0, 2,0,0,0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 9);
    assert(f.type == GZ_SRV_ERR);
    uint32_t code = 0;
    assert(gz_frame_err(&f, &code) == 1);
    assert(code == GZ_ERRCODE_USB_BUSY);
    assert(gz_err_retryable(code) == 1);
}

static void test_err_failed_is_not_retryable(void) {
    unsigned char wire[5 + 4] = {0xFF, 4,0,0,0, 1,0,0,0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 9);
    uint32_t code = 0;
    assert(gz_frame_err(&f, &code) == 1);
    assert(code == GZ_ERRCODE_FAILED);
    assert(gz_err_retryable(code) == 0);
}

static void test_err_unknown_code_is_tolerated(void) {
    /* proto.Err is a non-exhaustive Zig enum. A code from a newer daemon must
     * decode and be treated as non-retryable, not rejected or panicked on. */
    unsigned char wire[5 + 4] = {0xFF, 4,0,0,0, 0x39,0x30,0,0};   /* 12345 LE */
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 9);
    uint32_t code = 0;
    assert(gz_frame_err(&f, &code) == 1);
    assert(code == 12345);
    assert(gz_err_retryable(code) == 0);
}

static void test_err_code_is_little_endian(void) {
    unsigned char wire[5 + 4] = {0xFF, 4,0,0,0, 0x78,0x56,0x34,0x12};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 9);
    uint32_t code = 0;
    assert(gz_frame_err(&f, &code) == 1);
    assert(code == 0x12345678u);
}

static void test_err_wrong_length_is_desync(void) {
    unsigned char wire[5 + 8] = {0xFF, 8,0,0,0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
}

/* ---------- validity: 0 means VALID ---------- */

static void test_validity_zero_means_valid(void) {
    /* Inverting the rule flips every assert in this function. */
    assert(GZ_VALIDITY_VALID == 0);
    assert(gz_eye_valid(0) == 1);
    assert(gz_eye_valid(4) == 0);
    assert(gz_eye_valid(GZ_VALIDITY_NOT_DETECTED) == 0);
    assert(gz_eye_valid(1) == 0);

    struct gz_gaze_sample s;
    memset(&s, 0, sizeof s);
    s.validity_L = 0;
    s.validity_R = 0;
    assert(gz_sample_both_eyes_valid(&s) == 1);
    assert(gz_sample_any_eye_valid(&s) == 1);

    s.validity_R = GZ_VALIDITY_NOT_DETECTED;
    assert(gz_sample_both_eyes_valid(&s) == 0);
    assert(gz_sample_any_eye_valid(&s) == 1);

    s.validity_L = GZ_VALIDITY_NOT_DETECTED;
    assert(gz_sample_both_eyes_valid(&s) == 0);
    assert(gz_sample_any_eye_valid(&s) == 0);
}

/* ---------- gaze payload ---------- */

static void test_gaze_copy_from_unaligned_body(void) {
    /* body is buf+5, so it is 8-byte aligned only by luck. Casting it to a
     * struct with f64 members is undefined; the accessor must copy. */
    /* _Alignas pins the base, so body = raw + 6 is reliably unaligned. Without
     * it the stack address is incidental and the test passes by luck. */
    _Alignas(16) unsigned char raw[1 + 5 + 392];
    unsigned char *wire = raw + 1;
    build_gaze_frame(wire, 40);
    struct gz_frame f;
    assert(gz_frame_parse(wire, 5 + 392, &f) == 397);
    assert(((uintptr_t)f.body % 8) != 0);      /* genuinely unaligned */

    struct gz_gaze_sample s;
    assert(gz_frame_gaze(&f, &s) == 1);
    assert(s.frame_counter == 40);
    assert(s.timestamp_us == 123456789);
    assert(s.gaze_point_2d_norm[0] == 0.25);
    assert(s.gaze_point_2d_norm[1] == 0.75);
    assert((s.present_mask & GZ_BIT_GAZE_2D) != 0);
    assert(gz_sample_both_eyes_valid(&s) == 1);
}

static void test_gaze_accessor_rejects_other_types(void) {
    unsigned char wire[5 + 3] = {0x04, 3,0,0,0, 1, 1, GZ_PROTOCOL_VERSION};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 8);
    struct gz_gaze_sample s;
    assert(gz_frame_gaze(&f, &s) == 0);
}

/* ---------- accessors must defend their own inputs ---------- */

/* gz_frame_parse rejects a short gaze frame before an accessor ever sees it,
 * so the length checks inside the accessors are only reachable through a
 * hand-built gz_frame. They are still public entry points, and the OBS plugin
 * will hold frames across calls, so each one is exercised directly. Bodies are
 * heap-allocated to their exact length so ASan traps a loosened check. */

static void test_gaze_accessor_rejects_short_body(void) {
    unsigned char *body = malloc(8);
    assert(body != NULL);
    memset(body, 0, 8);
    struct gz_frame f = {GZ_SRV_GAZE, 0, body, 8};
    struct gz_gaze_sample s;
    assert(gz_frame_gaze(&f, &s) == 0);   /* must not memcpy 392 from 8 */
    f.body_len = 391;
    assert(gz_frame_gaze(&f, &s) == 0);
    f.body_len = 393;
    assert(gz_frame_gaze(&f, &s) == 0);
    free(body);
}

static void test_status_accessor_rejects_short_body(void) {
    unsigned char *body = malloc(2);
    assert(body != NULL);
    body[0] = 1; body[1] = 1;
    struct gz_frame f = {GZ_SRV_STATUS, 0, body, 2};
    struct gz_status st;
    assert(gz_frame_status(&f, &st) == 0);   /* must not read body[2] */
    free(body);
}

static void test_err_accessor_rejects_short_body(void) {
    unsigned char *body = malloc(3);
    assert(body != NULL);
    memset(body, 0, 3);
    struct gz_frame f = {GZ_SRV_ERR, 0, body, 3};
    uint32_t code = 0;
    assert(gz_frame_err(&f, &code) == 0);   /* must not read body[3] */
    assert(code == 0);
    free(body);
}

static void test_frame_counter_advances_by_four(void) {
    /* 33.2 Hz delivered, 133 Hz internal: the counter steps by 4 per sample,
     * which is the only reason a dropped frame is detectable at all. */
    assert(GZ_FRAME_COUNTER_STEP == 4);
    uint32_t prev = 100, next = 104;
    assert(gz_frames_dropped(prev, next) == 0);
    assert(gz_frames_dropped(100, 108) == 1);
    assert(gz_frames_dropped(100, 120) == 4);
    assert(gz_frames_dropped(100, 100) == 0);   /* duplicate, not a gap */
    assert(gz_frames_dropped(0xFFFFFFFCu, 0) == 0);   /* wraps cleanly */
    assert(gz_frames_dropped(0xFFFFFFF8u, 0) == 1);

    /* A sub-step delta is a duplicate or a reordering, never a gap. Guarding
     * on < 1 instead of < 4 makes these underflow to 0xFFFFFFFF, which a
     * caller would report as four billion lost samples. */
    assert(gz_frames_dropped(100, 101) == 0);
    assert(gz_frames_dropped(100, 102) == 0);
    assert(gz_frames_dropped(100, 103) == 0);
    /* Deltas that are not multiples of the step truncate downwards rather
     * than wrapping: 5..7 is still one step plus slop, so zero lost. */
    assert(gz_frames_dropped(100, 105) == 0);
    assert(gz_frames_dropped(100, 107) == 0);
    assert(gz_frames_dropped(100, 109) == 1);
}

static void test_present_mask_bits(void) {
    /* The 22 GAZE_BIT_* values from tobiifree_core.zig:1093-1114, in order.
     * A duplicated or mistyped shift here silently reads the wrong field's
     * presence, which no size or offset assert would catch. */
    static const uint32_t bits[] = {
        GZ_BIT_TIMESTAMP, GZ_BIT_FRAME_COUNTER, GZ_BIT_VALIDITY_L, GZ_BIT_VALIDITY_R,
        GZ_BIT_PUPIL_L, GZ_BIT_PUPIL_R, GZ_BIT_GAZE_2D, GZ_BIT_GAZE_2D_L,
        GZ_BIT_GAZE_2D_R, GZ_BIT_EYE_ORIGIN_L, GZ_BIT_EYE_ORIGIN_R,
        GZ_BIT_GAZE_DIR_L, GZ_BIT_GAZE_DIR_R, GZ_BIT_GAZE_3D_L, GZ_BIT_GAZE_3D_R,
        GZ_BIT_EYE_ORIGIN_L_DISP, GZ_BIT_EYE_ORIGIN_R_DISP,
        GZ_BIT_TRACKBOX_L_DISP, GZ_BIT_TRACKBOX_R_DISP,
        GZ_BIT_EYE_ORIGIN_RAW_L, GZ_BIT_EYE_ORIGIN_RAW_R, GZ_BIT_GAZE_2D_UNFILTERED
    };
    const size_t n = sizeof bits / sizeof bits[0];
    assert(n == 22);

    uint32_t seen = 0;
    for (size_t i = 0; i < n; i++) {
        assert(bits[i] == (1u << i));   /* right position, in the Zig's order */
        assert((seen & bits[i]) == 0);  /* and not a duplicate */
        seen |= bits[i];
    }
    assert(seen == 0x003FFFFFu);        /* exactly bits 0..21, nothing above */
}

/* ---------- display_area (0x03): known type, undefined shape ---------- */

static void test_display_area_frame_is_skippable(void) {
    /* The daemon never emits 0x03: get_display_area replies arrive as a
     * response (0x02) carrying a raw TTP payload. The type exists in Srv, so
     * it is consumed and handed to the caller rather than treated as desync. */
    unsigned char wire[5 + 10];
    memset(wire, 0, sizeof wire);
    wire[0] = GZ_SRV_DISPLAY_AREA;
    wire[1] = 10;
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 15);
    assert(f.type == GZ_SRV_DISPLAY_AREA);
    assert(f.body_len == 10);
}

/* ---------- command opcodes ---------- */

static void test_command_opcodes_match_the_zig_enum(void) {
    assert(GZ_CMD_SUBSCRIBE                == 0x01);
    assert(GZ_CMD_GET_DISPLAY_AREA         == 0x02);
    assert(GZ_CMD_SET_DISPLAY_AREA         == 0x03);
    assert(GZ_CMD_SET_DISPLAY_AREA_CORNERS == 0x04);
    assert(GZ_CMD_START_CAL                == 0x20);
    assert(GZ_CMD_ADD_CAL_POINT            == 0x21);
    assert(GZ_CMD_FINISH_CAL               == 0x22);
    assert(GZ_CMD_CAL_APPLY                == 0x23);
    assert(GZ_CMD_DISCONNECT               == 0xFF);
    assert(GZ_SRV_GAZE == 0x01 && GZ_SRV_RESPONSE == 0x02);
    assert(GZ_SRV_DISPLAY_AREA == 0x03 && GZ_SRV_STATUS == 0x04 && GZ_SRV_ERR == 0xFF);
    assert(GZ_CAL_BLOB_MAX == 4096);
}

/* ---------- TTP TLV and the display area ----------
 *
 * The fixture below is not synthesised. It is the exact 164-byte response body
 * captured from the live daemon on 2026-07-27 while the device held the
 * measured 597 x 336 mm geometry, so a decoder that agrees with it agrees with
 * the hardware rather than with this file's own idea of the format.
 *
 * Capture: connect, subscribe (0x01), get_display_area (0x02), take the body
 * of the response (0x02) whose cmd_type is 0x02, which starts at wire offset 6.
 */
static const unsigned char da_real[164] = {
    0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x03, 0x1f, 0x41, 0x04,
    0x00, 0x00, 0x00, 0x08, 0xff, 0xfb, 0x56, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x05, 0x68, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x03, 0x1f, 0x41, 0x04,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x05, 0x68, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x03, 0x1f, 0x41, 0x04,
    0x00, 0x00, 0x00, 0x08, 0xff, 0xfb, 0x56, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x30, 0x39,
};

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void put_be64(unsigned char *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

/* Encoder mirroring tobiifree_core.zig tlvF64Q42, used to build the negative
 * cases. Written independently of proto.c: if the decoder's scale were wrong
 * this would not compensate for it, because the scale is spelled out here. */
static size_t put_q42(unsigned char *p, double v) {
    p[0] = 4;
    put_be32(p + 1, 8);
    put_be64(p + 5, (uint64_t)(int64_t)(v * 4398046511104.0 + (v < 0 ? -0.5 : 0.5)));
    return 13;
}

static size_t put_tag(unsigned char *p, uint32_t tag) {
    p[0] = 5;
    put_be32(p + 1, 4);
    put_be32(p + 5, tag);
    return 9;
}

static size_t put_point3d(unsigned char *p, double x, double y, double z) {
    size_t n = put_tag(p, GZ_TLV_TAG_POINT3D);
    n += put_q42(p + n, x);
    n += put_q42(p + n, y);
    n += put_q42(p + n, z);
    return n;
}

/* Builds a display area body the way the device does. Returns the length. */
static size_t build_da(unsigned char *p, const double c[9]) {
    p[0] = 0; p[1] = 0;
    size_t n = 2;
    n += put_point3d(p + n, c[0], c[1], c[2]);
    n += put_point3d(p + n, c[3], c[4], c[5]);
    n += put_point3d(p + n, c[6], c[7], c[8]);
    n += put_tag(p + n, 0x010100);
    p[n] = 2; put_be32(p + n + 1, 4); put_be32(p + n + 5, 0x3039); n += 9;
    return n;
}

static void test_q42_against_hand_computed_values(void) {
    /* Q42 is raw / 2^42, and 2^42 is 4398046511104. Each expectation below is
     * the product worked out by hand, so a decoder using 2^32 or 2^42 with the
     * wrong sign convention fails here rather than at the hardware. */
    struct { uint64_t raw; double want; } cases[] = {
        { 0x0000040000000000ULL,  1.0     },   /* 1 * 2^42 = 4398046511104 */
        { 0xFFFFFC0000000000ULL, -1.0     },   /* two's complement of the above */
        { 0x0000010000000000ULL,  0.25    },   /* 2^40 / 2^42 */
        { 0x0000000000000000ULL,  0.0     },
        { 0x0000000000000001ULL,  1.0 / 4398046511104.0 },  /* one quantum */
        { 0xFFFFFFFFFFFFFFFFULL, -1.0 / 4398046511104.0 },  /* minus one quantum */
        { 0x0004AA0000000000ULL,  298.5   },   /* 298.5 * 2^42 = 1312816883564544 */
        { 0xFFFB560000000000ULL, -298.5   },
        { 0x0005680000000000ULL,  346.0   },   /* 346 * 2^42 = 1521724092841984 */
        { 0x0000280000000000ULL,  10.0    },   /* 10 * 2^42 = 43980465111040 */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unsigned char f[13];
        f[0] = 4; put_be32(f + 1, 8); put_be64(f + 5, cases[i].raw);
        struct gz_tlv r;
        gz_tlv_init(&r, f, sizeof f);
        double v = 12345.0;
        assert(gz_tlv_read_q42(&r, &v) == 1);
        assert(v == cases[i].want);
        assert(r.pos == 13);
    }
}

static void test_q42_extremes_do_not_trap(void) {
    /* INT64_MIN is the one value where negating the raw integer would
     * overflow. It must decode, and it must decode negative. */
    unsigned char f[13];
    f[0] = 4; put_be32(f + 1, 8); put_be64(f + 5, 0x8000000000000000ULL);
    struct gz_tlv r;
    gz_tlv_init(&r, f, sizeof f);
    double v = 0;
    assert(gz_tlv_read_q42(&r, &v) == 1);
    assert(v < 0);
    assert(v == -9223372036854775808.0 / 4398046511104.0);
}

static void test_tlv_rejects_the_wrong_type_byte(void) {
    unsigned char f[13];
    for (int t = 0; t < 256; t++) {
        f[0] = (unsigned char)t; put_be32(f + 1, 8); put_be64(f + 5, 0);
        struct gz_tlv r;
        gz_tlv_init(&r, f, sizeof f);
        double v;
        assert(gz_tlv_read_q42(&r, &v) == (t == GZ_TLV_TYPE_Q42));
    }
}

static void test_tlv_rejects_the_wrong_size_field(void) {
    /* The size is implied by the type, so checking it looks redundant. It is
     * the only thing separating a real field from a byte run of the right
     * length that happens to open with a 4. */
    for (uint32_t sz = 0; sz < 16; sz++) {
        unsigned char f[13];
        f[0] = 4; put_be32(f + 1, sz); put_be64(f + 5, 0);
        struct gz_tlv r;
        gz_tlv_init(&r, f, sizeof f);
        double v;
        assert(gz_tlv_read_q42(&r, &v) == (sz == 8));
    }
}

static void test_tlv_prolog_demands_type_five_size_four(void) {
    unsigned char f[9];
    put_tag(f, GZ_TLV_TAG_POINT3D);
    struct gz_tlv r;
    uint32_t tag = 0;
    gz_tlv_init(&r, f, sizeof f);
    assert(gz_tlv_read_prolog_tag(&r, &tag) == 1);
    assert(tag == 0x031f41u);
    assert(r.pos == 9);

    f[0] = 6;                       /* s64, not a prolog */
    gz_tlv_init(&r, f, sizeof f);
    assert(gz_tlv_read_prolog_tag(&r, &tag) == 0);

    put_tag(f, GZ_TLV_TAG_POINT3D);
    put_be32(f + 1, 8);             /* right type, wrong declared size */
    gz_tlv_init(&r, f, sizeof f);
    assert(gz_tlv_read_prolog_tag(&r, &tag) == 0);
}

static void test_point3d_demands_the_exact_tag(void) {
    unsigned char f[48];
    put_point3d(f, 1.0, 2.0, 3.0);
    double p[3];
    struct gz_tlv r;
    gz_tlv_init(&r, f, sizeof f);
    assert(gz_tlv_read_point3d(&r, p) == 1);
    assert(p[0] == 1.0 && p[1] == 2.0 && p[2] == 3.0);

    /* point2d's tag, and point3d_f's, both of which really occur in this
     * protocol. Accepting either would read the wrong number of coordinates
     * or the wrong fixed-point scale. */
    uint32_t wrong[] = { 0x021f40u, 0x031f42u, 0x031f40u, 0x020bb8u, 0 };
    for (size_t i = 0; i < sizeof wrong / sizeof wrong[0]; i++) {
        put_point3d(f, 1.0, 2.0, 3.0);
        put_be32(f + 5, wrong[i]);
        gz_tlv_init(&r, f, sizeof f);
        assert(gz_tlv_read_point3d(&r, p) == 0);
    }
}

static void test_point3d_leaves_the_output_alone_on_a_partial_read(void) {
    /* Two coordinates present, the third truncated. A decoder that wrote
     * straight into the caller's array would leave two fresh values beside one
     * stale one, which is a plausible-looking corner. */
    unsigned char f[48];
    put_point3d(f, 7.0, 8.0, 9.0);
    double p[3] = { -1, -1, -1 };
    struct gz_tlv r;
    gz_tlv_init(&r, f, 40);          /* cuts the last Q42 short */
    assert(gz_tlv_read_point3d(&r, p) == 0);
    assert(p[0] == -1 && p[1] == -1 && p[2] == -1);
}

static void test_tlv_reader_survives_a_position_past_the_end(void) {
    /* struct gz_tlv is public and pos is writable, which is how a caller
     * resumes a walk. A reader computing len - pos in size_t would wrap here
     * and treat a buffer it has run off the end of as having 2^64 bytes left,
     * so this is an out-of-bounds read rather than a wrong answer. */
    struct gz_tlv r;
    double v, p[3];
    uint32_t t;
    size_t past[] = { sizeof da_real + 1, sizeof da_real + 4096, SIZE_MAX };
    for (size_t i = 0; i < sizeof past / sizeof past[0]; i++) {
        gz_tlv_init(&r, da_real, sizeof da_real);
        r.pos = past[i];
        assert(gz_tlv_read_q42(&r, &v) == 0);
        assert(gz_tlv_read_prolog_tag(&r, &t) == 0);
        assert(gz_tlv_read_u32(&r, &t) == 0);
        assert(gz_tlv_read_point3d(&r, p) == 0);
    }
}

static void test_tlv_u32_reads_the_trailer(void) {
    /* The trailer the device appends: tag 0x010100 then u32 12345. Not part of
     * the geometry, but the reader has to be able to walk it or a later task
     * cannot tell a short body from a body it stopped reading early. */
    struct gz_tlv r;
    gz_tlv_init(&r, da_real, sizeof da_real);
    r.pos = 146;
    uint32_t tag = 0, v = 0;
    assert(gz_tlv_read_prolog_tag(&r, &tag) == 1 && tag == 0x010100u);
    assert(gz_tlv_read_u32(&r, &v) == 1 && v == 0x3039u);
    assert(r.pos == sizeof da_real);
    assert(gz_tlv_read_u32(&r, &v) == 0);   /* nothing after it */
}

static void test_decode_the_real_captured_body(void) {
    double c[9];
    assert(gz_decode_display_area(da_real, sizeof da_real, c) == 1);
    /* The corners the daemon logs at start-up: TL=(-299,346,0) TR=(299,346,0)
     * BL=(-299,10,0), printed with {d:.0}. Exact here. */
    assert(c[0] == -298.5 && c[1] == 346.0 && c[2] == 0.0);
    assert(c[3] ==  298.5 && c[4] == 346.0 && c[5] == 0.0);
    assert(c[6] == -298.5 && c[7] ==  10.0 && c[8] == 0.0);

    struct gz_rect r = gz_corners_to_rect(c);
    assert(r.w_mm == 597.0);
    assert(r.h_mm == 336.0);
    assert(r.ox_mm == -298.5 && r.oy_mm == 10.0);
    assert(r.z_mm == 0.0 && r.tilt_deg == 0.0);
}

static void test_decode_rejects_every_truncation(void) {
    /* Every prefix of a real body must be refused. A decoder that read a
     * partial point and returned success would hand back a rectangle built
     * from whatever the stack held. */
    for (size_t n = 0; n < GZ_DA_MIN_BODY; n++) {
        double c[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        assert(gz_decode_display_area(da_real, n, c) == 0);
        assert(c[0] == 1 && c[8] == 9);   /* untouched on refusal */
    }
    /* One byte more is enough, because the trailer is not read. */
    double c[9];
    assert(gz_decode_display_area(da_real, GZ_DA_MIN_BODY, c) == 1);
    assert(GZ_DA_MIN_BODY == 146);
}

static void test_decode_rejects_a_flipped_byte_anywhere_in_the_grammar(void) {
    /* The 146 bytes the decoder actually reads are prolog, three tags and nine
     * Q42 headers plus their bodies. Corrupting a structural byte must be
     * refused; corrupting a value byte must change the value. Neither may be
     * silently accepted as the original geometry. */
    double good[9];
    assert(gz_decode_display_area(da_real, sizeof da_real, good) == 1);

    for (size_t i = GZ_DA_PROLOG_SIZE; i < 146; i++) {
        unsigned char buf[sizeof da_real];
        memcpy(buf, da_real, sizeof buf);
        buf[i] ^= 0xFF;
        double c[9];
        if (gz_decode_display_area(buf, sizeof buf, c) == 1) {
            assert(memcmp(c, good, sizeof c) != 0);
        }
    }
}

static void test_decode_ignores_the_trailer(void) {
    /* decode_display_area stops after the third point, so a device that
     * changed or dropped the 0x010100 trailer still reports a geometry. */
    unsigned char buf[sizeof da_real];
    memcpy(buf, da_real, sizeof buf);
    memset(buf + 146, 0xAB, sizeof buf - 146);
    double c[9];
    assert(gz_decode_display_area(buf, sizeof buf, c) == 1);
    assert(c[0] == -298.5 && c[7] == 10.0);
    assert(gz_decode_display_area(buf, 146, c) == 1);
}

static void test_decode_rejects_a_body_of_doubles(void) {
    /* The shape the plan originally assumed: nine native f64 at offset 0. It
     * must not decode, because a decoder that accepted both could not tell
     * which it was looking at. */
    double raw[9] = { -298.5, 346, 0, 298.5, 346, 0, -298.5, 10, 0 };
    unsigned char body[sizeof raw];
    memcpy(body, raw, sizeof raw);
    double c[9];
    assert(gz_decode_display_area(body, sizeof body, c) == 0);
}

static void test_decode_rejects_null_and_empty(void) {
    double c[9];
    assert(gz_decode_display_area(NULL, 164, c) == 0);
    assert(gz_decode_display_area(da_real, 0, c) == 0);
    assert(gz_decode_display_area(da_real, 1, c) == 0);
}

/* ---------- corners to rect ---------- */

static void test_corners_to_rect(void) {
    /* The brief's own case, kept verbatim apart from the height: the real
     * device puts the bottom edge at y=10, not y=0. */
    double corners[9] = {
        -298.5, 346.0, 0.0,
         298.5, 346.0, 0.0,
        -298.5,  10.0, 0.0,
    };
    struct gz_rect r = gz_corners_to_rect(corners);
    assert(fabs(r.w_mm - 597.0) < 0.5);
    assert(fabs(r.h_mm - 336.0) < 0.5);
}

static void test_rect_round_trips_through_corners_at_a_nonzero_tilt(void) {
    /* THE CASE THE BRIEF'S FORMULAS GET WRONG, and the reason they are not
     * used. At tilt 0 every plausible variant agrees. Tilt the panel and:
     *   h  read off the y axis is h*cos(t), short by 1.5 mm at 10 degrees
     *   z  taken from tl is the TOP edge's z, not the bottom's
     *   atan2(bl.z - tl.z, h) is the negative of the tilt
     * gz_corners_to_rect is the exact inverse of Tracker.setDisplayArea, so
     * the round trip is exact and each of those three would break it. */
    struct gz_rect want = { 597.0, 336.0, -298.5, 10.0, 25.0, -12.5 };
    double c[9];
    gz_rect_to_corners(want, c);

    /* setDisplayArea's construction, spelled out, so this pins the wire
     * geometry rather than just self-consistency. */
    assert(c[6] == -298.5 && c[7] == 10.0 && c[8] == 25.0);          /* bl */
    assert(fabs(c[1] - (10.0 + 336.0 * cos(-12.5 * TEST_PI / 180))) < 1e-9);
    assert(fabs(c[2] - (25.0 + 336.0 * sin(-12.5 * TEST_PI / 180))) < 1e-9);
    assert(c[3] == c[0] + 597.0 && c[4] == c[1] && c[5] == c[2]);    /* tr */
    assert(c[2] < c[8]);   /* tilted toward the user: the top edge is nearer */

    struct gz_rect back = gz_corners_to_rect(c);
    assert(fabs(back.w_mm     - want.w_mm)     < 1e-9);
    assert(fabs(back.h_mm     - want.h_mm)     < 1e-9);
    assert(fabs(back.ox_mm    - want.ox_mm)    < 1e-9);
    assert(fabs(back.oy_mm    - want.oy_mm)    < 1e-9);
    assert(fabs(back.z_mm     - want.z_mm)     < 1e-9);
    assert(fabs(back.tilt_deg - want.tilt_deg) < 1e-9);

    /* And each wrong variant really is wrong here, so the assertions above are
     * not passing by coincidence. */
    assert(fabs(fabs(c[1] - c[7]) - want.h_mm) > 1.0);   /* projected height */
    assert(c[2] != want.z_mm);                            /* z from tl */
    assert(atan2(c[8] - c[2], want.h_mm) > 0);            /* flipped sign */
}

static void test_width_comes_from_the_top_edge(void) {
    /* setDisplayArea always sets tl.x = bl.x, so on a healthy device
     * tr.x - tl.x and tr.x - bl.x agree and the choice is invisible. It stops
     * being invisible the moment the device reports a quad that is not a
     * rectangle, and the width that matters is the one the daemon's builder
     * wrote: the top edge that tl and tr define together. The origin still
     * comes from bl, which is where setDisplayArea puts it. */
    double skew[9] = { -300, 346, 0,  297, 346, 0,  -280, 10, 0 };
    struct gz_rect r = gz_corners_to_rect(skew);
    assert(r.w_mm == 597.0);        /* 297 - (-300) */
    assert(r.w_mm != 577.0);        /* 297 - (-280), tr paired with bl */
    assert(r.ox_mm == -280.0);
}

static void test_rectangularity_check(void) {
    /* setDisplayArea writes tl.x = bl.x and a top edge level in both y and z,
     * so those three are the whole shape. tr.x is free because it carries the
     * width, and bl.y and bl.z are free because they carry the origin.
     *
     * This matters because gz_corners_to_rect takes the width from the top edge
     * and the origin from bl: a quad satisfying neither collapses into a
     * rectangle matching neither edge, and can land on the config by accident. */
    double good[9];
    gz_rect_to_corners((struct gz_rect){ 597, 336, -298.5, 10, 0, 0 }, good);
    assert(gz_corners_are_rectangular(good, 1.0) == 1);

    /* A genuine tilt is still rectangular: it moves the whole top edge
     * together, so tr stays level with tl. A check that rejected this would
     * refuse the very geometry Task 13 is about to measure. */
    double tilted[9];
    gz_rect_to_corners((struct gz_rect){ 597, 336, -298.5, 10, 25, -12.5 }, tilted);
    assert(gz_corners_are_rectangular(tilted, 1.0) == 1);
    assert(tilted[2] != tilted[8]);   /* top and bottom really are at different z */

    struct { int idx; double delta; const char *what; } skews[] = {
        { 0,  38.5, "tl.x away from bl.x" },
        { 6, -38.5, "bl.x away from tl.x" },
        { 4,   6.0, "tr.y above tl.y" },
        { 1,  -6.0, "tl.y below tr.y" },
        { 5,   9.0, "tr.z ahead of tl.z" },
        { 2,  -9.0, "tl.z behind tr.z" },
    };
    for (size_t i = 0; i < sizeof skews / sizeof skews[0]; i++) {
        double c[9];
        memcpy(c, good, sizeof c);
        c[skews[i].idx] += skews[i].delta;
        assert(gz_corners_are_rectangular(c, 1.0) == 0);
    }

    /* Inside the tolerance is still a rectangle, and the tolerance is the one
     * the caller passes rather than a second hidden constant. */
    double near[9];
    memcpy(near, good, sizeof near);
    near[0] += 0.5;
    assert(gz_corners_are_rectangular(near, 1.0) == 1);
    assert(gz_corners_are_rectangular(near, 0.25) == 0);

    /* NaN is not a rectangle, same !(x <= tol) spelling as gz_rect_diff. */
    double bad[9];
    memcpy(bad, good, sizeof bad);
    bad[0] = nan("");
    assert(gz_corners_are_rectangular(bad, 1e9) == 0);
}

static void test_a_skewed_quad_would_otherwise_pass(void) {
    /* Why the check is worth its cost: this quad is not a display area, and it
     * converts to exactly the rectangle the config asks for. Width from the top
     * edge is right, origin from bl is 38.5 mm out, and gz_rect_diff alone
     * would have to catch it on the origin. Shift bl.y to compensate and it
     * agrees on every field. */
    struct gz_rect want = { 597, 336, -298.5, 10, 0, 0 };
    double c[9] = { -298.5, 346, 0,  298.5, 346, 0,  -298.5, 10, 0 };
    c[0] = -260.0;                   /* tl.x pulled in, bl.x left alone */
    assert(gz_corners_are_rectangular(c, 1.0) == 0);

    struct gz_rect r = gz_corners_to_rect(c);
    assert(r.ox_mm == -298.5);       /* origin still from bl, still right */
    assert(fabs(r.w_mm - 558.5) < 1e-9);
    /* The width is wrong here, so the field comparison happens to catch it.
     * Move the skew to bl instead and it does not. */
    double c2[9] = { -298.5, 346, 0,  298.5, 346, 0,  -298.5, 10, 0 };
    c2[6] = -298.5;
    c2[0] = -298.5;
    c2[3] = 298.5;
    c2[4] = 346.0 + 0.0;
    c2[1] = 346.0;
    /* tr.z lifted: the top edge leans out of plane. Width, origin, height and
     * tilt all still read correct, because none of them looks at tr.z. */
    c2[5] = 40.0;
    assert(gz_corners_are_rectangular(c2, 1.0) == 0);
    assert(gz_rect_diff(gz_corners_to_rect(c2), want, 1.0, 0.5) == 0);
}

static void test_corners_to_rect_recovers_the_origin(void) {
    /* Two areas of identical size in different places are different
     * calibration frames, so w and h alone do not identify a geometry. */
    double a[9] = { -298.5, 346, 0,  298.5, 346, 0, -298.5, 10, 0 };
    double b[9] = { -298.5, 336, 0,  298.5, 336, 0, -298.5,  0, 0 };
    struct gz_rect ra = gz_corners_to_rect(a), rb = gz_corners_to_rect(b);
    assert(ra.w_mm == rb.w_mm && ra.h_mm == rb.h_mm);
    assert(ra.oy_mm != rb.oy_mm);
    assert(gz_rect_diff(ra, rb, 1.0, 0.5) == GZ_DA_DIFF_OY);
}

/* ---------- the pure half of the gate ---------- */

static void test_rect_diff_reports_each_field_separately(void) {
    struct gz_rect base = { 597, 336, -298.5, 10, 0, 0 };
    struct gz_rect m;

    assert(gz_rect_diff(base, base, 1.0, 0.5) == 0);

    m = base; m.w_mm    += 5;   assert(gz_rect_diff(m, base, 1.0, 0.5) == GZ_DA_DIFF_W);
    m = base; m.h_mm    -= 5;   assert(gz_rect_diff(m, base, 1.0, 0.5) == GZ_DA_DIFF_H);
    m = base; m.ox_mm   += 5;   assert(gz_rect_diff(m, base, 1.0, 0.5) == GZ_DA_DIFF_OX);
    m = base; m.oy_mm   += 5;   assert(gz_rect_diff(m, base, 1.0, 0.5) == GZ_DA_DIFF_OY);
    m = base; m.z_mm    += 5;   assert(gz_rect_diff(m, base, 1.0, 0.5) == GZ_DA_DIFF_Z);
    m = base; m.tilt_deg += 5;  assert(gz_rect_diff(m, base, 1.0, 0.5) == GZ_DA_DIFF_TILT);

    m = base; m.w_mm += 5; m.h_mm += 5;
    assert(gz_rect_diff(m, base, 1.0, 0.5) == (GZ_DA_DIFF_W | GZ_DA_DIFF_H));
}

static void test_rect_diff_is_inclusive_at_the_tolerance(void) {
    struct gz_rect base = { 597, 336, -298.5, 10, 0, 0 };
    struct gz_rect m = base;
    m.w_mm = 598.0;
    assert(gz_rect_diff(m, base, 1.0, 0.5) == 0);
    m.w_mm = 598.0 + 1e-9;
    assert(gz_rect_diff(m, base, 1.0, 0.5) == GZ_DA_DIFF_W);
    /* Tilt has its own tolerance: it is not measured in mm, and 0.5 degrees
     * over a 336 mm panel already moves the top edge by 2.9 mm. */
    m = base; m.tilt_deg = 0.9;
    assert(gz_rect_diff(m, base, 1.0, 0.5) == GZ_DA_DIFF_TILT);
    assert(gz_rect_diff(m, base, 1.0, 1.0) == 0);
}

static void test_rect_diff_calls_nan_a_mismatch(void) {
    /* A NaN from a corrupt decode must refuse, not pass. Written as
     * !(diff <= tol) for exactly this: (diff > tol) is false for NaN and would
     * have reported agreement. */
    struct gz_rect base = { 597, 336, -298.5, 10, 0, 0 };
    struct gz_rect m = base;
    m.w_mm = nan("");
    assert(m.w_mm != m.w_mm);
    assert(gz_rect_diff(m, base, 1e9, 1e9) == GZ_DA_DIFF_W);
    assert(gz_rect_diff(base, m, 1e9, 1e9) == GZ_DA_DIFF_W);
}

static void test_the_gate_refuses_the_daemons_template_geometry(void) {
    /* The concrete thing this whole chain defends against: the device holding
     * the shipped 1500x1000 mm placeholder while the config asks for the real
     * panel. Calibration in that frame is wrong everywhere and looks fine. */
    struct gz_rect want = { 597, 336, -298.5, 10, 0, 0 };
    struct gz_rect placeholder = { 1500, 1000, -750, -500, 0, 0 };
    double c[9];
    gz_rect_to_corners(placeholder, c);
    unsigned d = gz_rect_diff(gz_corners_to_rect(c), want, 1.0, 0.5);
    assert(d & GZ_DA_DIFF_W);
    assert(d & GZ_DA_DIFF_H);
    assert(d & GZ_DA_DIFF_OX);
    assert(d & GZ_DA_DIFF_OY);
}

static void test_the_gate_refuses_a_plausible_near_miss(void) {
    /* The dangerous case is not 1500x1000, which anyone would notice. It is a
     * geometry that is almost right: the same panel measured 5 mm wide, or the
     * right size sitting 30 mm lower than it does. */
    struct gz_rect want = { 597, 336, -298.5, 10, 0, 0 };
    struct gz_rect near[] = {
        { 602,   336,   -301.0,  10, 0, 0 },   /* 5 mm wider */
        { 597,   336,   -298.5, -20, 0, 0 },   /* 30 mm lower */
        { 597,   336,   -298.5,  10, 0, 3 },   /* 3 degrees of tilt */
        { 597,   336,   -298.5,  10, 40, 0 },  /* 40 mm further back */
        { 336,   597,   -168.0,  10, 0, 0 },   /* w and h swapped */
    };
    for (size_t i = 0; i < sizeof near / sizeof near[0]; i++) {
        double c[9];
        gz_rect_to_corners(near[i], c);
        assert(gz_rect_diff(gz_corners_to_rect(c), want, 1.0, 0.5) != 0);
    }
}

static void test_a_wrong_geometry_on_the_wire_is_refused_end_to_end(void) {
    /* From TLV bytes rather than from a struct, so the decode and the
     * comparison are both in the path a real refusal takes. */
    struct gz_rect want = { 597, 336, -298.5, 10, 0, 0 };
    double wrong_corners[9] = { -750, 500, 0,  750, 500, 0,  -750, -500, 0 };
    unsigned char body[256];
    size_t n = build_da(body, wrong_corners);
    assert(n == 164);

    double c[9];
    assert(gz_decode_display_area(body, n, c) == 1);
    assert(gz_rect_diff(gz_corners_to_rect(c), want, 1.0, 0.5) != 0);

    /* And the matching case really does pass, so the refusal above is not the
     * only thing this decoder can do. */
    double right[9];
    gz_rect_to_corners(want, right);
    n = build_da(body, right);
    assert(gz_decode_display_area(body, n, c) == 1);
    assert(gz_rect_diff(gz_corners_to_rect(c), want, 1.0, 0.5) == 0);

    /* The builder used above reproduces the device's bytes exactly, which is
     * what makes the two paragraphs comparable. */
    double real_corners[9] = { -298.5, 346, 0,  298.5, 346, 0,  -298.5, 10, 0 };
    n = build_da(body, real_corners);
    assert(n == sizeof da_real && memcmp(body, da_real, n) == 0);
}

/* ---------------- host-side gaze correction ----------------
 *
 * Every constant below traces to a measurement in
 * .superpowers/sdd/2026-07-26-phase1-bringup-and-daemon/cal-investigation.md
 * or to the display area the device was measured to be holding on 2026-07-27. */

/* Measured, and the one geometry any stored fit must match. Do not "improve"
 * these: z_mm was settled by the probe and the tilt was measured at
 * 2.15 +/- 1.95 deg, consistent with zero. */
static struct gz_rect measured_area(void) {
    struct gz_rect r = { 590.42, 333.72, -295.21, 5.0, -7.5, 0.0 };
    return r;
}

/* The head position measured over 376 frames at the normal playing position. */
static const double EYE_L[3] = { -22.96, 75.65, 439.40 };
static const double EYE_R[3] = {  43.05, 75.84, 437.81 };

static struct gz_gaze_sample mk_gaze(double gx, double gy,
                                     const double L[3], const double R[3],
                                     uint32_t vL, uint32_t vR) {
    struct gz_gaze_sample s;
    memset(&s, 0, sizeof s);
    /* What this firmware actually sends in EVERY frame, eyeless ones included.
     * Any code that gates on it is gating on a constant. */
    s.present_mask = 0x003fffffu;
    s.validity_L = vL;
    s.validity_R = vR;
    s.gaze_point_2d_norm[0] = gx;
    s.gaze_point_2d_norm[1] = gy;
    if (L != NULL) memcpy(s.eye_origin_L_mm, L, 3 * sizeof(double));
    if (R != NULL) memcpy(s.eye_origin_R_mm, R, 3 * sizeof(double));
    return s;
}

static struct gz_correction mk_corr(double gx, double gy, double bx, double by) {
    struct gz_correction c;
    memset(&c, 0, sizeof c);
    c.gx = gx; c.gy = gy; c.bx = bx; c.by = by;
    c.area = measured_area();
    c.form = GZ_CORR_FORM_STATIC;
    /* The seat the measured sweeps were taken at. Not on the transform path,
     * but gz_correction_check refuses a NaN or an absurd one. */
    c.eye_proj[0] = 0.5265;
    c.eye_proj[1] = 0.9818;
    c.valid = 1;
    return c;
}

/* Every key gz_correction_format writes, well formed, so the variants below
 * differ only in the one thing each is testing. */
#define CORR_GOOD_TEXT \
    "version=2\nform=1\ngx=1.1695\ngy=1.1875\nbx=-0.1034\nby=-0.2479\n" \
    "area_w_mm=590.42\narea_h_mm=333.72\narea_ox_mm=-295.21\n" \
    "area_oy_mm=5\narea_z_mm=-7.5\narea_tilt_deg=0\n" \
    "fit_eye_proj_x=0.5265\nfit_eye_proj_y=0.9818\n"

static void test_eye_proj_is_hand_computable(void) {
    struct gz_rect a = measured_area();
    double eye[3] = { 18.42, 70.61, 429.10 };
    double ep[2];
    gz_eye_proj(a, eye, ep);

    /* x: (18.42 + 295.21) / 590.42
     * y: (5.0 + 333.72 - 70.61) / 333.72, because normalised y grows DOWNWARD
     *    while tracker y grows upward. */
    assert(fabs(ep[0] - (18.42 + 295.21) / 590.42) < 1e-12);
    assert(fabs(ep[1] - (5.0 + 333.72 - 70.61) / 333.72) < 1e-12);
    assert(fabs(ep[0] - 0.5311981) < 1e-6);
    assert(fabs(ep[1] - 0.8033981) < 1e-6);
}

static void test_eye_proj_y_grows_downward(void) {
    /* The single easiest sign to get wrong, and it is silent when wrong: a
     * flipped y still produces plausible-looking gains. Raising the eye in
     * tracker space must LOWER the normalised y it projects to. */
    struct gz_rect a = measured_area();
    double low[3] = { 0, 50, 500 }, high[3] = { 0, 250, 500 };
    double lo[2], hi[2];
    gz_eye_proj(a, low, lo);
    gz_eye_proj(a, high, hi);
    assert(hi[1] < lo[1]);

    /* The tracker x axis is not flipped, so right is right. */
    double left[3] = { -200, 100, 500 }, right[3] = { 200, 100, 500 };
    gz_eye_proj(a, left, lo);
    gz_eye_proj(a, right, hi);
    assert(hi[0] > lo[0]);
}

static void test_eye_mid_uses_the_midpoint_of_two_eyes(void) {
    struct gz_eye_state e;
    gz_eye_state_init(&e);
    struct gz_gaze_sample s = mk_gaze(0.5, 0.5, EYE_L, EYE_R,
                                      GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
    double mid[3];
    assert(gz_sample_eye_mid(&e, &s, mid) == 1);
    for (int i = 0; i < 3; i++) assert(fabs(mid[i] - (EYE_L[i] + EYE_R[i]) / 2.0) < 1e-12);
    assert(e.have_lr == 1);
}

static void test_eye_mid_reconstructs_from_one_eye(void) {
    struct gz_eye_state e;
    gz_eye_state_init(&e);
    struct gz_gaze_sample both = mk_gaze(0.5, 0.5, EYE_L, EYE_R,
                                         GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
    double mid[3];
    assert(gz_sample_eye_mid(&e, &both, mid) == 1);

    /* Now the right eye drops out and the left one moves 10 mm right. The
     * midpoint must follow it by 10 mm, not jump back to the left eye. */
    double moved_L[3] = { EYE_L[0] + 10.0, EYE_L[1], EYE_L[2] };
    struct gz_gaze_sample one = mk_gaze(0.5, 0.5, moved_L, NULL,
                                        GZ_VALIDITY_VALID, GZ_VALIDITY_NOT_DETECTED);
    double got[3];
    assert(gz_sample_eye_mid(&e, &one, got) == 1);
    assert(fabs(got[0] - (moved_L[0] + (EYE_R[0] - EYE_L[0]) / 2.0)) < 1e-12);
    assert(fabs(got[0] - 20.045) < 1e-3);
    assert(fabs(got[1] - (moved_L[1] + (EYE_R[1] - EYE_L[1]) / 2.0)) < 1e-12);

    /* And symmetrically for a lone right eye, which subtracts instead. */
    struct gz_gaze_sample onlyR = mk_gaze(0.5, 0.5, NULL, EYE_R,
                                          GZ_VALIDITY_NOT_DETECTED, GZ_VALIDITY_VALID);
    assert(gz_sample_eye_mid(&e, &onlyR, got) == 1);
    assert(fabs(got[0] - (EYE_R[0] - (EYE_R[0] - EYE_L[0]) / 2.0)) < 1e-12);
}

static void test_eye_mid_refuses_before_it_has_seen_both_eyes(void) {
    struct gz_eye_state e;
    gz_eye_state_init(&e);
    struct gz_gaze_sample one = mk_gaze(0.5, 0.5, EYE_L, NULL,
                                        GZ_VALIDITY_VALID, GZ_VALIDITY_NOT_DETECTED);
    double mid[3] = { -1, -1, -1 };
    /* Guessing an interpupillary distance here would put a lateral error into
     * every frame of the session. One refused frame costs 30 ms. */
    assert(gz_sample_eye_mid(&e, &one, mid) == 0);
    assert(mid[0] == -1 && mid[1] == -1 && mid[2] == -1);
}

static void test_eye_mid_refuses_an_eyeless_frame(void) {
    struct gz_eye_state e;
    gz_eye_state_init(&e);
    struct gz_gaze_sample both = mk_gaze(0.5, 0.5, EYE_L, EYE_R,
                                         GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
    double mid[3];
    assert(gz_sample_eye_mid(&e, &both, mid) == 1);

    /* present_mask is still 0x003fffff and the eye-origin fields are zeros.
     * Only validity says so. */
    struct gz_gaze_sample none = mk_gaze(0.5, 0.5, NULL, NULL,
                                         GZ_VALIDITY_NOT_DETECTED, GZ_VALIDITY_NOT_DETECTED);
    assert(none.present_mask == 0x003fffffu);
    assert(none.eye_origin_L_mm[0] == 0.0);
    double got[3] = { 7, 7, 7 };
    assert(gz_sample_eye_mid(&e, &none, got) == 0);
    assert(got[0] == 7 && got[1] == 7 && got[2] == 7);
}

static void test_correction_identity_returns_the_input(void) {
    struct gz_correction c = mk_corr(1.0, 1.0, 0.0, 0.0);
    const double pts[][2] = { {0.1, 0.1}, {0.5, 0.5}, {0.9, 0.9}, {0.0, 1.0}, {0.37, 0.62} };
    for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        struct gz_gaze_sample s = mk_gaze(pts[i][0], pts[i][1], EYE_L, EYE_R,
                                          GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
        double out[2] = { -9, -9 };
        assert(gz_gaze_correct(&c, &s, out) == 1);
        /* Bit for bit, not within a tolerance. */
        assert(out[0] == pts[i][0]);
        assert(out[1] == pts[i][1]);
    }
}

static void test_correction_inverts_the_forward_model(void) {
    /* Build a reported point from the model, correct it, and land back on the
     * truth. This is what catches an inverted gain or a dropped offset. */
    const double gx = 1.1695, gy = 1.1875, bx = -0.1034, by = -0.2479;
    struct gz_correction c = mk_corr(gx, gy, bx, by);

    for (int i = 0; i < GZ_CAL_POINTS_IN_TEST; i++) {
        double tx = 0.1 + 0.4 * (i % 3), ty = 0.1 + 0.4 * (i / 3);
        double rx = gx * tx + bx;
        double ry = gy * ty + by;
        struct gz_gaze_sample s = mk_gaze(rx, ry, EYE_L, EYE_R,
                                          GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
        double out[2];
        assert(gz_gaze_correct(&c, &s, out) == 1);
        assert(fabs(out[0] - tx) < 1e-12);
        assert(fabs(out[1] - ty) < 1e-12);
    }
}

static void test_correction_refuses_and_leaves_out_untouched(void) {
    struct gz_correction c = mk_corr(1.1695, 1.1875, 0.0, 0.0);

    /* Both eyes not detected. The gaze point is still there and
     * present_mask still claims everything, so validity is the only signal:
     * return 0, out untouched. */
    struct gz_gaze_sample none = mk_gaze(0.5, 0.5, NULL, NULL,
                                         GZ_VALIDITY_NOT_DETECTED, GZ_VALIDITY_NOT_DETECTED);
    assert(none.present_mask == 0x003fffffu);
    double out[2] = { 42.0, 43.0 };
    assert(gz_gaze_correct(&c, &none, out) == 0);
    assert(out[0] == 42.0 && out[1] == 43.0);

    /* One eye is enough: form S needs no midpoint at all. */
    struct gz_gaze_sample one = mk_gaze(0.5, 0.5, EYE_L, NULL,
                                        GZ_VALIDITY_VALID, GZ_VALIDITY_NOT_DETECTED);
    assert(gz_gaze_correct(&c, &one, out) == 1);

    /* An unfitted correction is refused before anything else is read. */
    struct gz_correction unset;
    memset(&unset, 0, sizeof unset);
    struct gz_gaze_sample good = mk_gaze(0.5, 0.5, EYE_L, EYE_R,
                                         GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
    out[0] = 42.0; out[1] = 43.0;
    assert(gz_gaze_correct(&unset, &good, out) == 0);
    assert(out[0] == 42.0 && out[1] == 43.0);
}

static void test_the_single_eye_shortcut_still_moves_the_midpoint(void) {
    /* The midpoint is off the correction path under form S, but the sweep still
     * records it and spec 5.6 wants the head-aware question re-openable from
     * that record. Using the lone valid eye instead of reconstructing would put
     * half an interpupillary distance of error into every such record. */
    struct gz_eye_state e;
    gz_eye_state_init(&e);
    struct gz_gaze_sample both = mk_gaze(0.5, 0.5, EYE_L, EYE_R,
                                         GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
    double mid[3];
    assert(gz_sample_eye_mid(&e, &both, mid) == 1);

    struct gz_gaze_sample one = mk_gaze(0.5, 0.5, EYE_L, NULL,
                                        GZ_VALIDITY_VALID, GZ_VALIDITY_NOT_DETECTED);
    double kept[3];
    assert(gz_sample_eye_mid(&e, &one, kept) == 1);
    assert(fabs(kept[0] - mid[0]) < 1e-12);

    /* Against the lone eye, which is half an IPD away. */
    assert(fabs(mid[0] - EYE_L[0]) > 30.0);
    assert(fabs(mid[0] - EYE_L[0]) < 35.0);
}

static void test_the_correction_is_not_head_aware(void) {
    /* The measured decision, and the one a future reader is most likely to try
     * to undo. The design originally scaled about the eye's projection, form H,
     * on the geometric argument that an angular gain error must be centred on
     * the eye. Spec test 5.3 scored both forms on a recorded sweep at a seat
     * displaced 115 mm in the screen plane and form H lost, 69 px against 53.
     *
     * So the corrected point must depend on the reported gaze and nothing else.
     * Bit equality, not a tolerance: any eye term at all breaks this. */
    struct gz_correction c = mk_corr(1.1695, 1.1875, -0.1034, -0.2479);

    double L2[3] = { EYE_L[0] + 250.0, EYE_L[1] - 80.0, EYE_L[2] + 300.0 };
    double R2[3] = { EYE_R[0] + 250.0, EYE_R[1] - 80.0, EYE_R[2] + 300.0 };
    struct gz_gaze_sample s1 = mk_gaze(0.42, 0.37, EYE_L, EYE_R,
                                       GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
    struct gz_gaze_sample s2 = mk_gaze(0.42, 0.37, L2, R2,
                                       GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
    double o1[2], o2[2];
    assert(gz_gaze_correct(&c, &s1, o1) == 1);
    assert(gz_gaze_correct(&c, &s2, o2) == 1);
    assert(o1[0] == o2[0] && o1[1] == o2[1]);

    /* What that movement costs instead, which is what the report prints:
     * ((g-1)/g) times the in-plane displacement. 250 mm at 0.63 px/mm. */
    double g = (c.gx + c.gy) / 2.0;
    double px = (g - 1.0) / g * (250.0 / 590.42) * 2560.0;
    assert(fabs(px - 250.0 * GZ_CORR_DEGRADE_PX_PER_MM) < 10.0);
}

static void test_correction_check_holds_the_measured_envelope(void) {
    struct gz_correction c = mk_corr(1.1713, 1.1624, -0.0043, -0.1487);
    assert(gz_correction_check(&c) == 1);

    /* Every gain measured across the six nine-point sweeps passes. */
    const double measured[][2] = {
        { 1.1776, 1.1606 }, { 1.1642, 1.1652 }, { 1.1606, 1.1562 },
        { 1.1737, 1.1565 }, { 1.1828, 1.1690 }, { 1.1691, 1.1667 },
    };
    for (size_t i = 0; i < sizeof measured / sizeof measured[0]; i++) {
        struct gz_correction m = mk_corr(measured[i][0], measured[i][1], 0.0, -0.1);
        assert(gz_correction_check(&m) == 1);
    }

    struct gz_correction lo = mk_corr(1.04, 1.04, 0, 0);
    assert(gz_correction_check(&lo) == 0);
    struct gz_correction hi = mk_corr(1.31, 1.31, 0, 0);
    assert(gz_correction_check(&hi) == 0);

    /* Each axis is checked separately, and these two prove it: the pair stays
     * isotropic to within 5 percent and the other axis stays in range, so only
     * the one bound under test can refuse them. Without a case like this the
     * per-axis checks cover for each other and either could be deleted. */
    struct gz_correction only_x_out = mk_corr(1.35, 1.30, 0, 0);
    assert(gz_correction_check(&only_x_out) == 0);
    struct gz_correction only_y_out = mk_corr(1.05, 1.04, 0, 0);
    assert(gz_correction_check(&only_y_out) == 0);

    /* Anisotropy past 5 percent means bad input or a bug, not a person. */
    struct gz_correction aniso = mk_corr(1.25, 1.10, 0, 0);
    assert(gz_correction_check(&aniso) == 0);

    /* A NaN must fail rather than slip through a negated comparison, on every
     * parameter: `x < MIN || x > MAX` and `!(x >= MIN) || !(x <= MAX)` agree on
     * every real number and disagree exactly here. */
    const size_t nan_field[] = {
        offsetof(struct gz_correction, gx), offsetof(struct gz_correction, gy),
        offsetof(struct gz_correction, bx), offsetof(struct gz_correction, by),
    };
    for (size_t i = 0; i < sizeof nan_field / sizeof nan_field[0]; i++) {
        struct gz_correction n = mk_corr(1.17, 1.16, 0, 0);
        *(double *)((char *)&n + nan_field[i]) = 0.0 / 0.0;
        assert(gz_correction_check(&n) == 0);
    }

    /* An offset of a whole screen is not an offset, it is a broken fit. */
    struct gz_correction wild = mk_corr(1.17, 1.16, 0.0, -1.5);
    assert(gz_correction_check(&wild) == 0);

    /* A zero-sized area would divide by zero inside gz_eye_proj. */
    struct gz_correction flat = mk_corr(1.17, 1.16, 0, 0);
    flat.area.w_mm = 0;
    assert(gz_correction_check(&flat) == 0);

    /* The form lock, tested here rather than only through the parser. The
     * parser refuses an unknown form before it ever gets this far, so this is
     * the only reachable test of the predicate itself, and it is what stops a
     * caller that builds a gz_correction by hand, as the fit does, from
     * shipping one whose form nobody set. */
    struct gz_correction unset_form = mk_corr(1.17, 1.16, 0, 0);
    unset_form.form = 0;
    assert(gz_correction_check(&unset_form) == 0);
    struct gz_correction future_form = mk_corr(1.17, 1.16, 0, 0);
    future_form.form = GZ_CORR_FORM_STATIC + 1;
    assert(gz_correction_check(&future_form) == 0);

    /* And a seat that is NaN or absurd, which would print as one. */
    struct gz_correction nan_seat = mk_corr(1.17, 1.16, 0, 0);
    nan_seat.eye_proj[1] = 0.0 / 0.0;
    assert(gz_correction_check(&nan_seat) == 0);
    struct gz_correction wild_seat = mk_corr(1.17, 1.16, 0, 0);
    wild_seat.eye_proj[0] = 50.0;
    assert(gz_correction_check(&wild_seat) == 0);
}

static void test_correction_text_round_trips(void) {
    struct gz_correction c = mk_corr(1.17130042, 1.16240009, -0.00431234, -0.14872001);
    char buf[GZ_CORRECTION_TEXT_MAX];
    size_t n = gz_correction_format(&c, buf, sizeof buf);
    assert(n > 0 && buf[n] == '\0');

    struct gz_correction back;
    assert(gz_correction_parse(buf, n, &back) == GZ_CORR_PARSE_OK);
    assert(back.valid == 1);
    assert(fabs(back.gx - c.gx) < 1e-9);
    assert(fabs(back.gy - c.gy) < 1e-9);
    assert(fabs(back.bx - c.bx) < 1e-9);
    assert(fabs(back.by - c.by) < 1e-9);
    assert(gz_rect_diff(back.area, c.area, 1e-6, 1e-6) == 0);
    /* The two locks that stop a form H file being read as this one, and the
     * seat the fit belongs to, which spec 5.6 requires to be recorded. */
    assert(strstr(buf, "version=2\n") != NULL);
    assert(strstr(buf, "form=1\n") != NULL);
    assert(back.form == GZ_CORR_FORM_STATIC);
    assert(fabs(back.eye_proj[0] - c.eye_proj[0]) < 1e-9);
    assert(fabs(back.eye_proj[1] - c.eye_proj[1]) < 1e-9);

    /* Negative zero and exact integers survive the fixed-point spelling. */
    struct gz_correction z = mk_corr(1.2, 1.2, 0.0, 0.0);
    n = gz_correction_format(&z, buf, sizeof buf);
    assert(n > 0);
    assert(gz_correction_parse(buf, n, &back) == GZ_CORR_PARSE_OK);
    assert(back.gx == 1.2 && back.bx == 0.0);
}

static void test_correction_text_is_locale_independent(void) {
    /* proto.c hand-rolls both halves for exactly this reason: in a
     * comma-decimal locale strtod("1.1713") stops at the dot and returns 1.0,
     * a silently wrong gain that passes every bounds check below it. The
     * machine running the suite may have no such locale installed, in which
     * case the guarantee still holds by construction, since nothing in the
     * path calls strtod or a "%f" conversion. */
    struct gz_correction c = mk_corr(1.1713, 1.1624, -0.0043, -0.1487);
    char buf[GZ_CORRECTION_TEXT_MAX];
    size_t n = gz_correction_format(&c, buf, sizeof buf);
    assert(n > 0);
    /* The newline is part of the assertion: without the trailing-zero trim
     * the value would read gx=1.171300000, which still starts with the same
     * characters and would pass a prefix test. */
    assert(strstr(buf, "gx=1.1713\n") != NULL);
    assert(strstr(buf, "by=-0.1487\n") != NULL);
    assert(strstr(buf, "area_oy_mm=5\n") != NULL);
    assert(strchr(buf, ',') == NULL);

    static const char *const comma[] = {
        "de_DE.UTF-8", "fr_FR.UTF-8", "ru_RU.UTF-8", "es_ES.UTF-8", "de_DE", "fr_FR"
    };
    int tried = 0;
    for (size_t i = 0; i < sizeof comma / sizeof comma[0]; i++) {
        if (setlocale(LC_NUMERIC, comma[i]) == NULL) continue;
        tried = 1;
        char again[GZ_CORRECTION_TEXT_MAX];
        size_t m = gz_correction_format(&c, again, sizeof again);
        assert(m == n && memcmp(again, buf, n) == 0);
        struct gz_correction back;
        assert(gz_correction_parse(again, m, &back) == GZ_CORR_PARSE_OK);
        assert(fabs(back.gx - 1.1713) < 1e-9);
        break;
    }
    setlocale(LC_NUMERIC, "C");
    if (!tried) printf("  (no comma-decimal locale installed; the guarantee is structural)\n");
}

static void test_parse_ignores_provenance_and_comments(void) {
    /* The CLI appends keys this parser does not model. They must not break it,
     * because the whole point of writing them is that a later reader can find
     * out where the numbers came from. */
    const char *text =
        "# fitted 2026-07-27\n"
        "\n"
        "version=2\n"
        "form = 1\n"
        "gx = 1.1695\n"
        "gy=1.1875\n"
        "bx=-0.1034\n"
        "by=-0.2479\n"
        "area_w_mm=590.42\n"
        "area_h_mm=333.72\n"
        "area_ox_mm=-295.21\n"
        "area_oy_mm=5\n"
        "area_z_mm=-7.5\n"
        "area_tilt_deg=0\n"
        "fit_eye_proj_x=0.5265\n"
        "fit_eye_proj_y=0.9818\n"
        "fit_utc=2026-07-27T14:35:00Z\n"
        "fit_points=9\n"
        "degrade_px_per_mm=0.628\n"
        "fit_eye_z_mm=675\n";
    struct gz_correction c;
    assert(gz_correction_parse(text, strlen(text), &c) == GZ_CORR_PARSE_OK);
    assert(fabs(c.gx - 1.1695) < 1e-12);
    assert(fabs(c.area.ox_mm - -295.21) < 1e-12);
    assert(c.area.tilt_deg == 0.0);
    assert(c.form == GZ_CORR_FORM_STATIC);
    assert(fabs(c.eye_proj[1] - 0.9818) < 1e-12);

    /* A file with no trailing newline is the same file. */
    char trimmed[1024];
    size_t n = strlen(text);
    memcpy(trimmed, text, n - 1);
    assert(gz_correction_parse(trimmed, n - 1, &c) == GZ_CORR_PARSE_OK);
}

static void test_parse_refuses_a_form_h_file(void) {
    /* The reason the version was bumped. A form H file carries exactly these
     * key names, and its bx and by are measured relative to the eye's
     * projection rather than absolutely. Applying one here would be wrong by
     * (1-g) * E_proj, roughly 90 px, and would look like a calibration.
     *
     * This is the real correction.conf that shipped before spec test 5.3
     * decided against form H. */
    const char *form_h =
        "version=1\n"
        "gx=1.17213\ngy=1.189524978\nbx=-0.014093408\nby=-0.0627924\n"
        "area_w_mm=590.42\narea_h_mm=333.72\narea_ox_mm=-295.21\n"
        "area_oy_mm=5\narea_z_mm=-7.5\narea_tilt_deg=0\n"
        "fit_utc=2026-07-27T14:35:06Z\nfit_points=9\nfit_eye_z_mm=675.0\n";
    struct gz_correction c;
    /* STALE, not MALFORMED: the file is well formed, it is just of a version
     * this build does not apply, and the two have different remedies. The
     * version is judged before the key set, because an older version is
     * entitled to a different one. */
    assert(gz_correction_parse(form_h, strlen(form_h), &c) == GZ_CORR_PARSE_STALE);

    /* And a file that claims the current version but an unknown form. */
    const char *future =
        "version=2\nform=2\ngx=1.1695\ngy=1.1875\nbx=-0.1034\nby=-0.2479\n"
        "area_w_mm=590.42\narea_h_mm=333.72\narea_ox_mm=-295.21\n"
        "area_oy_mm=5\narea_z_mm=-7.5\narea_tilt_deg=0\n"
        "fit_eye_proj_x=0.5265\nfit_eye_proj_y=0.9818\n";
    assert(gz_correction_parse(future, strlen(future), &c) == GZ_CORR_PARSE_STALE);

    /* A file with no version at all is malformed rather than stale: nothing
     * says what it was meant to be. */
    const char *nover =
        "form=1\ngx=1.1695\ngy=1.1875\nbx=-0.1034\nby=-0.2479\n"
        "area_w_mm=590.42\narea_h_mm=333.72\narea_ox_mm=-295.21\n"
        "area_oy_mm=5\narea_z_mm=-7.5\narea_tilt_deg=0\n"
        "fit_eye_proj_x=0.5265\nfit_eye_proj_y=0.9818\n";
    assert(gz_correction_parse(nover, strlen(nover), &c) == GZ_CORR_PARSE_MALFORMED);
}

static void test_parse_refuses_everything_it_cannot_account_for(void) {
    const char *good = CORR_GOOD_TEXT;
    struct gz_correction c;
    assert(gz_correction_parse(good, strlen(good), &c) == GZ_CORR_PARSE_OK);

    char buf[1024];

    /* A missing key, one at a time, over every key the format models. A default
     * for any of them would be a number nobody chose. */
    static const char *const keys[] = {
        "gx=", "gy=", "bx=", "by=", "area_w_mm=",
        "area_h_mm=", "area_ox_mm=", "area_oy_mm=", "area_z_mm=",
        "area_tilt_deg=", "fit_eye_proj_x=", "fit_eye_proj_y="
    };
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        const char *at = strstr(good, keys[k]);
        assert(at != NULL);
        const char *eol = strchr(at, '\n');
        assert(eol != NULL);
        size_t head = (size_t)(at - good);
        size_t tail = strlen(eol + 1);
        assert(head + tail < sizeof buf);
        memcpy(buf, good, head);
        memcpy(buf + head, eol + 1, tail);
        assert(gz_correction_parse(buf, head + tail, &c) == GZ_CORR_PARSE_MALFORMED);
    }

    /* A repeated key: nobody could say which gain is in force. */
    snprintf(buf, sizeof buf, "%sgx=1.2\n", good);
    assert(gz_correction_parse(buf, strlen(buf), &c) == GZ_CORR_PARSE_MALFORMED);

    /* Junk where a number belongs, and junk after one. */
    snprintf(buf, sizeof buf, "%s", good);
    char *g = strstr(buf, "gx=1.1695");
    assert(g != NULL);
    memcpy(g, "gx=nan   ", 9);
    assert(gz_correction_parse(buf, strlen(buf), &c) == GZ_CORR_PARSE_MALFORMED);

    snprintf(buf, sizeof buf, "%s", good);
    g = strstr(buf, "gy=1.1875");
    assert(g != NULL);
    memcpy(g, "gy=1.18zz", 9);
    assert(gz_correction_parse(buf, strlen(buf), &c) == GZ_CORR_PARSE_MALFORMED);

    /* A line that is not key=value at all. */
    snprintf(buf, sizeof buf, "%sthis is not a setting\n", good);
    assert(gz_correction_parse(buf, strlen(buf), &c) == GZ_CORR_PARSE_MALFORMED);

    /* Empty and NULL. */
    assert(gz_correction_parse("", 0, &c) == GZ_CORR_PARSE_MALFORMED);
    assert(gz_correction_parse(NULL, 0, &c) == GZ_CORR_PARSE_MALFORMED);
}

static void test_parse_reports_a_bad_gain_separately(void) {
    /* Well formed but outside the envelope. The caller gets the numbers so it
     * can name the gain it refused, and valid stays 0 so nothing can use it. */
    const char *text =
        "version=2\nform=1\ngx=2.5\ngy=2.5\nbx=0\nby=0\n"
        "area_w_mm=590.42\narea_h_mm=333.72\narea_ox_mm=-295.21\n"
        "area_oy_mm=5\narea_z_mm=-7.5\narea_tilt_deg=0\n"
        "fit_eye_proj_x=0.5265\nfit_eye_proj_y=0.9818\n";
    struct gz_correction c;
    assert(gz_correction_parse(text, strlen(text), &c) == GZ_CORR_PARSE_BOUNDS);
    assert(c.valid == 0);
    assert(fabs(c.gx - 2.5) < 1e-12);

    struct gz_gaze_sample s = mk_gaze(0.5, 0.5, EYE_L, EYE_R,
                                      GZ_VALIDITY_VALID, GZ_VALIDITY_VALID);
    double out[2] = { 1, 2 };
    assert(gz_gaze_correct(&c, &s, out) == 0);
    assert(out[0] == 1 && out[1] == 2);
}

static void test_parse_handles_exponent_and_odd_spacing(void) {
    const char *text =
        "version=2\r\n"
        "form=1\n"
        "  gx  =  1.1695e0  \n"
        "gy=+1.1875\n"
        "bx=-1.034e-1\n"
        "by=-0.2479\n"
        "area_w_mm=5.9042e2\n"
        "area_h_mm=333.72\n"
        "area_ox_mm=-295.21\n"
        "area_oy_mm=5.\n"
        "area_z_mm=-7.5\n"
        "area_tilt_deg=0\n"
        "fit_eye_proj_x=0.5265\n"
        "fit_eye_proj_y=0.9818\n";
    struct gz_correction c;
    assert(gz_correction_parse(text, strlen(text), &c) == GZ_CORR_PARSE_OK);
    assert(fabs(c.gx - 1.1695) < 1e-12);
    assert(fabs(c.bx - -0.1034) < 1e-15);
    assert(fabs(c.area.w_mm - 590.42) < 1e-9);
    assert(c.area.oy_mm == 5.0);
}

static void test_format_refuses_what_it_cannot_write(void) {
    struct gz_correction c = mk_corr(1.1713, 1.1624, -0.0043, -0.1487);
    char buf[GZ_CORRECTION_TEXT_MAX];

    /* Every prefix of the needed length must refuse rather than truncate: a
     * half-written correction file that still parses is the bad outcome. */
    size_t n = gz_correction_format(&c, buf, sizeof buf);
    assert(n > 0);
    for (size_t cap = 0; cap <= n; cap++) {
        char small[GZ_CORRECTION_TEXT_MAX];
        memset(small, 'x', sizeof small);
        assert(gz_correction_format(&c, small, cap) == 0);
    }

    struct gz_correction bad = c;
    bad.gx = 0.0 / 0.0;
    assert(gz_correction_format(&bad, buf, sizeof buf) == 0);
    bad.gx = 1.0 / 0.0;
    assert(gz_correction_format(&bad, buf, sizeof buf) == 0);
    bad = c;
    bad.area.ox_mm = -1e12;
    assert(gz_correction_format(&bad, buf, sizeof buf) == 0);
}

int main(void) {
    test_struct_size_matches_wire();
    test_field_offsets();
    test_remaining_field_offsets();

    test_encode_subscribe();
    test_encode_payload_is_little_endian();
    test_encode_rejects_short_buffer();
    test_encode_rejects_huge_payload();
    test_encode_rejects_null_payload_with_nonzero_length();
    test_frame_ceiling_covers_every_real_frame();

    test_response_body_starts_at_offset_six();
    test_response_offset_five_would_be_a_different_number();
    test_response_empty_body();
    test_response_with_zero_length_is_desync();
    test_non_response_leaves_cmd_type_clear();

    test_rejects_absurd_length();
    test_rejects_wrong_length_for_type();
    test_gaze_length_must_be_exact_not_minimum();
    test_four_byte_header_is_not_over_read();
    test_incomplete_frame_returns_zero();
    test_rejects_unknown_type();
    test_length_near_ceiling_never_wraps();
    test_consumed_never_exceeds_available();

    test_every_split_point_returns_incomplete();
    test_reader_loop_over_concatenated_frames();
    test_two_frames_then_partial_third();

    test_exact_fit_buffer_is_not_over_read();
    test_hostile_header_at_end_of_buffer();

    test_status_fields();
    test_status_version_survives_a_longer_payload();
    test_status_shorter_than_three_is_desync();
    test_status_accessor_rejects_other_types();

    test_err_code_decoding();
    test_err_failed_is_not_retryable();
    test_err_unknown_code_is_tolerated();
    test_err_code_is_little_endian();
    test_err_wrong_length_is_desync();

    test_validity_zero_means_valid();

    test_gaze_copy_from_unaligned_body();
    test_gaze_accessor_rejects_other_types();
    test_gaze_accessor_rejects_short_body();
    test_status_accessor_rejects_short_body();
    test_err_accessor_rejects_short_body();
    test_frame_counter_advances_by_four();
    test_present_mask_bits();

    test_display_area_frame_is_skippable();
    test_command_opcodes_match_the_zig_enum();

    test_q42_against_hand_computed_values();
    test_q42_extremes_do_not_trap();
    test_tlv_rejects_the_wrong_type_byte();
    test_tlv_rejects_the_wrong_size_field();
    test_tlv_prolog_demands_type_five_size_four();
    test_point3d_demands_the_exact_tag();
    test_point3d_leaves_the_output_alone_on_a_partial_read();
    test_tlv_reader_survives_a_position_past_the_end();
    test_tlv_u32_reads_the_trailer();

    test_decode_the_real_captured_body();
    test_decode_rejects_every_truncation();
    test_decode_rejects_a_flipped_byte_anywhere_in_the_grammar();
    test_decode_ignores_the_trailer();
    test_decode_rejects_a_body_of_doubles();
    test_decode_rejects_null_and_empty();

    test_corners_to_rect();
    test_rect_round_trips_through_corners_at_a_nonzero_tilt();
    test_corners_to_rect_recovers_the_origin();
    test_width_comes_from_the_top_edge();
    test_rectangularity_check();
    test_a_skewed_quad_would_otherwise_pass();

    test_rect_diff_reports_each_field_separately();
    test_rect_diff_is_inclusive_at_the_tolerance();
    test_rect_diff_calls_nan_a_mismatch();
    test_the_gate_refuses_the_daemons_template_geometry();
    test_the_gate_refuses_a_plausible_near_miss();
    test_a_wrong_geometry_on_the_wire_is_refused_end_to_end();

    test_eye_proj_is_hand_computable();
    test_eye_proj_y_grows_downward();
    test_eye_mid_uses_the_midpoint_of_two_eyes();
    test_eye_mid_reconstructs_from_one_eye();
    test_eye_mid_refuses_before_it_has_seen_both_eyes();
    test_eye_mid_refuses_an_eyeless_frame();
    test_correction_identity_returns_the_input();
    test_correction_inverts_the_forward_model();
    test_correction_refuses_and_leaves_out_untouched();
    test_the_single_eye_shortcut_still_moves_the_midpoint();
    test_the_correction_is_not_head_aware();
    test_correction_check_holds_the_measured_envelope();
    test_correction_text_round_trips();
    test_correction_text_is_locale_independent();
    test_parse_ignores_provenance_and_comments();
    test_parse_refuses_a_form_h_file();
    test_parse_refuses_everything_it_cannot_account_for();
    test_parse_reports_a_bad_gain_separately();
    test_parse_handles_exponent_and_odd_spacing();
    test_format_refuses_what_it_cannot_write();

    printf("all proto tests passed\n");
    return 0;
}
