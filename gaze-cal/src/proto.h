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

#define GZ_HEADER_SIZE   5
#define GZ_MAX_PAYLOAD   65536
#define GZ_ERR_DESYNC    (-1)

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
_Static_assert(sizeof(struct gz_gaze_sample) == 392,
               "GazeSample must match the Zig extern struct exactly");
_Static_assert(offsetof(struct gz_gaze_sample, timestamp_us) == 16, "layout drift");
_Static_assert(offsetof(struct gz_gaze_sample, gaze_point_2d_norm) == 40, "layout drift");
_Static_assert(offsetof(struct gz_gaze_sample, gaze_point_2d_unfiltered) == 376, "layout drift");

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

#endif
