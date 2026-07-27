# tobiifreed as a systemd user unit

```bash
scripts/build.sh                        # the unit refuses to install without a binary
scripts/install-systemd.sh              # render and daemon-reload, nothing else
systemctl --user start tobiifreed       # run it now
systemctl --user enable tobiifreed      # and with every graphical session
scripts/install-systemd.sh --uninstall  # remove it again
```

`systemd/tobiifreed.service.in` is a template, not a unit. `install-systemd.sh`
substitutes the absolute path of the built daemon into `ExecStart` so the unit
follows the checkout instead of assuming it lives in `~/Documents`. Copying the
`.in` file into `~/.config/systemd/user` by hand gives a unit that fails with
`203/EXEC`.

It is a **user** unit. The socket is `$XDG_RUNTIME_DIR/tobiifreed/gaze.sock`,
the udev rule already grants unprivileged access to `2104:0313`, and the daemon
creates the socket `0755` and owned by whoever runs it, so a system unit would
put it out of reach of the desktop session.

`ExecStart` points into `vendor/tobiifree/.../zig-out/bin/`. That path survives
`scripts/build.sh`, which resets the submodule with `git clean -fd` and cannot
remove `zig-out` because it is gitignored. A copy under `~/.local/bin` was
rejected: it would go stale after every rebuild, silently.

## Why the restart policy looks the way it does

A daemon that loses the tracker while running never exits. It tears the USB
session down and retries in process, so `Restart=` cannot fire and cannot cut a
live session short.

A daemon that cannot open the tracker **at startup** exits with status 0. That
is why `Restart=always` and not `on-failure`: `on-failure` would never fire, and
the unit would sit `inactive` looking like a clean stop while the tracker went
unclaimed. The cost is that a busy or absent tracker at startup produces a
restart loop. `RestartSteps` spreads it out over 2.25, 3.5, 6.0, 10.25, 17.5 and
then 30 s, matching the daemon's own reconnect backoff, so the steady state is
one attempt and four journal lines per 30 s. Recovery after the tracker becomes
available takes up to 30 s.

`StartLimitIntervalSec=0` is for the operator, not the restart loop. The backoff
above already stays under the default limiter, but a fifth `systemctl --user
restart` inside 10 s latches a default unit into `start-limit-hit`, which only
`reset-failed` clears. Rebuild-and-restart cycles hit that.

## Diagnosing

`systemctl --user status tobiifreed` and the journal carry everything. Two
daemons cannot share the tracker: whichever loses prints

```
error(usb): claim_interface failed (device busy?)
error(tobiifreed): failed to open USB: error.ClaimInterface
```

and exits without touching the running daemon's socket. `systemd` already
prevents two instances of the unit itself, so this only happens against a
hand-started daemon.

The socket appears 59 to 71 ms after `systemctl --user start` returns. The unit
is `Type=exec`, which reports execve failures but not readiness, so a script
that starts the unit and connects immediately has to retry.

Idle cost is four journal lines a minute, about 350 KB of message text a day.
The daemon logs at `.log_level = .debug` with no way to turn it down, but the
volume is one gaze line per 500 samples, which at 33.2 Hz is one per 15 s.

## Not here yet

The unit has no `ExecStartPost`. Phase 1's plan called for
`gaze-cal --apply-saved` to reapply calibration after the socket binds, but
nothing persists a calibration blob to disk yet: the daemon's `saved_cal` is a
process-local buffer for replaying across a reconnect, and `gaze-cal` has no
such subcommand. Task 13 decides whether persistence is possible at all.
