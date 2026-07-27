# Physical test checklist: unplug and replug

Produced by the Task 8 review and corrected by its re-review. These are the things no
agent can test, because they need a hand on the cable. Everything else in Task 8 was
verified by forcing a real USB port reset from a second process, which produces a genuine
`LIBUSB_ERROR_NO_DEVICE` against the shipped binary with no fault injection, and drove 8
real re-enumeration recoveries. What a port reset cannot do is remove VBUS, so **nothing
below about a power cycle has ever been exercised.**

**Item 4 is the load-bearing one.** It decides whether a whole branch of the reconnect
path is dead code or the critical path, and it also decides how much Task 13's persistence
experiment has left to answer.

## Setup

Start the daemon, and in a second terminal run
`python3 scripts/daemon-tests/gaze_test.py gaze 600`. Watch the daemon's stderr.

## The checklist

- [ ] **1. Record the baseline geometry.** Note the `device display: TL=... TR=... BL=...`
      line from a fresh start. It must read 597 x 336 mm, that is `TL=(-298.5,346,0)`,
      `TR=(298.5,346,0)`, `BL=(-298.5,10,0)`. Everything below compares against this.

- [ ] **2. Physically unplug the tracker.** Confirm within about 1 s: `recv fatal: libusb
      r=-4`, `deinit`, `session closed`, `USB connection lost, reopening`, then
      `USB reopen attempt 1 failed (DeviceNotFound), down 0s, next try in 200ms`.

- [ ] **3. Leave it unplugged for 60 s**, watching `top -p $(pgrep tobiifreed)`. CPU must
      stay under 1%. Expect exactly 4 lines in the first 60 s: one `USB connection lost`
      and three `USB reopen attempt N failed`. The first once-a-minute line lands at about
      60.7 s. Anything above about 5 lines is a regression.

- [ ] **4. Replug. THIS IS THE DECIDING TEST.** Watch which branch the reconnect takes.
      - `device display area preserved across reconnect` means the ET5 keeps its geometry
        across a power cycle, and the replay branch stays dead code.
      - `replaying display area after reconnect` means a power cycle clears it and the
        replay branch is the critical path.
      Either result is valuable. This is the single fact that decides how much of the
      unplug-recovery patch actually matters.

- [ ] **5. Verify geometry after the replug.** Re-read it and compare against step 1 byte
      for byte.

- [ ] **6. Confirm gaze resumes** within about 2 s, and that `frame_counter` deltas are all
      +4 across the gap.

- [ ] **7. Issue a command after the replug**, for example
      `python3 scripts/daemon-tests/gaze_test.py cmdlatency`. It must return responses
      rather than time out. This was the direct test for a Critical that has since been
      fixed, so it should now pass either way. If it times out, the fix regressed.

- [ ] **8. Unplug for 30 minutes, then replug.** Confirms the backoff cap and the log
      volume over a realistic absence, and that recovery still works after a long gap.
      Expect the backoff to reach its 30 s cap after the first minute, and about 33 log
      lines over the 30 minutes rather than the 1800 an earlier build would have produced.

- [ ] **9. Unplug during a calibration**, once Task 13's calibration client exists. This is
      the untested interaction that matters most for Task 13, because a fatal error lands
      on the main thread inside `drainReads` under an acknowledged pause, rather than in
      the USB thread's `poll()`.

- [ ] **10. If a calibration exists at the time, check whether it survives the power
      cycle.** This decides whether applying a saved calibration is a convenience or a
      requirement.

## Known gaps this checklist does not cover

- **Bootloader-mode re-enumeration.** `LibusbTransport.init` only looks for `2104:0313`,
  while `2104:0102` is the bootloader PID. A tracker that comes back in bootloader mode
  retries forever at the cap. The per-attempt log line now names `DeviceNotFound`, so it
  is at least visible. About ten lines of code whenever someone wants it handled.
- **Cable flapping.** `reconnect()` resets its attempt counter, backoff and downtime per
  call, so a cable flapping every few seconds emits about 5 lines per flap with no
  cross-event rate limit.
