#!/usr/bin/env python3
"""Race a response against fd recycling.

Client A sends get_display_area and vanishes before the tracker answers. The
daemon closes A's fd, client B connects and the kernel hands it the same fd
number. If routing is keyed on the raw fd, B receives A's response.

B subscribes, so a correct daemon sends it only status (0x04) and gaze (0x01).
Any response (0x02) or error (0xFF) B sees belongs to A.
"""
import socket, struct, sys, os, time

SOCK = os.environ.get("TOBII_SOCK", "/run/user/1000/tobiifreed/gaze.sock")
SUBSCRIBE = bytes([0x01]) + struct.pack("<I", 0)
GET_DA = bytes([0x02]) + struct.pack("<I", 0)


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    return s


def frames(buf):
    out = []
    while len(buf) >= 5:
        plen = struct.unpack("<I", buf[1:5])[0]
        if len(buf) < 5 + plen:
            break
        out.append((buf[0], buf[5:5 + plen]))
        buf = buf[5 + plen:]
    return out, buf


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    listen = float(sys.argv[2]) if len(sys.argv) > 2 else 0.12
    # Gap between A closing and B connecting. Long enough that the daemon has
    # reaped A's fd, short enough that the tracker has not answered yet.
    gap = float(sys.argv[3]) if len(sys.argv) > 3 else 0.002
    # "sub" makes A subscribe before its command, so the next gaze broadcast
    # hits a closed socket and the daemon reaps A's slot while the reply is
    # still in flight. That frees the slot for B before the reply lands, which
    # is the case the generation check exists for.
    sub = len(sys.argv) > 4 and sys.argv[4] == "sub"
    stray = 0
    seen = {}
    for i in range(rounds):
        a = connect()
        a.sendall((SUBSCRIBE if sub else b"") + GET_DA)
        a.close()
        time.sleep(gap)
        b = connect()
        b.sendall(SUBSCRIBE)
        buf = b""
        t_end = time.time() + listen
        while time.time() < t_end:
            b.settimeout(max(0.0, t_end - time.time()))
            try:
                chunk = b.recv(1 << 16)
            except (socket.timeout, OSError):
                break
            if not chunk:
                break
            buf += chunk
            msgs, buf = frames(buf)
            for mtype, _p in msgs:
                seen[mtype] = seen.get(mtype, 0) + 1
                if mtype in (0x02, 0xFF):
                    stray += 1
        b.close()
    print("rounds=%d stray_responses=%d opcodes=%s"
          % (rounds, stray, {hex(k): v for k, v in sorted(seen.items())}))
    return 1 if stray else 0


if __name__ == "__main__":
    sys.exit(main())
