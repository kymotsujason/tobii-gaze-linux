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

## Session lifecycle

`After=` and `PartOf=graphical-session.target` follow the pattern in
`~/.config/systemd/user/alpha-overlay.service`. `WantedBy=graphical-session.target`
does not, since that unit is `WantedBy=default.target`; the `[Install]` half
follows `opentabletdriver.service`, `osu-ar-gamma.service` and
`xmousepasteblock.service`, which are the three units already sitting in
`graphical-session.target.wants`.

`PartOf=` is belt-and-braces rather than the thing that stops the daemon at
logout. `loginctl show-user jason -p Linger` reports `Linger=no`, so the whole
user manager and everything under it goes away when the session ends, and the
USB claim is released either way. `PartOf=` matters only if lingering is ever
turned on, or if the Plasma session restarts without the manager restarting.

## Why the restart policy looks the way it does

The daemon handles the two ways it can lose the tracker very differently, and
the unit exists to paper over the gap. This is a recorded decision, not an
oversight.

A daemon that loses the tracker **while running** never exits. It tears the USB
session down and retries in process with a 2 s cap for the first minute and 30 s
after, so `Restart=` cannot fire and cannot cut a live session short.

A daemon that cannot open the tracker **at startup** exits with status 0
(`main.zig:721`). That is why `Restart=always` and not `on-failure`: `on-failure`
would never fire, and the unit would sit `inactive` looking like a clean stop
while the tracker went unclaimed. `RestartSteps` spreads the retries over 2.25,
3.5, 6.0, 10.25, 17.5 and then 30 s, so the steady state is one attempt and four
journal lines per 30 s.

Two things that costs, both measured, both accepted:

- Recovery takes **up to 30 s** after the tracker becomes available, against the
  2 s the in-process path would manage in the first minute.
- **Every connected client is dropped on each cycle**, because the process
  really does exit and the socket really does go away. The in-process path keeps
  them attached and just reports `device_present=0`.

Fixing the exit alone would not buy back the second one. `Server.init` binds the
socket at `main.zig:756`, after the USB open, so a daemon that retried its own
boot still would not accept a client while the tracker was absent. Getting the
full benefit needs the socket bound **before** the device is opened, which is a
larger reorder than the exit code. That reorder is scoped to **Phase 2**, where
the OBS plugin's actual requirements can define what readiness has to mean,
rather than being guessed at now.

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

**`Type=exec` is right today and will not stay right.** It is only adequate
because nothing orders itself after this unit. The first unit that declares
`After=tobiifreed.service`, or the first `ExecStartPost` added here, makes
`Type=notify` mandatory, and that means adding an `sd_notify("READY=1")` to the
daemon after `Server.init`. There is no way to fake it from the unit file:
`Type=notify` with `NotifyAccess=all` plus an `ExecStartPost` that sends
`systemd-notify --ready` deadlocks, since `ExecStartPost` does not run until the
ready notification has already arrived.

Idle cost is four journal lines a minute, about 350 KB of message text a day.
The daemon logs at `.log_level = .debug` with no way to turn it down, but the
volume is one gaze line per 500 samples, which at 33.2 Hz is one per 15 s.

## Not here yet

The unit has no `ExecStartPost`. Phase 1's plan called for
`gaze-cal --apply-saved` to reapply calibration after the socket binds, but
nothing persists a calibration blob to disk yet: the daemon's `saved_cal` is a
process-local buffer for replaying across a reconnect, and `gaze-cal` has no
such subcommand. Task 13 decides whether persistence is possible at all.
