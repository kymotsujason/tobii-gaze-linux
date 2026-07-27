/* gaze-cal/src/proto.c - see proto.h. No allocation, no I/O, no CLI deps. */
#include <string.h>
#include "proto.h"

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
