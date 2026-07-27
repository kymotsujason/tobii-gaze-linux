#!/usr/bin/env bash
# Mutation harness for test_proto.c.
#
# Breaks proto.c or proto.h one way at a time and requires the suite to fail.
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
    cp "$SRC/src/proto.c" "$SRC/src/proto.h" "$SRC/src/client.c" "$SRC/src/client.h" "$WORK/m/src/"
    cp "$SRC/tests/test_proto.c" "$SRC/tests/test_client.c" "$WORK/m/tests/"
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
    if ! out=$($CC $CFLAGS -o "$WORK/m/t" "$WORK/m/$TEST_MAIN" $srcs 2>&1); then
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
