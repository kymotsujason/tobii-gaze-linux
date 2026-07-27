/* gaze-cal/src/proto.h - tobiifreed unix-socket wire protocol.
 *
 * Mirrors vendor/tobiifree/driver/src/daemon_protocol.zig and the GazeSample
 * extern struct in driver/src/tobiifree_core.zig. Those two files are the only
 * source of truth. ARCHITECTURE.md in the vendored driver is wrong about the
 * opcodes and about the gaze payload size and is never cited here.
 *
 * This translation unit allocates nothing, performs no I/O, and depends on
 * nothing CLI-specific, because Plan 2's OBS filter plugin links it directly.
 *
 * Framing: [u8 msg_type][u32 LE payload_len][payload]. Header is 5 bytes.
 * A response (0x02) additionally prepends a one-byte cmd_type, so its body
 * starts at offset 6.
 */
#ifndef GZ_PROTO_H
#define GZ_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Plan 2's OBS filter plugin is C++, so this header must compile as both.
 * _Static_assert is C-only and the declarations would otherwise mangle and
 * fail to link against a C-built proto.o. */
#ifdef __cplusplus
#  define GZ_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
extern "C" {
#else
#  define GZ_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define GZ_HEADER_SIZE   5
#define GZ_ERR_DESYNC    (-1)

/* main.zig:265 MAX_RESPONSE_PAYLOAD. The daemon drops any device payload
 * larger than this and sends an err instead, so no bigger frame is emitted. */
#define GZ_MAX_RESPONSE_PAYLOAD 8192

/* The response is the largest frame the daemon can produce: encodeResponse
 * prepends a cmd_type byte, so payload_len tops out at 1 + 8192 = 8193.
 * Bounding at the real ceiling rather than a round 64 KiB keeps desync
 * detection tight: a garbage length above this is rejected on the spot instead
 * of stalling the reader while it waits for bytes that never arrive. */
#define GZ_MAX_PAYLOAD (1 + GZ_MAX_RESPONSE_PAYLOAD)

/* Size a receive accumulator with this, never with GZ_MAX_PAYLOAD. A caller
 * that allocates only GZ_MAX_PAYLOAD is 5 bytes short of the largest frame
 * this parser will call well-formed, and would stall on it forever. */
#define GZ_MAX_FRAME (GZ_HEADER_SIZE + GZ_MAX_PAYLOAD)

/* daemon_protocol.zig: PROTOCOL_VERSION, bumped when a message changes shape. */
#define GZ_PROTOCOL_VERSION 1

/* daemon_protocol.zig: STATUS_SIZE. */
#define GZ_STATUS_SIZE 3

/* tobiifree_core.zig cal_finish_blob_ptr() returns &out_scratch, a [4096]u8.
 * Bound against this, never against a local buffer size. */
#define GZ_CAL_BLOB_MAX 4096

/* Delivered samples step the counter by 4: the sensor runs at 133 Hz
 * internally and ships every fourth frame, giving the measured 33.2 Hz. */
#define GZ_FRAME_COUNTER_STEP 4

enum { GZ_CMD_SUBSCRIBE = 0x01, GZ_CMD_GET_DISPLAY_AREA = 0x02,
       GZ_CMD_SET_DISPLAY_AREA = 0x03, GZ_CMD_SET_DISPLAY_AREA_CORNERS = 0x04,
       GZ_CMD_START_CAL = 0x20, GZ_CMD_ADD_CAL_POINT = 0x21,
       GZ_CMD_FINISH_CAL = 0x22, GZ_CMD_CAL_APPLY = 0x23, GZ_CMD_DISCONNECT = 0xFF };
enum { GZ_SRV_GAZE = 0x01, GZ_SRV_RESPONSE = 0x02, GZ_SRV_DISPLAY_AREA = 0x03,
       GZ_SRV_STATUS = 0x04, GZ_SRV_ERR = 0xFF };

/* proto.Err. usb_busy means nothing reached the device, so it is retryable.
 * The Zig enum is non-exhaustive: treat any other code as a failure and log
 * it rather than switching exhaustively over these two. */
enum { GZ_ERRCODE_FAILED = 1, GZ_ERRCODE_USB_BUSY = 2 };

/* validity == 0 means VALID. Do not invert. */
#define GZ_VALIDITY_VALID        0u
#define GZ_VALIDITY_NOT_DETECTED 4u

/* present_mask bits, from tobiifree_core.zig GAZE_BIT_*. A field not flagged
 * here was absent from the device frame and is left zeroed, so 0.0 is not
 * distinguishable from "gaze at the top-left corner" without the mask. */
#define GZ_BIT_TIMESTAMP          (1u << 0)
#define GZ_BIT_FRAME_COUNTER      (1u << 1)
#define GZ_BIT_VALIDITY_L         (1u << 2)
#define GZ_BIT_VALIDITY_R         (1u << 3)
#define GZ_BIT_PUPIL_L            (1u << 4)
#define GZ_BIT_PUPIL_R            (1u << 5)
#define GZ_BIT_GAZE_2D            (1u << 6)
#define GZ_BIT_GAZE_2D_L          (1u << 7)
#define GZ_BIT_GAZE_2D_R          (1u << 8)
#define GZ_BIT_EYE_ORIGIN_L       (1u << 9)
#define GZ_BIT_EYE_ORIGIN_R       (1u << 10)
#define GZ_BIT_GAZE_DIR_L         (1u << 11)
#define GZ_BIT_GAZE_DIR_R         (1u << 12)
#define GZ_BIT_GAZE_3D_L          (1u << 13)
#define GZ_BIT_GAZE_3D_R          (1u << 14)
#define GZ_BIT_EYE_ORIGIN_L_DISP  (1u << 15)
#define GZ_BIT_EYE_ORIGIN_R_DISP  (1u << 16)
#define GZ_BIT_TRACKBOX_L_DISP    (1u << 17)
#define GZ_BIT_TRACKBOX_R_DISP    (1u << 18)
#define GZ_BIT_EYE_ORIGIN_RAW_L   (1u << 19)
#define GZ_BIT_EYE_ORIGIN_RAW_R   (1u << 20)
#define GZ_BIT_GAZE_2D_UNFILTERED (1u << 21)

/* validity == 0 means VALID. Do not invert. */
struct gz_gaze_sample {
    uint32_t present_mask, frame_counter, validity_L, validity_R;
    int64_t  timestamp_us;
    double   pupil_L_mm, pupil_R_mm;
    double   gaze_point_2d_norm[2], gaze_point_2d_L_norm[2], gaze_point_2d_R_norm[2];
    double   eye_origin_L_mm[3], eye_origin_R_mm[3];
    double   trackbox_eye_pos_L[3], trackbox_eye_pos_R[3];
    double   gaze_point_3d_L_mm[3], gaze_point_3d_R_mm[3];
    double   eye_origin_L_display_mm[3], eye_origin_R_display_mm[3];
    double   trackbox_eye_pos_L_display[3], trackbox_eye_pos_R_display[3];
    double   eye_origin_raw_L_mm[3], eye_origin_raw_R_mm[3];
    double   gaze_point_2d_unfiltered[2];
};

/* Checked in every translation unit that includes this header, so the OBS
 * plugin fails at its own compile rather than at ours. The offsets pin the
 * three places a reordering would land: the u32 block, the first f64 array,
 * and the tail. tests/test_proto.c pins all of them at runtime. */
GZ_STATIC_ASSERT(sizeof(struct gz_gaze_sample) == 392,
                 "GazeSample must match the Zig extern struct exactly");
GZ_STATIC_ASSERT(offsetof(struct gz_gaze_sample, timestamp_us) == 16, "layout drift");
GZ_STATIC_ASSERT(offsetof(struct gz_gaze_sample, gaze_point_2d_norm) == 40, "layout drift");
GZ_STATIC_ASSERT(offsetof(struct gz_gaze_sample, gaze_point_2d_unfiltered) == 376, "layout drift");

struct gz_frame {
    uint8_t type;
    uint8_t cmd_type;          /* valid only when type == GZ_SRV_RESPONSE */
    const unsigned char *body;
    size_t body_len;
};

struct gz_status {
    uint8_t device_present;
    uint8_t calibration_applied;
    uint8_t protocol_version;
};

/* Returns bytes consumed, 0 if incomplete, GZ_ERR_DESYNC if unparseable.
 * `out` points into `buf` and is valid only until the caller moves it. */
int gz_frame_parse(const unsigned char *buf, size_t len, struct gz_frame *out);

/* Returns bytes written, or 0 if the buffer is too small or the payload is
 * larger than the protocol allows. Writes nothing on refusal. */
size_t gz_encode_cmd(unsigned char *buf, size_t cap, uint8_t cmd,
                     const void *payload, size_t payload_len);

/* Typed views of a parsed frame. Each returns 1 on success, 0 when the frame
 * is the wrong type or too short. */
int gz_frame_status(const struct gz_frame *f, struct gz_status *out);
int gz_frame_err(const struct gz_frame *f, uint32_t *out_code);
int gz_frame_gaze(const struct gz_frame *f, struct gz_gaze_sample *out);

/* Only usb_busy is worth retrying: nothing reached the device. */
int gz_err_retryable(uint32_t code);

int gz_eye_valid(uint32_t validity);
int gz_sample_both_eyes_valid(const struct gz_gaze_sample *s);
int gz_sample_any_eye_valid(const struct gz_gaze_sample *s);

/* Samples lost between two consecutive delivered frames. */
uint32_t gz_frames_dropped(uint32_t prev_counter, uint32_t next_counter);

/* ---------------- TTP TLV, and the display area that rides on it -----------
 *
 * get_display_area does NOT come back as nine f64. Srv.display_area (0x03)
 * exists in the Zig enum and reaches encodeHeader nowhere in the daemon, so
 * that frame type is never emitted. The reply is a response (0x02) with
 * cmd_type 0x02 whose body is the device's raw TTP payload, forwarded verbatim:
 * tobiifree_core.zig dispatchFrame() hands on_response ttp + TTP_HDR_SIZE, and
 * main.zig onResponse() memcpys that straight into encodeResponse.
 *
 * The body is TLV, described in driver/src/tlv.zig. Every field is
 * [u8 type][u32 BE size][size bytes of body]. Only three types are needed
 * here, and each is length-checked against its declared size so that a body
 * which is merely the right length cannot be read as the right shape.
 *
 * Measured against the live device on 2026-07-27, body length exactly 164:
 *   [00 00] prolog, 2 bytes, skipped
 *   point3d TL, 48 bytes   (9-byte tag + 3 x 13-byte Q42)
 *   point3d TR, 48 bytes
 *   point3d BL, 48 bytes
 *   tag 0x010100, 9 bytes  + u32 0x3039, 9 bytes   (trailer, not read)
 * BR is never on the wire, in either direction: build_set_display_area_corners
 * encodes only TL, TR and BL. */
#define GZ_TLV_TYPE_U32     2
#define GZ_TLV_TYPE_FIX16   3
#define GZ_TLV_TYPE_Q42     4
#define GZ_TLV_TYPE_PROLOG  5

/* tlv.zig: point3d = prolog(0x031f41) + 3 x Q42. tobiifree_core.zig writes the
 * same constant as 0x31f41. Demanding it exactly is what stops a misparse from
 * yielding a clean-looking rectangle. */
#define GZ_TLV_TAG_POINT3D  0x031f41u

/* Q42 is a signed 64-bit fixed-point value scaled by 2^42. */
#define GZ_Q42_SCALE 4398046511104.0

/* decode_display_area (tobiifree_core.zig:445) skips two bytes before the
 * first point and does not look at them. */
#define GZ_DA_PROLOG_SIZE 2

/* Three point3d after the prolog. The trailer is not required, so a device
 * that stops here still decodes. */
#define GZ_DA_MIN_BODY (GZ_DA_PROLOG_SIZE + 3 * 48)

struct gz_tlv {
    const unsigned char *buf;
    size_t len;
    size_t pos;
};

void gz_tlv_init(struct gz_tlv *r, const unsigned char *buf, size_t len);

/* Each returns 1 on success and 0 on a short read, a wrong type byte, a size
 * field that does not match the type, or a tag that is not the one demanded.
 * A failed read leaves pos wherever it got to: the callers here abandon the
 * whole body rather than resynchronise, because there is nothing to
 * resynchronise onto. */
int gz_tlv_read_prolog_tag(struct gz_tlv *r, uint32_t *out_tag);
int gz_tlv_read_q42(struct gz_tlv *r, double *out);
int gz_tlv_read_u32(struct gz_tlv *r, uint32_t *out);
int gz_tlv_read_point3d(struct gz_tlv *r, double out[3]);

/* Port of decode_display_area. Writes tl,tr,bl as x,y,z into out[9] and
 * returns 1, or returns 0 and writes nothing. */
int gz_decode_display_area(const unsigned char *body, size_t len, double out[9]);

/* ---------------- display geometry ----------------
 *
 * The same parameterisation the daemon's config file uses,
 * Tracker.DisplayArea in driver/src/tracker.zig. ox/oy are the bottom-left
 * corner in tracker-space mm, which is where tracker.zig puts the origin, and
 * they are NOT derivable from w and h: two areas of identical size sitting in
 * different places are different calibration frames.
 *
 * All six fields are recovered, so gz_corners_to_rect is the exact inverse of
 * Tracker.setDisplayArea rather than a projection that happens to agree at
 * tilt 0. tests/test_proto.c pins the round trip at a nonzero tilt. */
struct gz_rect {
    double w_mm, h_mm;
    double ox_mm, oy_mm;   /* bottom-left corner, tracker space */
    double z_mm;           /* bottom edge distance along the tracker's z */
    double tilt_deg;       /* negative = top edge toward the user */
};

/* 1 when the three corners really describe the rectangle setDisplayArea builds:
 * tl.x == bl.x, tr.y == tl.y and tr.z == tl.z. gz_corners_to_rect takes the
 * width from the top edge and the origin from bl, which is where the daemon
 * puts them, so a quad that satisfies neither converts to a rectangle matching
 * neither edge and could pass the gate. Check this before converting. */
int gz_corners_are_rectangular(const double c[9], double tol_mm);

struct gz_rect gz_corners_to_rect(const double c[9]);

/* setDisplayArea's forward map, so a round trip is testable and Task 13 can
 * build a set_display_area_corners payload without repeating the trigonometry. */
void gz_rect_to_corners(struct gz_rect r, double out[9]);

/* Which fields of `got` differ from `want`. Zero means every field agrees.
 * Pure: the caller decides what to print and whether to refuse. */
#define GZ_DA_DIFF_W    (1u << 0)
#define GZ_DA_DIFF_H    (1u << 1)
#define GZ_DA_DIFF_OX   (1u << 2)
#define GZ_DA_DIFF_OY   (1u << 3)
#define GZ_DA_DIFF_Z    (1u << 4)
#define GZ_DA_DIFF_TILT (1u << 5)

unsigned gz_rect_diff(struct gz_rect got, struct gz_rect want,
                      double tol_mm, double tol_deg);

#ifdef __cplusplus
}
#endif

#endif
