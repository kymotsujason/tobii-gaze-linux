#!/usr/bin/env python3
"""Drive a USB re-enumeration under a live subscriber and report the gap."""
import os, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gaze_test as g

HERE = os.path.dirname(os.path.abspath(__file__))
PID = int(sys.argv[1])
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
RESET_AT = 6.0

state = {"samples": []}


def sub():
    s = g.connect()
    s.sendall(g.SUBSCRIBE)
    r = g.Reader(s)
    t_end = time.time() + DURATION
    while time.time() < t_end:
        for mtype, payload in r.frames(0.5):
            if mtype == 0x01 and len(payload) == g.GAZE_SIZE:
                import struct
                fc = struct.unpack("<I", payload[4:8])[0]
                state["samples"].append((time.time(), fc))
    s.close()


def ticks():
    with open("/proc/%d/stat" % PID) as f:
        p = f.read().split()
    return int(p[13]) + int(p[14])


t0 = time.time()
th = threading.Thread(target=sub)
th.start()
time.sleep(RESET_AT)

print("[%5.1fs] firing USB reset" % (time.time() - t0), flush=True)
before = ticks()
t_reset = time.time()
out = subprocess.run([os.path.join(HERE, "reset_tobii")],
                     capture_output=True, text=True)
print(out.stdout.strip(), out.stderr.strip(), flush=True)

# CPU over the outage window: a spinning reader shows up here.
time.sleep(10.0)
after = ticks()
print("[%5.1fs] CPU over the 10s after reset: %.2f%% of a core"
      % (time.time() - t0, (after - before) / 100.0 / 10.0 * 100), flush=True)

th.join()

s = state["samples"]
print("total gaze frames: %d, distinct: %d" % (len(s), len(set(fc for _, fc in s))))
gaps = []
for (ta, _), (tb, _) in zip(s, s[1:]):
    if tb - ta > 0.5:
        gaps.append((ta - t0, tb - t0, tb - ta))
for a, b, d in gaps:
    print("gap: %.2fs .. %.2fs  (%.2fs)" % (a, b, d))
if s:
    print("first sample after reset: +%.2fs"
          % (min(t for t, _ in s if t > t_reset) - t_reset
             if any(t > t_reset for t, _ in s) else float("nan")))
print("frame_counter integrity:", g.analyse([fc for _, fc in s]))
