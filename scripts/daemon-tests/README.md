# tobiifreed daemon tests

Reproduction harness for the Task 8 measurements. Everything here talks to a
running daemon over `/run/user/1000/tobiifreed/gaze.sock`, or forces USB faults
from a second process. **The daemon binary is never modified**, so what these
measure is what ships.

Integrity is checked against `frame_counter`, which advances by exactly 4 per
delivered gaze sample. A delta that is not +4 means a sample was lost, replayed
or torn.

## Build the USB fault tools

```bash
ZIG=/nix/store/9ljn49hx3a2lhha1anl3agivwb3z0ga1-zig-0.15.2/bin/zig
LIBUSB_DEV=/nix/store/xgvlkhckizm184d1n8qndi9phqlk8bfi-libusb-1.0.29-dev
LIBUSB_LIB=/nix/store/rqflzdkgggyp4cvp7hlwwjdyph62shik-libusb-1.0.29
for t in reset_tobii hold_tobii; do
  "$ZIG" cc -o "$t" "$t.c" -I"$LIBUSB_DEV/include/libusb-1.0" -L"$LIBUSB_LIB/lib" -lusb-1.0
done
```

## gaze_test.py

```bash
python3 gaze_test.py gaze 15          # integrity and rate on an idle daemon
python3 gaze_test.py ring 500 45      # gaze integrity under a 500-command batch
python3 gaze_test.py twoclient 500    # two clients pipelining, fairness
python3 gaze_test.py cmdlatency       # main-loop service latency
```

`ring` is the gaze ring overrun test. On an unpatched daemon it reports fewer
distinct frames than delivered frames plus backwards `frame_counter` jumps of
-252, which is one lap of a 64-slot ring. Fixed, delivered equals distinct and
`bad_delta` is 0.

`twoclient` shows head-of-line blocking: the second client's first response
arrives exactly when the first client's batch ends.

## reset_tobii

Opens the tracker from a second process and calls `libusb_reset_device`. The
kernel re-enumerates, the daemon's transfers fail with `LIBUSB_ERROR_NO_DEVICE`,
and its recovery path runs for real. This is the closest root-free stand-in for
a replug. It does **not** remove VBUS, so it does not test a power cycle.

**Do not run it while a calibration is in progress.** A re-enumeration in the
middle of `cal_apply` writing a blob is the one place a reset could plausibly
damage stored device state. Nothing in the harness writes to the device, but
this is the exception worth respecting.

**Read the geometry back before and after a session that uses it.** The device
was once seen reporting a 4x4 mm display area on the start following a session
containing eight `reset_tobii` runs. Unexplained and never reproduced, but it is
the one correlation to know about. The `device display: TL=... TR=... BL=...`
line at daemon start is the check, and it must read
`TL=(-299,346,0) TR=(299,346,0) BL=(-299,10,0)`.

## hold_tobii

Claims interface 0 and holds it for N seconds, so the daemon's reopen keeps
failing and its retry loop and exponential backoff run for a controlled
duration. Use with `reset_tobii` to simulate a tracker that stays away.

Run it **before** the daemon starts and the daemon's own boot fails with
`claim_interface failed (device busy?)` and exits. Harmless, but confusing if
you were not expecting it. Start the daemon first.

The two compiled binaries are gitignored.

## replug_test.py / outage_test.py

```bash
python3 replug_test.py <daemon-pid> 40      # one reset, measure the gaze gap
python3 outage_test.py <daemon-pid> 14      # 14s absence: CPU, commands, recovery
```

`outage_test.py` also drives one command per 0.4 s throughout, so the
`usb_busy` refusals during the outage and the return to normal after it are
both visible.

## status_test.py

Task 9. Covers the status message, the bounded dispatch and the backpressure
timeout.

```bash
python3 status_test.py connect        # first frame on a new connection is status
python3 status_test.py strand 200     # send a batch, go quiet; nothing stranded
python3 status_test.py halfclose 100  # send a batch, shutdown(SHUT_WR); nothing lost
python3 status_test.py wedge 90       # subscriber that never reads is disconnected
python3 status_test.py idle 45        # unsubscribed silent client is NOT disconnected
python3 status_test.py watch 40       # print status transitions; pair with reset_tobii
```

`wedge` detects the close with `POLLHUP` rather than `recv`. The kernel holds
about 200 KB of gaze for a client that is not reading, so a reading test would
un-wedge itself and keep seeing data long after the daemon had let it go.

`halfclose` is the case that found the hold counter: without it the daemon
released the slot as soon as the input drained, and the reply to the last
command, which arrives later on the USB thread, was dropped. 99 of 100.

## recycle_test.py

Task 9, P9. Client A sends a command and vanishes before the tracker answers.
The daemon reaps A, client B connects into the same fd and slot, and the reply
lands. Routing keyed on a raw fd hands it to B.

```bash
python3 recycle_test.py 2000 0.03 0.0025        # A closes without subscribing
python3 recycle_test.py 500 0.03 0.0025 sub     # A subscribes first
```

The third argument is the gap between A closing and B connecting, and it has to
be tuned: too short and the daemon has not reaped A yet, so B gets a different
fd, and too long and the reply has already been delivered. Measured on the
fd-keyed daemon, 2000 rounds: 0 strays at 1 ms, **21 at 2.5 ms**, 0 at 20 ms.

The `sub` variant makes A subscribe, so the next gaze broadcast hits a closed
socket and the daemon reaps A's slot while the reply is still in flight. That is
what forces the generation check to fire rather than the hold counter, and it
shows up as `dropping N bytes for departed client (slot 0, gen N != N+1)`.
