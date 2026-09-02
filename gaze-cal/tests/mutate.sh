#!/usr/bin/env bash
# Mutation harness for the gaze-cal test suites.
#
# Breaks proto.c, client.c, display.c, calibrate.c or record.c one way at a
# time and requires the matching suite to fail.
# A mutation that survives means the test covering it is decorative, which is
# the failure mode this project has been bitten by: three plan-mandated code
# blocks were wrong and were caught by testing rather than by reading.
#
# No `set -e`: every build and every run below is expected to fail, and the
# exit status is the measurement.
set -uo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
CFLAGS="-std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
# proto.c grew hypot and atan2 with the display-area conversion.
LDLIBS="-lm"

# Counted separately on purpose. Folding a documented survivor into the killed
# total inflates the headline, which is exactly the loose claim the Task 10
# review caught.
killed=0
documented=0
unexpected=0

# Which suite the mutations below are judged against. Set once per section.
TEST_MAIN="tests/test_proto.c"
TEST_SRCS="src/proto.c"

fresh_copy() {
    rm -rf "$WORK/m"; mkdir -p "$WORK/m/src" "$WORK/m/tests"
    cp "$SRC/src/proto.c" "$SRC/src/proto.h" "$SRC/src/client.c" "$SRC/src/client.h" \
       "$SRC/src/display.c" "$SRC/src/display.h" "$SRC/src/calibrate.c" "$SRC/src/calibrate.h" \
       "$SRC/src/record.c" "$SRC/src/record.h" "$SRC/src/view.c" "$SRC/src/view.h" \
       "$WORK/m/src/"
    cp "$SRC/tests/test_proto.c" "$SRC/tests/test_client.c" "$SRC/tests/test_display.c" \
       "$SRC/tests/test_calibrate.c" "$SRC/tests/test_record.c" "$SRC/tests/test_view.c" \
       "$WORK/m/tests/"
}

apply_edit() {
    if ! grep -qF -- "$2" "$WORK/m/src/$1"; then
        echo "  STALE pattern not present in $1: ${2:0:60}"
        return 1
    fi
    python3 - "$WORK/m/src/$1" "$2" "$3" <<'PY'
import sys
p, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
assert s.count(a) >= 1
open(p, 'w').write(s.replace(a, b, 1))
PY
}

judge() {
    name="$1"; allow_survive="$2"
    srcs=""
    for s in $TEST_SRCS; do srcs="$srcs $WORK/m/$s"; done
    if ! out=$($CC $CFLAGS -o "$WORK/m/t" "$WORK/m/$TEST_MAIN" $srcs $LDLIBS 2>&1); then
        echo "  KILLED   $name  (compile-time)"
        killed=$((killed+1)); return
    fi
    out=$("$WORK/m/t" 2>&1); rc=$?
    if [ $rc -ne 0 ]; then
        detail=$(echo "$out" | grep -m1 -E 'Assertion|runtime error|ERROR: ' | sed 's/^ *//' | cut -c1-100)
        echo "  KILLED   $name  (rc=$rc) $detail"
        killed=$((killed+1))
    elif [ -n "$allow_survive" ]; then
        echo "  SURVIVED $name  (documented: unreachable given the other guard)"
        documented=$((documented+1))
    else
        echo "  SURVIVED $name  <-- the suite does not detect this"
        unexpected=$((unexpected+1))
    fi
}

run_mutation() {
    fresh_copy
    apply_edit "$2" "$3" "$4" || { unexpected=$((unexpected+1)); return; }
    judge "$1" "${5:-}"
}

# Two edits at once, for guards that only matter in combination.
run_mutation2() {
    fresh_copy
    apply_edit "$2" "$3" "$4" || { unexpected=$((unexpected+1)); return; }
    apply_edit "$5" "$6" "$7" || { unexpected=$((unexpected+1)); return; }
    judge "$1" ""
}

echo "== proto mutations =="

run_mutation "validity inverted" proto.c \
    "return validity == GZ_VALIDITY_VALID;" "return validity != GZ_VALIDITY_VALID;"

run_mutation "response body parsed from offset 5" proto.c \
    "out->body     = buf + GZ_HEADER_SIZE + 1;" "out->body     = buf + GZ_HEADER_SIZE;"

run_mutation "response body_len not decremented" proto.c \
    "out->body_len = (size_t)plen - 1;" "out->body_len = (size_t)plen;"

run_mutation "status length pinned to == 3" proto.c \
    "case GZ_SRV_STATUS:       return len >= GZ_STATUS_SIZE;" \
    "case GZ_SRV_STATUS:       return len == GZ_STATUS_SIZE;"

run_mutation "status version read from offset 1" proto.c \
    "out->protocol_version    = f->body[2];" "out->protocol_version    = f->body[1];"

run_mutation "plen bound removed" proto.c \
    "if (plen > GZ_MAX_PAYLOAD) return GZ_ERR_DESYNC;" "if (plen > 0xFFFFFFFEu) return GZ_ERR_DESYNC;"

# Expected to survive alone: the GZ_MAX_PAYLOAD bound makes the wrap
# unreachable. Kept to document that, and paired with the combined mutation
# at the end which removes both guards and must be caught.
run_mutation "total computed in 32-bit (unreachable alone)" proto.c \
    "size_t total = (size_t)GZ_HEADER_SIZE + (size_t)plen;" \
    "size_t total = (size_t)(GZ_HEADER_SIZE + plen);" ALLOW_SURVIVE

run_mutation "gaze copied via a misaligned cast" proto.c \
    "memcpy(out, f->body, sizeof *out);" \
    "*out = *(const struct gz_gaze_sample *)(const void *)f->body;"

run_mutation "gaze length check loosened" proto.c \
    "if (f->type != GZ_SRV_GAZE || f->body_len != sizeof *out) return 0;" \
    "if (f->type != GZ_SRV_GAZE || f->body_len < 8) return 0;"

run_mutation "encode payload bound removed" proto.c \
    "if (payload_len > GZ_MAX_PAYLOAD) return 0;" "if (payload_len > SIZE_MAX) return 0;"

run_mutation "encode cap check off by one" proto.c \
    "if (cap < (size_t)GZ_HEADER_SIZE + payload_len) return 0;" \
    "if (cap < payload_len) return 0;"

run_mutation "header length written big endian" proto.c \
    "p[0] = (unsigned char)(v & 0xFFu);" "p[0] = (unsigned char)((v >> 24) & 0xFFu);"

run_mutation "header length read big endian" proto.c \
    "return (uint32_t)p[0]" "return (uint32_t)p[3]"

run_mutation "unknown type accepted" proto.c \
    "    default:                  return 0;" "    default:                  return 1;"

run_mutation "cmd_type left stale on non-response" proto.c \
    "        out->cmd_type = 0;" "        ;"

run_mutation "err frame length check dropped" proto.c \
    "case GZ_SRV_ERR:          return len == 4;" "case GZ_SRV_ERR:          return len >= 1;"

run_mutation "usb_busy and failed swapped" proto.c \
    "return code == GZ_ERRCODE_USB_BUSY;" "return code == GZ_ERRCODE_FAILED;"

run_mutation "incomplete frame reported as complete" proto.c \
    "    if (len < total) return 0;" "    if (len < total) return (int)len;"

run_mutation "status accessor length check dropped" proto.c \
    "if (f->type != GZ_SRV_STATUS || f->body_len < GZ_STATUS_SIZE) return 0;" \
    "if (f->type != GZ_SRV_STATUS) return 0;"

run_mutation "err accessor length check dropped" proto.c \
    "if (f->type != GZ_SRV_ERR || f->body_len < 4) return 0;" \
    "if (f->type != GZ_SRV_ERR) return 0;"

run_mutation "frame counter step set to 1" proto.h \
    "#define GZ_FRAME_COUNTER_STEP 4" "#define GZ_FRAME_COUNTER_STEP 1"

run_mutation "struct fields reordered" proto.h \
    "    uint32_t present_mask, frame_counter, validity_L, validity_R;
    int64_t  timestamp_us;
    double   pupil_L_mm, pupil_R_mm;" \
    "    uint32_t present_mask, frame_counter, validity_R, validity_L;
    int64_t  timestamp_us;
    double   pupil_R_mm, pupil_L_mm;"

run_mutation "a [3]f64 field added" proto.h \
    "    double   eye_origin_raw_L_mm[3], eye_origin_raw_R_mm[3];" \
    "    double   eye_origin_raw_L_mm[3], eye_origin_raw_R_mm[3], pad_[3];"

run_mutation "subscribe opcode changed" proto.h \
    "enum { GZ_CMD_SUBSCRIBE = 0x01," "enum { GZ_CMD_SUBSCRIBE = 0x11,"

# Found surviving by the Task 10 review, which probed past the original set.
# Each is now killed by a test added in the fix round.

run_mutation "header completeness check off by one" proto.c \
    "    if (len < GZ_HEADER_SIZE) return 0;" "    if (len < 4) return 0;"

run_mutation "gaze frame length accepted as a minimum" proto.c \
    "case GZ_SRV_GAZE:         return len == sizeof(struct gz_gaze_sample);" \
    "case GZ_SRV_GAZE:         return len >= sizeof(struct gz_gaze_sample);"

run_mutation "encode NULL payload guard dropped" proto.c \
    "    if (payload_len > 0 && payload == NULL) return 0;" "    ;"

run_mutation "sub-step counter delta underflows" proto.c \
    "if (delta < GZ_FRAME_COUNTER_STEP) return 0;" "if (delta < 1) return 0;"

# Neither guard alone is provably load-bearing, so drop both and require the
# near-ceiling test to still catch the resulting 32-bit wrap.
run_mutation2 "plen bound AND widening both removed" \
    proto.c "if (plen > GZ_MAX_PAYLOAD) return GZ_ERR_DESYNC;" \
            "if (plen > 0xFFFFFFFEu) return GZ_ERR_DESYNC;" \
    proto.c "size_t total = (size_t)GZ_HEADER_SIZE + (size_t)plen;" \
            "size_t total = (size_t)(GZ_HEADER_SIZE + plen);"

echo
echo "== display area TLV and geometry mutations (judged by test_proto) =="

run_mutation "Q42 scaled by 2^32" proto.h \
    "#define GZ_Q42_SCALE 4398046511104.0" "#define GZ_Q42_SCALE 4294967296.0"

run_mutation "Q42 read as unsigned" proto.c \
    "    if (v <= (uint64_t)INT64_MAX) return (int64_t)v;
    return -(int64_t)(~v) - 1;" \
    "    return (int64_t)(v & 0x7FFFFFFFFFFFFFFFULL);"

run_mutation "TLV integers read little endian" proto.c \
    "    return ((uint32_t)p[0] << 24)" "    return ((uint32_t)p[3] << 24)"

run_mutation "TLV type byte not checked" proto.c \
    "    if (r->buf[r->pos] != want_type) return 0;" "    ;"

run_mutation "TLV declared size not checked" proto.c \
    "    if (rd_u32be(r->buf + r->pos + 1) != want_size) return 0;" "    ;"

run_mutation "point3d accepts any prolog tag" proto.c \
    "    if (tag != GZ_TLV_TAG_POINT3D) return 0;" "    ;"

run_mutation "point3d tag is point3d_f's" proto.h \
    "#define GZ_TLV_TAG_POINT3D  0x031f41u" "#define GZ_TLV_TAG_POINT3D  0x031f42u"

run_mutation "point3d writes through on a partial read" proto.c \
    "    double v[3];
    for (int i = 0; i < 3; i++) {
        if (!gz_tlv_read_q42(r, &v[i])) return 0;
    }
    memcpy(out, v, sizeof v);" \
    "    for (int i = 0; i < 3; i++) {
        if (!gz_tlv_read_q42(r, &out[i])) return 0;
    }"

run_mutation "TLV bounds computed by raw subtraction" proto.c \
    "    return r->pos <= r->len ? r->len - r->pos : 0;" "    return r->len - r->pos;"

run_mutation "display area prolog not skipped" proto.h \
    "#define GZ_DA_PROLOG_SIZE 2" "#define GZ_DA_PROLOG_SIZE 0"

run_mutation "display area decode writes on failure" proto.c \
    "    double v[9];
    for (int i = 0; i < 3; i++) {
        if (!gz_tlv_read_point3d(&r, v + i * 3)) return 0;
    }" \
    "    double *v = out;
    for (int i = 0; i < 3; i++) {
        if (!gz_tlv_read_point3d(&r, v + i * 3)) return 0;
    }"

# The three formulas the brief specified. Each agrees with the correct one at
# tilt 0 and disagrees the moment the panel is tilted, which is why the round
# trip test uses a nonzero tilt rather than the device's current geometry.
run_mutation "height taken as the y projection (brief's formula)" proto.c \
    "    r.h_mm     = hypot(dy, dz);" "    r.h_mm     = fabs(dy);"

run_mutation "z taken from the top corner (brief's formula)" proto.c \
    "    r.z_mm     = c[8];              /* bl.z */" \
    "    r.z_mm     = c[2];              /* bl.z */"

run_mutation "tilt sign flipped (brief's formula)" proto.c \
    "    r.tilt_deg = atan2(dz, dy) * GZ_DEG_PER_RAD;" \
    "    r.tilt_deg = atan2(-dz, dy) * GZ_DEG_PER_RAD;"

run_mutation "width read from the wrong corner pair" proto.c \
    "    r.w_mm     = fabs(c[3] - c[0]); /* tr.x - tl.x */" \
    "    r.w_mm     = fabs(c[3] - c[6]); /* tr.x - tl.x */"

run_mutation "left edge skew accepted" proto.c \
    "    if (!(fabs(c[0] - c[6]) <= tol_mm)) return 0;   /* tl.x vs bl.x */" "    ;"

run_mutation "top edge y lean accepted" proto.c \
    "    if (!(fabs(c[4] - c[1]) <= tol_mm)) return 0;   /* tr.y vs tl.y */" "    ;"

run_mutation "top edge z lean accepted" proto.c \
    "    if (!(fabs(c[5] - c[2]) <= tol_mm)) return 0;   /* tr.z vs tl.z */" "    ;"

# A shape check strict enough to reject a genuinely tilted panel is as bad as
# none: it would refuse the geometry Task 13 is about to measure.
run_mutation "shape check also demands a flush panel" proto.c \
    "    if (!(fabs(c[5] - c[2]) <= tol_mm)) return 0;   /* tr.z vs tl.z */" \
    "    if (!(fabs(c[5] - c[8]) <= tol_mm)) return 0;   /* tr.z vs tl.z */"

run_mutation "origin x dropped from the comparison" proto.c \
    "    if (!(fabs(got.ox_mm    - want.ox_mm)    <= tol_mm))  d |= GZ_DA_DIFF_OX;" "    ;"

run_mutation "origin y dropped from the comparison (brief's field set)" proto.c \
    "    if (!(fabs(got.oy_mm    - want.oy_mm)    <= tol_mm))  d |= GZ_DA_DIFF_OY;" "    ;"

run_mutation "tilt compared against the mm tolerance" proto.c \
    "    if (!(fabs(got.tilt_deg - want.tilt_deg) <= tol_deg)) d |= GZ_DA_DIFF_TILT;" \
    "    if (!(fabs(got.tilt_deg - want.tilt_deg) <= tol_mm))  d |= GZ_DA_DIFF_TILT;"

run_mutation "comparison written so NaN reads as a match" proto.c \
    "    if (!(fabs(got.w_mm     - want.w_mm)     <= tol_mm))  d |= GZ_DA_DIFF_W;" \
    "    if (fabs(got.w_mm     - want.w_mm)     >  tol_mm)     d |= GZ_DA_DIFF_W;"

run_mutation "rect to corners ignores tilt" proto.c \
    "    double tl_z = r.z_mm  + r.h_mm * sin(a);" "    double tl_z = r.z_mm;"

echo
echo "== display gate mutations =="

TEST_MAIN="tests/test_display.c"
TEST_SRCS="src/display.c src/client.c src/proto.c"

run_mutation "version gate removed" display.c \
    "    if (c->version_mismatch) {" "    if (0) {"

run_mutation "version gate runs after the geometry is read" display.c \
    "    if (gz_display_gate_status(c, 2000) != 0) return GZ_GATE_UNKNOWN;

    double corners[9];" \
    "    double corners[9];"

run_mutation "absent device tolerated" display.c \
    "    if (!c->status.device_present) {" "    if (0) {"

# Expected to survive: gz_client_init zeroes the client, so a connection that
# never produced a status also has device_present == 0, and the check below
# refuses for that reason instead. Kept because the two refusals mean different
# things and the messages are not interchangeable.
run_mutation "missing status tolerated (unreachable given the device check)" display.c \
    "    if (!c->have_status) {" "    if (0) {" ALLOW_SURVIVE

run_mutation "a mismatch is reported as unreadable" display.c \
    "    return gz_display_verify(corners, want, tol_mm, tol_deg) == 0
         ? GZ_GATE_OK : GZ_GATE_MISMATCH;" \
    "    gz_display_verify(corners, want, tol_mm, tol_deg);
    return GZ_GATE_UNKNOWN;"

run_mutation "verify passes whatever it is given" display.c \
    "    unsigned d = gz_rect_diff(r, want, tol_mm, tol_deg);" "    unsigned d = 0;"

run_mutation "an unparseable reply is accepted" display.c \
    "            if (gz_decode_display_area(c->resp, c->resp_len, out)) return 0;" \
    "            gz_decode_display_area(c->resp, c->resp_len, out); return 0;"

run_mutation "usb_busy not retried" display.c \
    "            if (!gz_err_retryable(c->err_code) || attempt + 1 == GZ_DA_RETRIES) {" \
    "            if (1) {"

run_mutation "every error code retried" display.c \
    "            if (!gz_err_retryable(c->err_code) || attempt + 1 == GZ_DA_RETRIES) {" \
    "            if (attempt + 1 == GZ_DA_RETRIES) {"

# Expected to survive: the give-up conditions inside the loop all test
# attempt + 1 == GZ_DA_RETRIES, so they cap the count before the loop bound
# ever does. The bound is belt and braces. The mutation below changes the
# constant both of them read, and must be caught.
run_mutation "loop bound raised (unreachable, the inner checks cap first)" display.c \
    "    for (int attempt = 0; attempt < GZ_DA_RETRIES; attempt++) {" \
    "    for (int attempt = 0; attempt < 1000; attempt++) {" ALLOW_SURVIVE

run_mutation "retry budget cut to two" display.c \
    "#define GZ_DA_RETRIES 3" "#define GZ_DA_RETRIES 2"

run_mutation "a timeout is retried on the same connection" display.c \
    "            if (path == NULL || attempt + 1 == GZ_DA_RETRIES) {" \
    "            if (0) {"

# The Important finding from the Task 12 review. gz_client_reconnect goes
# through gz_client_init, which memsets the client and clears have_status and
# version_mismatch, so without the re-gate the loop commands a daemon it has
# never identified. Every earlier test passed path == NULL and missed it.
run_mutation "version gate dropped on reconnect" display.c \
    "            if (gz_display_gate_status(c, timeout_ms) != 0) {" "            if (0) {"

run_mutation "skewed quad converted instead of refused" display.c \
    "    if (!gz_corners_are_rectangular(got, tol_mm)) {" "    if (0) {"

run_mutation "success caveat dropped from the verdict" display.c \
    "    if (d == 0) {" "    if (d == 0) return 0;
    if (0) {"

run_mutation "caveat also printed on a refusal" display.c \
    "    fprintf(stderr, \"  differs:\");" \
    "    fprintf(stderr, \"z_mm, tilt, cx and cy are the daemon's\\n  differs:\");"

run_mutation "config parse failure falls back to the daemon's template" display.c \
    "    if (j.pos != j.n) return -2;
    if (!found) return -2;" \
    "    if (j.pos != j.n) { *out = r; return 0; }
    if (!found) { *out = r; return 0; }"

run_mutation "config origin sign flipped" display.c \
    "        r.oy_mm = -cy - half_h;" "        r.oy_mm = cy - half_h;"

run_mutation "config halves taken from the template, not the panel" display.c \
    "    double half_w = r.w_mm / 2.0, half_h = r.h_mm / 2.0;" \
    "    double half_w = 750.0, half_h = 500.0;"

run_mutation "anchor expression sign ignored" display.c \
    "    else if (s[i] == '-') sign = -1;" "    else if (s[i] == '-') sign = 1;"

run_mutation "anchor expression axis not checked" display.c \
    "    case 'b': if (!is_vertical) return -1; anchor = -half; break;" \
    "    case 'b': anchor = -half; break;"

run_mutation "anchor expression accepts trailing text" display.c \
    "    if (end == s + i || *end != '\\0') return -1;" "    if (end == s + i) return -1;"

run_mutation "JSON string escape accepted blindly" display.c \
    "            default:   return 0;" "            default:   break;"

run_mutation "config size bound raised past what the daemon reads" display.c \
    "#define GZ_CFG_MAX 4096" "#define GZ_CFG_MAX 65536"

run_mutation "config size bound off by one" display.c \
    "    if (n > GZ_CFG_MAX) return -2;" "    if (n >= GZ_CFG_MAX) return -2;"

run_mutation "trailing garbage after the config accepted" display.c \
    "    if (j.pos != j.n) return -2;" "    ;"

echo
echo "== client mutations =="

TEST_MAIN="tests/test_client.c"
TEST_SRCS="src/client.c src/proto.c"

run_mutation "subscribe never queued" client.c \
    "    c->out_len = gz_encode_cmd(c->out, sizeof c->out, GZ_CMD_SUBSCRIBE, NULL, 0);" \
    "    c->out_len = 0;"

run_mutation "reconnect does not re-subscribe or reset state" client.c \
    "    gz_client_init(c);
    c->fd = fd;" \
    "    c->fd = fd;"

run_mutation "incomplete and desync conflated as r <= 0" client.c \
    "        if (r == 0) break;                       /* incomplete: keep the bytes */" \
    "        if (r <= 0) break;                       /* incomplete: keep the bytes */"

run_mutation "a torn read treated as corruption" client.c \
    "        if (r == 0) break;                       /* incomplete: keep the bytes */" \
    "        if (r == 0) { desync = 1; break; }"

run_mutation "desync not sticky" client.c \
    "int gz_client_feed_at(struct gz_client *c, const unsigned char *b, size_t n, uint64_t now_ns) {
    if (c->link_broken) return GZ_CLIENT_RECONNECT;" \
    "int gz_client_feed_at(struct gz_client *c, const unsigned char *b, size_t n, uint64_t now_ns) {"

run_mutation "response body copied from the wrong offset" client.c \
    "            memcpy(c->resp, f.body, n);" "            memcpy(c->resp, c->in, n);"

run_mutation "compaction skipped" client.c \
    "        memmove(c->in, c->in + off, c->in_len - off);" "        ;"

run_mutation "feed does not chunk into the accumulator" client.c \
    "        if (take > space) take = space;" "        ;"

run_mutation "gap detection runs before the first sample" client.c \
    "                if (c->have_prev_counter) {" "                if (1) {"

run_mutation "EOF is not a reconnect" client.c \
    "            if (r == 0) {                        /* orderly shutdown by the daemon */
                c->link_broken = 1;
                return GZ_CLIENT_RECONNECT;
            }" \
    "            if (r == 0) {
                break;
            }"

run_mutation "SIGPIPE not suppressed on a dead peer" client.c \
    "        ssize_t w = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);" \
    "        ssize_t w = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, 0);"

run_mutation "adopted fd left blocking" client.c \
    "    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {" \
    "    if (flags < 0 || fcntl(fd, F_SETFL, flags) < 0) {"

run_mutation "connect closes an fd it did not open" client.c \
    "    gz_client_init(c);

    if (path == NULL) { errno = EINVAL; return -1; }" \
    "    gz_client_close(c);
    gz_client_init(c);

    if (path == NULL) { errno = EINVAL; return -1; }"

run_mutation "overlong socket path truncated instead of refused" client.c \
    "    if (len == 0 || len >= sizeof a.sun_path) {" "    if (len == 0 || len >= sizeof a.sun_path + 512) {"

run_mutation "socket path spelled differently from the daemon" client.c \
    "\"%s/tobiifreed/gaze.sock\"" "\"%s/tobiifreed/gaze.socket\""

run_mutation "socket path truncation not detected" client.c \
    "    if (n < 0 || (size_t)n >= cap) return -1;" "    if (n < 0) return -1;"

# Expected to survive: gz_client_flush and gz_client_send both perform the same
# reset before they touch the queue, so no caller can observe the difference.
# Kept because the postcondition is worth stating where the bytes leave.
run_mutation "outbound queue never resets (unreachable)" client.c \
    "    if (c->out_off == c->out_len) { c->out_off = 0; c->out_len = 0; }" "    ;" ALLOW_SURVIVE

run_mutation "protocol version mismatch ignored" client.c \
    "                c->version_mismatch = (st.protocol_version != GZ_PROTOCOL_VERSION);" \
    "                c->version_mismatch = 0;"

run_mutation "err frame does not end a request" client.c \
    "        if (c->have_error) return GZ_CLIENT_REMOTE;" "        ;"

run_mutation "any response accepted as the answer" client.c \
    "            if (c->resp_cmd == cmd) return 0;" "            return 0;"

run_mutation "returning device does not re-arm the watchdog" client.c \
    "                if (st.device_present && was_absent) c->last_gaze_ns = now_ns;" "                ;"

run_mutation "every status re-arms the watchdog" client.c \
    "                if (st.device_present && was_absent) c->last_gaze_ns = now_ns;" \
    "                c->last_gaze_ns = now_ns;"

run_mutation "watchdog window doubled" client.h \
    "#define GZ_WATCHDOG_NS 1000000000ULL" "#define GZ_WATCHDOG_NS 2000000000ULL"

run_mutation "caller watchdog interval ignored" client.c \
    "    if (now_ns - c->last_gaze_ns <= interval_ns) return GZ_LINK_OK;" \
    "    if (now_ns - c->last_gaze_ns <= GZ_WATCHDOG_NS) return GZ_LINK_OK;"

run_mutation "adopt keeps the caller's fd when handed a bad one" client.c \
    "        gz_client_init(c);
        errno = EBADF;
        return -1;" \
    "        errno = EBADF;
        return -1;"

run_mutation "adopt leaks the fd it owns on a failed subscribe" client.c \
    "        int e = errno;
        gz_client_close(c);
        gz_client_init(c);
        errno = e;
        return -1;
    }
    return 0;
}" \
    "        return -1;
    }
    return 0;
}"

run_mutation "watchdog wraps on a backwards clock" client.c \
    "    if (now_ns <= c->last_gaze_ns) return GZ_LINK_OK;" "    ;"

run_mutation "absent device reported as a broken link" client.c \
    "    if (c->have_status && !c->status.device_present) return GZ_LINK_STALE;" "    ;"

run_mutation "stale and present inverted" client.c \
    "    if (c->have_status && !c->status.device_present) return GZ_LINK_STALE;" \
    "    if (c->have_status && c->status.device_present) return GZ_LINK_STALE;"

# The brief's own wording: "1 s with no valid gaze frame". The device streams
# at 33.2 Hz whether or not it sees eyes, so arming on validity reconnects
# every time the user looks away, and the reconnect does not help.
run_mutation2 "watchdog armed by valid gaze only" \
    client.c "                c->last_gaze_ns = now_ns;" \
             "                ;" \
    client.c "                if (gz_sample_any_eye_valid(&s)) c->last_valid_gaze_ns = now_ns;" \
             "                if (gz_sample_any_eye_valid(&s)) { c->last_valid_gaze_ns = now_ns; c->last_gaze_ns = now_ns; }"

# Expected to survive: proto.c caps a response payload at GZ_MAX_PAYLOAD and
# strips the cmd_type byte, so body_len can never exceed sizeof c->resp. Kept
# to document that the clamp is defence in depth rather than live logic.
run_mutation "response length clamp removed (unreachable)" client.c \
    "            if (n > sizeof c->resp) n = sizeof c->resp;" "            ;" ALLOW_SURVIVE

echo
echo "== calibration mutations =="

TEST_MAIN="tests/test_calibrate.c"
TEST_SRCS="src/calibrate.c src/display.c src/client.c src/proto.c"

# The stimulus offset. This is the mutation that matters most on this machine:
# the plan, the brief and CLAUDE.md all name a monitor at +4000+0 while the
# real one is at +4000+1440, and dropping the offset draws every dot 1440 px
# from where the eye is looking without any later step being able to tell.
run_mutation "stimulus ignores the output offset" calibrate.c \
    "    *px = s->x + (int)lround(fx);
    *py = s->y + (int)lround(fy);" \
    "    *px = (int)lround(fx);
    *py = (int)lround(fy);"

run_mutation "stimulus rounds without the half-pixel" calibrate.c \
    "    double fx = nx * (double)s->w - 0.5;
    double fy = ny * (double)s->h - 0.5;" \
    "    double fx = nx * (double)s->w;
    double fy = ny * (double)s->h;"

run_mutation "stimulus not clamped to the output" calibrate.c \
    "    if (fx > (double)(s->w - 1)) fx = (double)(s->w - 1);" "    ;"

run_mutation "NaN falls through the clamp" calibrate.c \
    "    if (!(fx == fx)) fx = (double)s->w / 2.0;" "    ;"

run_mutation "point grid transposed" calibrate.c \
    "    {0.1, 0.5}, {0.5, 0.5}, {0.9, 0.5}," "    {0.5, 0.1}, {0.5, 0.5}, {0.5, 0.9},"

# The CRC. A polynomial that only agrees with itself round-trips fine and
# disagrees with every other reader of the file.
run_mutation "CRC polynomial changed" calibrate.c \
    "0xEDB88320u" "0x82F63B78u"

run_mutation "CRC not checked on load" calibrate.c \
    "    if (got != crc) {" "    if (0) {"

run_mutation "blob magic not checked" calibrate.c \
    "    if (magic != GZ_BLOB_MAGIC) {" "    if (0) {"

run_mutation "blob format version not checked" calibrate.c \
    "    if (version != GZ_BLOB_VERSION) {" "    if (0) {"

# Bounded against the caller's buffer instead of the device's 4096-byte
# out_scratch, which is the one bound that means anything.
run_mutation "load bound against the buffer, not the device" calibrate.c \
    "    if (len == 0 || len > GZ_CAL_BLOB_MAX) {
        fprintf(stderr, \"%s: declares %u bytes, outside 1..%d\\n\", path, len, GZ_CAL_BLOB_MAX);
        fclose(f);
        return -1;
    }" "    ;"

run_mutation "load tolerates a file longer than it declares" calibrate.c \
    "    if (trailing) {" "    if (0) {"

run_mutation "save bound raised past the device scratch" calibrate.c \
    "    if (n == 0 || n > GZ_CAL_BLOB_MAX) {
        fprintf(stderr, \"calibration blob is %zu bytes, outside 1..%d: refusing to save\\n\"," \
    "    if (0) {
        fprintf(stderr, \"calibration blob is %zu bytes, outside 1..%d: refusing to save\\n\","

run_mutation "save truncates in place instead of renaming" calibrate.c \
    "    if (snprintf(tmp, sizeof tmp, \"%s.tmp\", path) >= (int)sizeof tmp) {" \
    "    if (snprintf(tmp, sizeof tmp, \"%s\", path) >= (int)sizeof tmp) {"

# The sequence.
run_mutation "finish blob bound removed" calibrate.c \
    "    if (c->resp_len == 0 || c->resp_len > GZ_CAL_BLOB_MAX) {" "    if (0) {"

run_mutation "an empty finish blob accepted" calibrate.c \
    "    if (c->resp_len == 0 || c->resp_len > GZ_CAL_BLOB_MAX) {" \
    "    if (c->resp_len > GZ_CAL_BLOB_MAX) {"

run_mutation "cal_apply length not checked" calibrate.c \
    "    if (n == 0 || n > GZ_CAL_BLOB_MAX) {
        say(\"cal_apply refused: %zu bytes is outside 1..%d\\n\", n, GZ_CAL_BLOB_MAX);
        return -1;
    }" "    ;"

run_mutation "a failed start still trains the points" calibrate.c \
    "    if (rc != 0) {
        say(\"start_calibration failed, nothing was calibrated\\n\");
        return rc;
    }" "    ;"

run_mutation "a point is sent even when the dot never appeared" calibrate.c \
    "            if (stim->show(stim->ctx, nx, ny) != 0) {" "            if (0) {"

run_mutation "the eyes-open gate never fails" calibrate.c \
    "            if (o->min_valid_frac <= 0 || frac >= o->min_valid_frac) break;" "            break;"

run_mutation "validity divided by every frame, not the inspected ones" calibrate.c \
    "    return (double)s->n_both / (double)s->n_seen;" \
    "    return s->n_total ? (double)s->n_both / (double)s->n_total : 0.0;"

run_mutation "one lucky frame passes the eyes-open gate" calibrate.c \
    "    if (s->n_seen < GZ_CAL_MIN_SEEN) return 0.0;" \
    "    if (s->n_seen < 1) return 0.0;"

run_mutation "both eyes relaxed to either eye" calibrate.c \
    "        if (gz_sample_both_eyes_valid(g)) s->n_both++;" \
    "        s->n_both++;"

# usb_busy is the one retryable code: nothing reached the device, so the
# connection is still in step. Everything else, and every timeout, is not.
run_mutation "every error code retried" calibrate.c \
    "            if (gz_err_retryable(c->err_code) && attempt + 1 < GZ_CAL_RETRIES) {" \
    "            if (attempt + 1 < GZ_CAL_RETRIES) {"

run_mutation "usb_busy not retried" calibrate.c \
    "            if (gz_err_retryable(c->err_code) && attempt + 1 < GZ_CAL_RETRIES) {" \
    "            if (0) {"

run_mutation "a timeout is retried on the same connection" calibrate.c \
    "            return rc;
        }

        say(\"  %s: request failed (%d)\\n\", cmd_name(cmd), rc);" \
    "            continue;
        }

        say(\"  %s: request failed (%d)\\n\", cmd_name(cmd), rc);"

run_mutation "reconnect skips the status re-gate" calibrate.c \
    "                } else if (gz_display_gate_status(c, GZ_CLIENT_CMD_TIMEOUT_MS) != 0) {" \
    "                } else if (0) {"

run_mutation "a repeated frame counter counted twice" calibrate.c \
    "        if (have_last && g->frame_counter == last_counter) continue;" "        ;"

run_mutation "an unavailable gap reported as zero" calibrate.c \
    "    if (c->gaze_frames == 0) return -1;" "    if (c->gaze_frames == 0) return 0;"

run_mutation "median taken from the unsorted array" calibrate.c \
    "    qsort(v, n, sizeof *v, cmp_double);" "    ;"

echo
echo "== gaze correction mutations (judged by test_proto) =="
TEST_MAIN="tests/test_proto.c"
TEST_SRCS="src/proto.c"

# Normalised y grows DOWNWARD while tracker y grows upward. A flipped
# projection still yields a plausible gain and a wrong offset, so nothing
# downstream would notice.
run_mutation "eye projection y not inverted" proto.c \
    "    out[1] = (area.oy_mm + area.h_mm - eye_mm[1]) / area.h_mm;" \
    "    out[1] = (eye_mm[1] - area.oy_mm) / area.h_mm;"

run_mutation "eye projection ignores the area origin" proto.c \
    "    out[0] = (eye_mm[0] - area.ox_mm) / area.w_mm;" \
    "    out[0] = eye_mm[0] / area.w_mm;"

# The whole point of the stage: dividing by g removes the gain, multiplying
# doubles it.
run_mutation "correction multiplies by the gain" proto.c \
    "    v[0] = (reported[0] - c->bx) / c->gx;" \
    "    v[0] = (reported[0] - c->bx) * c->gx;"

run_mutation "correction ignores the offset" proto.c \
    "    v[1] = (reported[1] - c->by) / c->gy;" \
    "    v[1] = reported[1] / c->gy;"

# The measured decision. An eye term here is form H, which spec test 5.3 scored
# at 69 px against form S's 53 at a displaced seat.
run_mutation "an eye term reintroduced on the correction path" proto.c \
    "    if (!gz_sample_any_eye_valid(s)) return 0;

    gz_correct_point(c, s->gaze_point_2d_norm, out);" \
    "    if (!gz_sample_any_eye_valid(s)) return 0;

    double eye[3], ep[2];
    struct gz_eye_state e_;
    gz_eye_state_init(&e_);
    if (!gz_sample_eye_mid(&e_, s, eye)) return 0;
    gz_eye_proj(c->area, eye, ep);
    double r_[2] = { s->gaze_point_2d_norm[0] + ep[0] * (c->gx - 1.0),
                     s->gaze_point_2d_norm[1] + ep[1] * (c->gy - 1.0) };
    gz_correct_point(c, r_, out);"

run_mutation "an eyeless frame corrected anyway" proto.c \
    "    if (!gz_sample_any_eye_valid(s)) return 0;" "    ;"

run_mutation "the form field not checked" proto.c \
    "    if (c->form != GZ_CORR_FORM_STATIC) return 0;" "    ;"

run_mutation "a form H file accepted by version" proto.c \
    "    if (version != (double)GZ_CORRECTION_VERSION) return GZ_CORR_PARSE_STALE;" "    ;"

run_mutation "an unknown form accepted on parse" proto.c \
    "    if ((seen & (1u << CK_FORM)) && form != (double)GZ_CORR_FORM_STATIC)
        return GZ_CORR_PARSE_STALE;" "    ;"

run_mutation "a stale file reported as malformed" proto.c \
    "    if (version != (double)GZ_CORRECTION_VERSION) return GZ_CORR_PARSE_STALE;" \
    "    if (version != (double)GZ_CORRECTION_VERSION) return GZ_CORR_PARSE_MALFORMED;"

run_mutation "an unfitted correction is applied anyway" proto.c \
    "    if (!c->valid) return 0;" "    ;"

# 3.1: using the lone eye jumps E_proj by half an interpupillary distance.
run_mutation "one-eye frame uses the lone eye" proto.c \
    "        for (int i = 0; i < 3; i++) v[i] = s->eye_origin_L_mm[i] + e->lr_mm[i] * 0.5;" \
    "        for (int i = 0; i < 3; i++) v[i] = s->eye_origin_L_mm[i];"

run_mutation "one-eye frame reconstructs with the wrong sign" proto.c \
    "        for (int i = 0; i < 3; i++) v[i] = s->eye_origin_R_mm[i] - e->lr_mm[i] * 0.5;" \
    "        for (int i = 0; i < 3; i++) v[i] = s->eye_origin_R_mm[i] + e->lr_mm[i] * 0.5;"

run_mutation "eye midpoint reconstructed before it has seen both eyes" proto.c \
    "    } else if (!e->have_lr) {" "    } else if (0) {"

# do-not #4. present_mask is 0x003fffff in EVERY frame, eyeless ones included,
# where the eye-origin fields read as a clean 0.0.
run_mutation "eye midpoint gates on present_mask" proto.c \
    "    int have_l = gz_eye_valid(s->validity_L);" \
    "    int have_l = (s->present_mask & GZ_BIT_EYE_ORIGIN_L) != 0;"

run_mutation "validity inverted in the eye midpoint" proto.c \
    "    int have_r = gz_eye_valid(s->validity_R);" \
    "    int have_r = !gz_eye_valid(s->validity_R);"

# 5.5 invariants. These are the load-bearing guard, not the outlier rule.
run_mutation "gain envelope not checked" proto.c \
    "    if (!(c->gx >= GZ_CORR_G_MIN) || !(c->gx <= GZ_CORR_G_MAX)) return 0;" "    ;"

run_mutation "isotropy not checked" proto.c \
    "    if (!(fabs(c->gx / c->gy - 1.0) < GZ_CORR_ISO_TOL)) return 0;" "    ;"

# Documented survivor. The two spellings agree on every real number and differ
# only on NaN, and a NaN gain is already refused one line below: the isotropy
# test is !(fabs(gx/gy - 1) < TOL), and NaN loses that comparison too. The
# spelling is kept because it should not depend on a neighbouring guard.
run_mutation "gain check written so NaN passes" proto.c \
    "    if (!(c->gy >= GZ_CORR_G_MIN) || !(c->gy <= GZ_CORR_G_MAX)) return 0;" \
    "    if (c->gy < GZ_CORR_G_MIN || c->gy > GZ_CORR_G_MAX) return 0;" "documented"

run_mutation "a whole-screen offset accepted" proto.c \
    "    if (!(fabs(c->bx) < 1.0) || !(fabs(c->by) < 1.0)) return 0;" "    ;"

# The text format. A decimal parsed through a reciprocal does not round trip:
# 12 * 0.1 is 1.2000000000000002 while 12 / 10 is 1.2.
run_mutation "decimals scaled by a reciprocal" proto.c \
    "    double v = e10 >= 0 ? mant * pow10i(e10) : mant / pow10i(-e10);" \
    "    double v = e10 >= 0 ? mant * pow10i(e10) : mant * (1.0 / pow10i(-e10));"

run_mutation "trailing zeros not trimmed" proto.c \
    "    while (nfrac > 0 && frac[nfrac - 1] == '0') nfrac--;" "    ;"

run_mutation "parse accepts a missing key" proto.c \
    "    if (seen != (1u << CK_COUNT) - 1u) return GZ_CORR_PARSE_MALFORMED;" "    ;"

run_mutation "parse accepts a repeated key" proto.c \
    "        if (seen & (1u << idx)) return GZ_CORR_PARSE_MALFORMED;" "        ;"


run_mutation "parse accepts junk after a number" proto.c \
    "        if (p != buf + le) return GZ_CORR_PARSE_MALFORMED;  /* trailing junk */" \
    "        ;"

run_mutation "parse accepts a line that is not key=value" proto.c \
    "        if (eq == le) return GZ_CORR_PARSE_MALFORMED;   /* a line that is not key=value */" \
    "        if (eq == le) continue;"

run_mutation "parse stores parameters outside the envelope" proto.c \
    "    if (!gz_correction_check(&c)) {" "    if (0) {"

run_mutation "format truncates instead of refusing" proto.c \
    "        size_t w = fmt_num(buf + n, cap - n, vals[k]);
        if (w == 0) return 0;" \
    "        size_t w = fmt_num(buf + n, cap - n, vals[k]);
        if (w == 0) { buf[n] = 0; return n; }"

run_mutation "format writes a non-finite parameter" proto.c \
    "    if (!(v > -GZ_NUM_MAX && v < GZ_NUM_MAX)) return 0;" \
    "    if (v > GZ_NUM_MAX || v < -GZ_NUM_MAX) return 0;"

echo
echo "== correction fit and file mutations (judged by test_calibrate) =="
TEST_MAIN="tests/test_calibrate.c"
TEST_SRCS="src/calibrate.c src/display.c src/client.c src/proto.c"

# do-not #3. The head drifts during a sweep; averaging folds that drift into
# the parameters themselves.
# Form S regresses reported on target. Reintroducing the eye is form H.
run_mutation "the fit made head-aware again" calibrate.c \
    "        vx[i] = in[i].target[0];" \
    "        { double ep_[2]; gz_eye_proj(area, in[i].eye_mm, ep_); vx[i] = in[i].target[0] - ep_[0]; }"

run_mutation "the fitted seat not recorded" calibrate.c \
    "    out->eye_proj[0] = rep->eye_proj[0];" "    out->eye_proj[0] = 0.0;"

run_mutation "fit regresses the wrong way round" calibrate.c \
    "        if (!ols(vx, ux, use, n, &gx, &bx)) return GZ_FIT_ERR_FLAT;" \
    "        if (!ols(ux, vx, use, n, &gx, &bx)) return GZ_FIT_ERR_FLAT;"

run_mutation "fit never rejects an outlier" calibrate.c \
    "        if (drops == 0) break;" "        break;"

run_mutation "fit refits around two outliers" calibrate.c \
    "            return GZ_FIT_ERR_OUTLIER;" "            (void)0;"

run_mutation "fit skips the envelope check" calibrate.c \
    "    if (!gz_correction_check(out)) return GZ_FIT_ERR_BOUNDS;" "    ;"

run_mutation "fit rejects a perfect sweep against a zero median" calibrate.c \
    "        if (!(med > 1e-6)) break;" "        ;"

# The post-refit guard. Disabling it restores the absorbed opposite-axis
# outlier, which is a 5.7 percent gain error no other guard sees.
run_mutation "the post-refit outlier accepted" calibrate.c \
    "            if (rep->refit_outlier >= 0) {" "            if (0) {"

# Its median floor. Without it the eight survivors of a clean single-outlier
# sweep sit past 3x a median of picometres and refuse themselves.
run_mutation "the post-refit guard has no median floor" calibrate.c \
    "            if (med2 > 1e-6) {" "            if (1) {"

run_mutation "fit accepts a degenerate point count" calibrate.c \
    "    if (n < 4 || n > GZ_CAL_POINTS) return GZ_FIT_ERR_POINTS;" "    ;"

# 4.2. A correction fitted against one geometry says nothing about another.
run_mutation "correction loaded without the display-area gate" calibrate.c \
    "    unsigned d = gz_rect_diff(c.area, want, GZ_DA_TOL_MM, GZ_DA_TOL_DEG);" \
    "    unsigned d = 0; (void)want;"

# Documented survivor. Removing this branch changes which message is printed,
# not the outcome: a file that does not parse also cannot present a matching
# display area, so the 4.2 gate below refuses it anyway and the caller still
# gets -1. The diagnostic is the only thing lost.
run_mutation "a malformed correction file loads as absent" calibrate.c \
    "    if (r == GZ_CORR_PARSE_MALFORMED) {" "    if (0) {" "documented"

run_mutation "an out-of-envelope correction file is applied" calibrate.c \
    "    if (r == GZ_CORR_PARSE_BOUNDS) {" "    if (0) {"

run_mutation "an overlong correction file is read truncated" calibrate.c \
    "    if (overlong) {" "    if (0) {"

# The fit pair must come from frames that carried both, or the two means
# describe two different head positions.
run_mutation "gaze paired with an eye it never had" calibrate.c \
    "        if (have_gaze && gz_sample_eye_mid(e, g, mid)) {" \
    "        if (have_gaze) {"

# The diagnostic that decides what to tell a human whose sweep lost points.
# Getting it wrong sends them to the light switch when they are simply sitting
# too close, which has already cost a five-minute round trip.
run_mutation "proximity never blamed" calibrate.c \
    "    if (nz == 0 || !(med < GZ_FIT_TOO_CLOSE_MM)) return GZ_MISS_LIGHTS;" \
    "    return GZ_MISS_LIGHTS;"

run_mutation "proximity blamed at the playing distance" calibrate.c \
    "    if (nz == 0 || !(med < GZ_FIT_TOO_CLOSE_MM)) return GZ_MISS_LIGHTS;" \
    "    if (nz == 0) return GZ_MISS_LIGHTS;"

run_mutation "the top row is not distinguished" calibrate.c \
    "    if (top_row_lost > 0 && other_lost == 0) return GZ_MISS_TOO_CLOSE;" \
    "    if (top_row_lost > 0) return GZ_MISS_TOO_CLOSE;"

run_mutation "the eyeless zero counted as a distance" calibrate.c \
    "            if (eye_z[i] != 0.0) z[nz++] = fabs(eye_z[i]);" \
    "            z[nz++] = fabs(eye_z[i]);"

run_mutation "residual provenance written in millimetres" calibrate.c \
    "                            \"fit_median_resid_px=%.1f\\nfit_worst_resid_px=%.1f\\n\"," \
    "                            \"fit_median_resid_mm=%.1f\\nfit_worst_resid_mm=%.1f\\n\","


# The sweep verdicts. Nothing prints them yet, so a wrong field would sit there
# until the setup view drew it, which is where it would be hardest to see.
run_mutation "fit verdict does not mark a refusal" calibrate.c \
    "        out->refused = 1;
        /* The same point the terminal names, and the same one-based number. */" \
    "        out->refused = 0;
        /* The same point the terminal names, and the same one-based number. */"

run_mutation "fit verdict names the wrong refit point" calibrate.c \
    "gz_fit_err_text(fit_rc), rep->refit_outlier + 1);" \
    "gz_fit_err_text(fit_rc), rep->refit_outlier);"

# The same defect the say_missing_points mutations above cover, on the path a
# window reads instead of the terminal.
run_mutation "missing verdict never blames proximity" calibrate.c \
    "    if (cause == GZ_MISS_TOO_CLOSE) {
        snprintf(out->next, sizeof out->next," \
    "    if (0) {
        snprintf(out->next, sizeof out->next,"

run_mutation "accuracy verdict judges one degree against the wrong number" calibrate.c \
    "    out->within_one_degree = cs->median_px <= GZ_ACC_TARGET_PX;" \
    "    out->within_one_degree = cs->median_px <= 2 * GZ_ACC_TARGET_PX;"

# The middle band splits on the predicted cost in pixels, which is what the
# acceptance line prints. Judging the millimetres against the same 15 calls a
# 20 mm move a lost seat and tells the human to re-fit for nothing.
run_mutation "accuracy verdict judges the seat in millimetres" calibrate.c \
    "    double moved_px = cs->moved_mm >= 0.0 ? cs->moved_mm * GZ_CORR_DEGRADE_PX_PER_MM : -1;" \
    "    double moved_px = cs->moved_mm;"

run_mutation "corrected stats skip the worst point" calibrate.c \
    "        if (cors[n] > worst) worst = cors[n];" \
    "        if (0) worst = cors[n];"


# ---------------------------------------------------------------------------
# record.c, the trace recorder.
#
# Only the pure half is mutated. gz_cmd_record drives a socket, a file and a
# signal handler, and tests/test_record.c has no harness for any of the three,
# so a mutation there would survive and be counted as an unexpected survivor
# rather than telling anyone anything. What IS mutated is the half that decides
# what a row says, which is where a defect would be invisible: a trace with a
# wrong corr_* column looks exactly like a good one until Task 16 fits the
# overlay filter to the wrong signal.
TEST_MAIN="tests/test_record.c"
TEST_SRCS="src/record.c src/calibrate.c src/display.c src/client.c src/proto.c"

run_mutation "validity inverted for the left eye" record.c \
    "        if (gz_eye_valid(s->validity_L)) {" \
    "        if (!gz_eye_valid(s->validity_L)) {"

run_mutation "validity inverted for the right eye" record.c \
    "        if (gz_eye_valid(s->validity_R)) {" \
    "        if (!gz_eye_valid(s->validity_R)) {"

run_mutation "the left corrected point taken from the combined field" record.c \
    "            gz_correct_point(corr, s->gaze_point_2d_L_norm, cl);" \
    "            gz_correct_point(corr, s->gaze_point_2d_norm, cl);"

run_mutation "the right corrected point taken from the left eye" record.c \
    "            gz_correct_point(corr, s->gaze_point_2d_R_norm, cr);" \
    "            gz_correct_point(corr, s->gaze_point_2d_L_norm, cr);"

run_mutation "an eyeless frame's combined point corrected anyway" record.c \
    "        hc = gz_gaze_correct(corr, s, cc);" \
    "        hc = 1;"

run_mutation "a correction that was never loaded applied anyway" record.c \
    "    if (c == NULL || !c->valid) return 0;" \
    "    if (c == NULL) return 0;"

run_mutation "the caller's buffer bound dropped" record.c \
    "    if ((size_t)n >= cap) return 0;" \
    "    if (0) return 0;"

run_mutation "an oversize row truncated instead of refused" record.c \
    "    if (n < 0 || (size_t)n >= sizeof row) return 0;" \
    "    if (n < 0) return 0;"

run_mutation "the device stamp printed unsigned" record.c \
    "                     \"%llu,%lld,%u,%u,%u,%u,\"" \
    "                     \"%llu,%llu,%u,%u,%u,%u,\""

run_mutation "the corrected columns dropped from the header" record.h \
    "    \"corr_lx,corr_ly,corr_rx,corr_ry,corr_cx,corr_cy,\" \\" \
    "    \"\" \\"

run_mutation "a missing correction recorded anyway" record.c \
    "    if (load_rc == 1) return GZ_REC_CORRECTED;" \
    "    return GZ_REC_CORRECTED;"

run_mutation "--raw ignored" record.c \
    "    if (raw) return GZ_REC_RAW;" \
    "    if (0) return GZ_REC_RAW;"

# The six eye-origin columns. Form S is scoped per seating position, so a trace
# that loses where the head was cannot be re-analysed for head movement without
# costing the human another five-minute session.
run_mutation "the eye origins dropped from the header" record.h \
    "    \"eye_lx,eye_ly,eye_lz,eye_rx,eye_ry,eye_rz\\n\"" \
    "    \"\\n\""

run_mutation "the right eye origin read from the left" record.c \
    "                     s->eye_origin_R_mm[0], s->eye_origin_R_mm[1], s->eye_origin_R_mm[2]);" \
    "                     s->eye_origin_L_mm[0], s->eye_origin_L_mm[1], s->eye_origin_L_mm[2]);"

run_mutation "the eye origins corrected like a gaze point" record.c \
    "                     s->eye_origin_L_mm[0], s->eye_origin_L_mm[1], s->eye_origin_L_mm[2]," \
    "                     s->eye_origin_L_mm[0] / 1.18, s->eye_origin_L_mm[1], s->eye_origin_L_mm[2],"

# The Important finding of review round 1: a --raw recording over a path that
# once held a corrected trace left the old .correction.conf in place, so an
# uncorrected trace sat beside a file claiming a fit produced it.
run_mutation "raw branch leaves a stale provenance file" record.c \
    "    if (unlink(dest) == 0) {" \
    "    if (0) {"

run_mutation "a missing provenance file reported as a failure" record.c \
    "    if (errno == ENOENT) return 0;" \
    "    if (0) return 0;"

run_mutation "the provenance name built without the suffix" record.c \
    "    int n = snprintf(buf, cap, \"%s.correction.conf\", trace_path);" \
    "    int n = snprintf(buf, cap, \"%s\", trace_path);"

run_mutation "the provenance name truncated instead of refused" record.c \
    "    if (n < 0 || (size_t)n >= cap) return -1;" \
    "    if (n < 0) return -1;"

# The per-eye divide. gz_correct_point divides without checking, so without the
# gain test the two halves of one row disagree: inf in corr_lx, nan in corr_cx.
run_mutation "the per-eye path divides by an unchecked gain" record.c \
    "    return c->gx != 0.0 && c->gy != 0.0;" \
    "    return 1;"

echo
echo "== setup view mutations =="

# The pure half of the view. The frame loop is not mutated: it owns the window
# and the socket and no test reaches it, so a mutation there would survive and
# say nothing. What is mutated is everything a human reads off the screen and
# everything that decides when a sweep runs, because a wrong dwell or a client
# left open costs a session rather than a frame.
TEST_MAIN="tests/test_view.c"
TEST_SRCS="src/view.c src/calibrate.c src/display.c src/client.c src/proto.c"

run_mutation "the box is not a third of the screen" view.c \
    "    out->box_h = scr->h / 3;" "    out->box_h = scr->h / 4;"

run_mutation "eye dots not clamped to the box" view.c \
    "    if (box_x > 1.0) box_x = 1.0;" "    if (0) box_x = 1.0;"

# The dots followed the head backwards until 2026-09-01: the device counts box
# x towards the user's left, like a camera image.
run_mutation "eye dots not mirrored" view.c \
    "    *px = l->box_x + l->box_w - (int)lround(box_x * l->box_w);" \
    "    *px = l->box_x + (int)lround(box_x * l->box_w);"

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

echo
echo "killed=$killed  documented_survivors=$documented  unexpected_survivors=$unexpected"
[ "$unexpected" -eq 0 ]
