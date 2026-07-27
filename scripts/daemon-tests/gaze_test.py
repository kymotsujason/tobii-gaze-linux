#!/usr/bin/env python3
"""Exercise tobiifreed: gaze ring integrity under command load, multi-client fairness."""
import socket, struct, sys, threading, time, os

SOCK = os.environ.get("TOBII_SOCK", "/run/user/1000/tobiifreed/gaze.sock")
HDR = 5
GAZE_SIZE = 392

SUBSCRIBE = bytes([0x01]) + struct.pack("<I", 0)
GET_DA = bytes([0x02]) + struct.pack("<I", 0)


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    return s


class Reader:
    """Incremental daemon-protocol frame reader."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def frames(self, timeout):
        self.sock.settimeout(timeout)
        try:
            chunk = self.sock.recv(1 << 16)
        except socket.timeout:
            return
        except OSError:
            return
        if not chunk:
            return
        self.buf += chunk
        while len(self.buf) >= HDR:
            mtype = self.buf[0]
            plen = struct.unpack("<I", self.buf[1:5])[0]
            if len(self.buf) < HDR + plen:
                break
            payload = self.buf[HDR:HDR + plen]
            self.buf = self.buf[HDR + plen:]
            yield mtype, payload


def subscriber(name, duration, out):
    s = connect()
    s.sendall(SUBSCRIBE)
    r = Reader(s)
    counters = []
    times = []
    t_end = time.time() + duration
    while time.time() < t_end:
        for mtype, payload in r.frames(0.5):
            if mtype == 0x01 and len(payload) == GAZE_SIZE:
                fc = struct.unpack("<I", payload[4:8])[0]
                counters.append(fc)
                times.append(time.time())
    s.close()
    out[name] = {"counters": counters, "times": times}


def pipeliner(name, n, out, budget=120.0):
    s = connect()
    s.sendall(GET_DA * n)
    r = Reader(s)
    resp = 0
    errs = {}
    first = None
    t0 = time.time()
    last_progress = t0
    while resp < n and time.time() - t0 < budget:
        got = False
        for mtype, payload in r.frames(1.0):
            got = True
            if mtype == 0x02:
                resp += 1
                if first is None:
                    first = time.time() - t0
            elif mtype == 0xFF:
                code = struct.unpack("<I", payload[:4])[0]
                errs[code] = errs.get(code, 0) + 1
                resp += 1
        if got:
            last_progress = time.time()
        elif time.time() - last_progress > 20.0:
            break
    s.close()
    out[name] = {"sent": n, "got": resp, "errs": errs,
                 "elapsed": time.time() - t0, "first": first}


def analyse(counters):
    """frame_counter advances by exactly 4 per delivered sample."""
    bad = 0
    dups = 0
    backwards = 0
    deltas = {}
    for a, b in zip(counters, counters[1:]):
        d = (b - a) & 0xFFFFFFFF
        if d == 4:
            continue
        bad += 1
        signed = d - (1 << 32) if d > 0x80000000 else d
        deltas[signed] = deltas.get(signed, 0) + 1
        if d == 0:
            dups += 1
        if d > 0x80000000:
            backwards += 1
    return {"n": len(counters), "unique": len(set(counters)), "bad_delta": bad,
            "dups": dups, "backwards": backwards, "bad_deltas": deltas}


def main():
    mode = sys.argv[1]
    out = {}
    if mode == "ring":
        # One subscriber, one pipeliner. Ring wrap shows up as frame_counter
        # deltas that are not +4 in the subscriber's stream.
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 500
        dur = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
        ts = threading.Thread(target=subscriber, args=("sub", dur, out))
        ts.start()
        time.sleep(2.0)
        tp = threading.Thread(target=pipeliner, args=("pipe", n, out))
        tp.start()
        tp.join()
        ts.join()
        print("pipe:", out["pipe"])
        print("gaze:", analyse(out["sub"]["counters"]))
    elif mode == "twoclient":
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 500
        threads = [threading.Thread(target=pipeliner, args=(f"c{i}", n, out))
                   for i in range(2)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        for k in sorted(out):
            print(k, out[k])
    elif mode == "gaze":
        dur = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
        subscriber("sub", dur, out)
        st = analyse(out["sub"]["counters"])
        tms = out["sub"]["times"]
        rate = (len(tms) - 1) / (tms[-1] - tms[0]) if len(tms) > 1 else 0
        print("gaze:", st, "rate=%.2f Hz" % rate)
    elif mode == "cmdlatency":
        # Single command on an otherwise idle daemon: measures the loop's
        # service latency, not USB time.
        s = connect()
        r = Reader(s)
        lats = []
        for _ in range(20):
            t0 = time.time()
            s.sendall(GET_DA)
            deadline = time.time() + 5
            while time.time() < deadline:
                done = False
                for mtype, _p in r.frames(1.0):
                    if mtype in (0x02, 0xFF):
                        lats.append(time.time() - t0)
                        done = True
                        break
                if done:
                    break
            time.sleep(0.05)
        s.close()
        lats.sort()
        print("n=%d min=%.1fms med=%.1fms max=%.1fms" %
              (len(lats), lats[0] * 1e3, lats[len(lats) // 2] * 1e3, lats[-1] * 1e3))
    else:
        print("usage: gaze_test.py ring|twoclient|gaze|cmdlatency ...")


if __name__ == "__main__":
    main()
