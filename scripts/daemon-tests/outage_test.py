#!/usr/bin/env python3
"""Prolonged tracker absence: reset, hold the interface, then release it.

Checks CPU during the outage, command behaviour while the device is gone, and
that gaze resumes afterwards.
"""
import os, struct, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gaze_test as g

HERE = os.path.dirname(os.path.abspath(__file__))
PID = int(sys.argv[1])
HOLD = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

samples = []
cmd_log = []
stop = threading.Event()


def sub():
    s = g.connect()
    s.sendall(g.SUBSCRIBE)
    r = g.Reader(s)
    while not stop.is_set():
        for mtype, payload in r.frames(0.3):
            if mtype == 0x01 and len(payload) == g.GAZE_SIZE:
                samples.append((time.time(), struct.unpack("<I", payload[4:8])[0]))
    s.close()


def commander():
    """One command per second, recording what came back."""
    s = g.connect()
    r = g.Reader(s)
    while not stop.is_set():
        t = time.time()
        s.sendall(g.GET_DA)
        got = None
        deadline = time.time() + 3.0
        while time.time() < deadline and got is None:
            for mtype, payload in r.frames(0.5):
                if mtype == 0x02:
                    got = "response"
                elif mtype == 0xFF:
                    got = "err%d" % struct.unpack("<I", payload[:4])[0]
                if got:
                    break
        cmd_log.append((t, got or "timeout", time.time() - t))
        time.sleep(0.4)
    s.close()


def ticks():
    with open("/proc/%d/stat" % PID) as f:
        p = f.read().split()
    return int(p[13]) + int(p[14])


t0 = time.time()
threads = [threading.Thread(target=sub), threading.Thread(target=commander)]
for t in threads:
    t.start()
time.sleep(4.0)

holder = subprocess.Popen([os.path.join(HERE, "hold_tobii"), str(HOLD)],
                          stdout=subprocess.PIPE, text=True)
time.sleep(0.3)
print("[%5.1fs] firing USB reset" % (time.time() - t0), flush=True)
subprocess.run([os.path.join(HERE, "reset_tobii")], capture_output=True)

time.sleep(1.0)
c0, w0 = ticks(), time.time()
time.sleep(HOLD - 2.0)
c1, w1 = ticks(), time.time()
print("[%5.1fs] CPU during the outage (%.1fs window): %.2f%% of a core"
      % (time.time() - t0, w1 - w0, (c1 - c0) / 100.0 / (w1 - w0) * 100), flush=True)

print(holder.communicate()[0].strip(), flush=True)
t_release = time.time()
time.sleep(6.0)
stop.set()
for t in threads:
    t.join()

back = [t for t, _ in samples if t > t_release]
print("gaze resumed %.2fs after release" % (back[0] - t_release) if back
      else "GAZE DID NOT RESUME")
print("total gaze %d, distinct %d" % (len(samples), len(set(f for _, f in samples))))
print("integrity:", g.analyse([f for _, f in samples]))
print("commands during the run:")
for t, res, dt in cmd_log:
    print("  t=%5.1fs  %-10s  %.3fs" % (t - t0, res, dt))
