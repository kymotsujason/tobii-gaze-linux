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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/proto.h"

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
    unsigned char buf[64];
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

    printf("all proto tests passed\n");
    return 0;
}
