#!/usr/bin/env python3
"""Check the device status message (Srv.status = 0x04).

modes:
  connect            first frame on a fresh connection must be status
  strand <n>         send n commands then go quiet; all n must still be answered
  halfclose <n>      send n commands then shutdown(SHUT_WR); all n must be answered
  wedge <seconds>    subscribe and never read; the daemon must disconnect us
  idle <seconds>     connect, do not subscribe, do not read; must stay connected
  watch <seconds>    print every status transition (pair with reset_tobii)
"""
import select, socket, struct, sys, os, time

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


def mode_connect():
    s = connect()
    s.settimeout(2.0)
    data = s.recv(65536)
    msgs, _ = frames(data)
    mtype, payload = msgs[0]
    print("first opcode: 0x%02x  payload=%s" % (mtype, payload.hex()))
    assert mtype == 0x04, "first frame was not status"
    assert len(payload) == 3, "status payload is not 3 bytes"
    print("device_present=%d calibration_applied=%d protocol_version=%d"
          % (payload[0], payload[1], payload[2]))
    s.close()
    print("PASS")


def mode_strand(n):
    """Send a batch, then never send another byte. Nothing may be stranded."""
    s = connect()
    s.sendall(GET_DA * n)
    got, buf, t0 = 0, b"", time.time()
    s.settimeout(2.0)
    while got < n and time.time() - t0 < 180:
        try:
            chunk = s.recv(1 << 16)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        msgs, buf = frames(buf)
        for mtype, _p in msgs:
            if mtype in (0x02, 0xFF):
                got += 1
    s.close()
    print("sent=%d answered=%d elapsed=%.1fs" % (n, got, time.time() - t0))
    print("PASS" if got == n else "FAIL: %d stranded" % (n - got))
    return 0 if got == n else 1


def mode_watch(seconds):
    """Subscribe and print every status transition. Run reset_tobii alongside:
    the daemon must report device_present=0 on the loss and 1 on recovery."""
    s = connect()
    s.sendall(SUBSCRIBE)
    buf, t0, gaze, last = b"", time.time(), 0, None
    while time.time() - t0 < seconds:
        s.settimeout(1.0)
        try:
            chunk = s.recv(1 << 16)
        except socket.timeout:
            continue
        if not chunk:
            print("%.1fs daemon closed the connection" % (time.time() - t0))
            return 1
        buf += chunk
        msgs, buf = frames(buf)
        for mtype, payload in msgs:
            if mtype == 0x01:
                gaze += 1
            elif mtype == 0x04:
                cur = (payload[0], payload[1])
                print("%6.1fs status present=%d cal=%d version=%d (gaze so far %d)"
                      % (time.time() - t0, payload[0], payload[1], payload[2], gaze))
                last = cur
    s.close()
    print("gaze frames: %d, final status: %s" % (gaze, last))
    return 0


def mode_halfclose(n):
    """Send a batch, half-close the write side, then read. The daemon sees EOF
    with the batch still buffered, and must serve it before releasing the slot."""
    s = connect()
    s.sendall(GET_DA * n)
    s.shutdown(socket.SHUT_WR)
    got, buf, t0 = 0, b"", time.time()
    s.settimeout(2.0)
    while got < n and time.time() - t0 < 180:
        try:
            chunk = s.recv(1 << 16)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        msgs, buf = frames(buf)
        for mtype, _p in msgs:
            if mtype in (0x02, 0xFF):
                got += 1
    s.close()
    print("sent=%d answered=%d elapsed=%.1fs" % (n, got, time.time() - t0))
    print("PASS" if got == n else "FAIL: %d lost to the half-close" % (n - got))
    return 0 if got == n else 1


def mode_wedge(seconds):
    """Subscribe, then never read a byte. The daemon should let us go on its
    backpressure timeout.

    Detection is by POLLHUP, not by recv: the kernel has ~200KB of gaze buffered
    for us, so a reading test would both un-wedge itself and see data long after
    the daemon had gone."""
    s = connect()
    s.sendall(SUBSCRIBE)
    # Shrink the receive window so the kernel stops absorbing sooner.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    t0 = time.time()
    poller = select.poll()
    poller.register(s.fileno(), select.POLLHUP | select.POLLERR)
    while time.time() - t0 < seconds:
        for _fd, ev in poller.poll(200):
            if ev & (select.POLLHUP | select.POLLERR):
                print("disconnected by daemon after %.1fs (revents=0x%x)"
                      % (time.time() - t0, ev))
                print("PASS")
                return 0
    print("FAIL: still connected after %.1fs" % (time.time() - t0))
    return 1


def mode_idle(seconds):
    """Connect without subscribing and read nothing. Must survive."""
    s = connect()
    t0 = time.time()
    while time.time() - t0 < seconds:
        time.sleep(0.5)
    # Prove the connection still works by asking for something.
    s.sendall(GET_DA)
    s.settimeout(3.0)
    buf = b""
    try:
        while True:
            chunk = s.recv(1 << 16)
            if not chunk:
                print("FAIL: daemon closed an idle unsubscribed client")
                return 1
            buf += chunk
            msgs, buf = frames(buf)
            for mtype, _p in msgs:
                if mtype in (0x02, 0xFF):
                    print("still served after %.1fs idle, reply 0x%02x" % (time.time() - t0, mtype))
                    print("PASS")
                    return 0
    except socket.timeout:
        print("FAIL: no reply after %.1fs idle" % (time.time() - t0))
        return 1


def main():
    mode = sys.argv[1]
    if mode == "connect":
        mode_connect()
        return 0
    if mode == "strand":
        return mode_strand(int(sys.argv[2]))
    if mode == "halfclose":
        return mode_halfclose(int(sys.argv[2]))
    if mode == "watch":
        return mode_watch(float(sys.argv[2]))
    if mode == "wedge":
        return mode_wedge(float(sys.argv[2]))
    if mode == "idle":
        return mode_idle(float(sys.argv[2]))
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
