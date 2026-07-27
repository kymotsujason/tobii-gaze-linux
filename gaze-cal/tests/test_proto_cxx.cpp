/* gaze-cal/tests/test_proto_cxx.cpp
 *
 * Plan 2's OBS filter plugin is C++ and includes proto.h directly. This
 * translation unit is compiled with a C++ compiler and linked against a
 * C-compiled proto.o, so it proves both halves: that the header parses as C++,
 * and that the declarations do not mangle. A header that merely parses would
 * still fail at the link, which is where the missing extern "C" would bite.
 *
 * Every exported function is called, so all ten symbols must resolve.
 */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../src/proto.h"

#ifdef NDEBUG
#error "test_proto_cxx.cpp relies on assert(); do not build it with NDEBUG"
#endif

/* The C++ spelling of the same guarantee the C side asserts. If GZ_STATIC_ASSERT
 * silently expanded to nothing under __cplusplus, this line would still fail. */
static_assert(sizeof(struct gz_gaze_sample) == 392, "GazeSample must be 392 bytes in C++ too");
static_assert(offsetof(struct gz_gaze_sample, gaze_point_2d_norm) == 40, "layout drift in C++");
static_assert(GZ_MAX_FRAME == GZ_HEADER_SIZE + GZ_MAX_PAYLOAD, "frame ceiling");

int main() {
    /* gz_encode_cmd */
    unsigned char buf[64];
    size_t n = gz_encode_cmd(buf, sizeof buf, GZ_CMD_SUBSCRIBE, nullptr, 0);
    assert(n == 5);
    assert(buf[0] == 0x01 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0 && buf[4] == 0);

    /* gz_frame_parse on a gaze frame, then gz_frame_gaze */
    unsigned char wire[GZ_HEADER_SIZE + 392];
    std::memset(wire, 0, sizeof wire);
    struct gz_gaze_sample src;
    std::memset(&src, 0, sizeof src);
    src.frame_counter = 84;
    src.validity_L = GZ_VALIDITY_VALID;
    src.validity_R = GZ_VALIDITY_NOT_DETECTED;
    src.gaze_point_2d_norm[0] = 0.5;
    wire[0] = GZ_SRV_GAZE;
    wire[1] = static_cast<unsigned char>(392u & 0xFFu);
    wire[2] = static_cast<unsigned char>((392u >> 8) & 0xFFu);
    std::memcpy(wire + GZ_HEADER_SIZE, &src, sizeof src);

    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == static_cast<int>(sizeof wire));
    struct gz_gaze_sample out;
    assert(gz_frame_gaze(&f, &out) == 1);
    assert(out.frame_counter == 84);
    assert(out.gaze_point_2d_norm[0] == 0.5);

    /* validity helpers */
    assert(gz_eye_valid(GZ_VALIDITY_VALID) == 1);
    assert(gz_sample_any_eye_valid(&out) == 1);
    assert(gz_sample_both_eyes_valid(&out) == 0);

    /* gz_frame_status */
    unsigned char st_wire[GZ_HEADER_SIZE + 3] = {0x04, 3, 0, 0, 0, 1, 0, GZ_PROTOCOL_VERSION};
    assert(gz_frame_parse(st_wire, sizeof st_wire, &f) == 8);
    struct gz_status st;
    assert(gz_frame_status(&f, &st) == 1);
    assert(st.protocol_version == GZ_PROTOCOL_VERSION);

    /* gz_frame_err and gz_err_retryable */
    unsigned char er_wire[GZ_HEADER_SIZE + 4] = {0xFF, 4, 0, 0, 0, 2, 0, 0, 0};
    assert(gz_frame_parse(er_wire, sizeof er_wire, &f) == 9);
    uint32_t code = 0;
    assert(gz_frame_err(&f, &code) == 1);
    assert(code == GZ_ERRCODE_USB_BUSY);
    assert(gz_err_retryable(code) == 1);

    /* gz_frames_dropped */
    assert(gz_frames_dropped(100, 108) == 1);

    /* The TLV reader and the geometry conversion. Plan 2's plugin needs the
     * conversion to map gaze into panel coordinates, so these symbols have to
     * resolve from C++ as well. */
    unsigned char pt[48];
    pt[0] = 5; pt[1] = 0; pt[2] = 0; pt[3] = 0; pt[4] = 4;
    pt[5] = 0x00; pt[6] = 0x03; pt[7] = 0x1f; pt[8] = 0x41;
    for (int i = 0; i < 3; i++) {
        unsigned char *q = pt + 9 + i * 13;
        q[0] = 4; q[1] = 0; q[2] = 0; q[3] = 0; q[4] = 8;
        std::memset(q + 5, 0, 8);
        q[7] = 0x04;                  /* 1.0 in Q42: raw 2^42, big endian */
    }
    struct gz_tlv r;
    gz_tlv_init(&r, pt, sizeof pt);
    double p3[3] = {0, 0, 0};
    assert(gz_tlv_read_point3d(&r, p3) == 1);
    assert(p3[0] == 1.0 && p3[1] == 1.0 && p3[2] == 1.0);

    struct gz_rect want = {597, 336, -298.5, 10, 0, 0};
    double corners[9];
    gz_rect_to_corners(want, corners);
    struct gz_rect back = gz_corners_to_rect(corners);
    assert(back.w_mm == 597 && back.h_mm == 336 && back.oy_mm == 10);
    assert(gz_rect_diff(back, want, 1.0, 0.5) == 0);
    back.w_mm += 10;
    assert(gz_rect_diff(back, want, 1.0, 0.5) == GZ_DA_DIFF_W);

    unsigned char da[164];
    std::memset(da, 0, sizeof da);
    assert(gz_decode_display_area(da, sizeof da, corners) == 0);

    std::printf("all proto C++ interop tests passed\n");
    return 0;
}
