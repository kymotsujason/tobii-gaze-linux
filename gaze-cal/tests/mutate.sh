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

pass=0
fail=0

fresh_copy() {
    rm -rf "$WORK/m"; mkdir -p "$WORK/m/src" "$WORK/m/tests"
    cp "$SRC/src/proto.c" "$SRC/src/proto.h" "$WORK/m/src/"
    cp "$SRC/tests/test_proto.c" "$WORK/m/tests/"
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
    if ! out=$($CC $CFLAGS -o "$WORK/m/t" "$WORK/m/tests/test_proto.c" "$WORK/m/src/proto.c" 2>&1); then
        echo "  CAUGHT   $name  (compile-time)"
        pass=$((pass+1)); return
    fi
    out=$("$WORK/m/t" 2>&1); rc=$?
    if [ $rc -ne 0 ]; then
        detail=$(echo "$out" | grep -m1 -E 'Assertion|runtime error|ERROR: ' | sed 's/^ *//' | cut -c1-100)
        echo "  CAUGHT   $name  (rc=$rc) $detail"
        pass=$((pass+1))
    elif [ -n "$allow_survive" ]; then
        echo "  SURVIVED $name  (expected: unreachable given the other guard)"
        pass=$((pass+1))
    else
        echo "  SURVIVED $name  <-- the suite does not detect this"
        fail=$((fail+1))
    fi
}

run_mutation() {
    fresh_copy
    apply_edit "$2" "$3" "$4" || { fail=$((fail+1)); return; }
    judge "$1" "${5:-}"
}

# Two edits at once, for guards that only matter in combination.
run_mutation2() {
    fresh_copy
    apply_edit "$2" "$3" "$4" || { fail=$((fail+1)); return; }
    apply_edit "$5" "$6" "$7" || { fail=$((fail+1)); return; }
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

# Neither guard alone is provably load-bearing, so drop both and require the
# near-ceiling test to still catch the resulting 32-bit wrap.
run_mutation2 "plen bound AND widening both removed" \
    proto.c "if (plen > GZ_MAX_PAYLOAD) return GZ_ERR_DESYNC;" \
            "if (plen > 0xFFFFFFFEu) return GZ_ERR_DESYNC;" \
    proto.c "size_t total = (size_t)GZ_HEADER_SIZE + (size_t)plen;" \
            "size_t total = (size_t)(GZ_HEADER_SIZE + plen);"

echo
echo "caught=$pass survived_unexpectedly=$fail"
[ "$fail" -eq 0 ]
