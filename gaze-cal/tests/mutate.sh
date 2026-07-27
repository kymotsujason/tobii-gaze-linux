#!/usr/bin/env bash
# Mutation harness for the gaze-cal test suites.
#
# Breaks proto.c, client.c or display.c one way at a time and requires the
# matching suite to fail.
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
       "$SRC/src/display.c" "$SRC/src/display.h" "$WORK/m/src/"
    cp "$SRC/tests/test_proto.c" "$SRC/tests/test_client.c" "$SRC/tests/test_display.c" \
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
echo "killed=$killed  documented_survivors=$documented  unexpected_survivors=$unexpected"
[ "$unexpected" -eq 0 ]
