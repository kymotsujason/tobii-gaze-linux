# Tobii Gaze Overlay, Phase 1: Bring-up and Daemon Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get calibrated gaze coordinates streaming reliably from a Tobii ET5 on Linux, and record a real gaze trace to refit the overlay's signal-processing constants against.

**Architecture:** Vendor `tobiifree` at a pinned commit, apply a nine-patch series that fixes a USB thread race, a client-disconnect bug, undersized calibration buffers, and missing recovery paths. Build a `gaze-cal` tool that owns display-area configuration, calibration, blob persistence, and live preview. Nothing touches OBS in this phase.

**Tech Stack:** Zig 0.15.x (via `nix develop`, which pins the exact toolchain in `flake.lock`), C11 for `gaze-cal`, libusb 1.0, systemd user units, Python 3 for the trace analysis harness.

**Source spec:** `docs/superpowers/specs/2026-07-26-tobii-obs-overlay-design.md`. Section numbers below refer to it.

## Why this is Plan 1 of 3

Spec section 13 lists three unknowns. All three resolve in this phase, and all three change what Plans 2 and 3 must say:

| Unknown | Resolved by | What it changes downstream |
|---|---|---|
| Does the ET5 stream gaze on Linux without Windows provisioning? | Task 3 | If no, the project stops. Everything else is moot. |
| Does calibration survive a replug? | Task 13 | Decides whether P5's bootstrap replay and `gaze-cal --apply-saved` are mandatory or merely harmless. |
| What are the real noise, saccade and blink characteristics? | Task 16 | `beta` is declared unset in spec section 8. `dCutoff`, `minCutoff`, the gap threshold and the fusion EMA all refit against the recorded trace. |

**Plan 2** (OBS filter plugin) cannot be written until Task 16 produces a trace, because its filter constants are fitted to it and its shader constants depend on the thin-versus-wide ring decision.
**Plan 3** (verification harness, spec section 12) depends on Plan 2 existing.

## Global Constraints

- **Zig 0.15.x only.** Not 0.14 (`tobiifree_decode.zig:246` uses the unmanaged `std.ArrayList(u32) = .{}` form). Not 0.16 (removes `std.process.args`, `std.posix.getenv`, `std.fs.cwd`, all used at `main.zig:374`, `:256`, `:260`). The Arch package is 0.16, so use `nix develop`.
- **`driver/src/daemon_protocol.zig` at the pinned commit is the only source of truth for the wire protocol.** `ARCHITECTURE.md` is wrong about four opcodes, invents a `cal_retrieve` at `0x23` that is actually `cal_apply`, lists realm commands `0x10`-`0x13` that do not exist, and states the gaze payload is 232 bytes when it is 392.
- **Upstream is never edited in place.** Every daemon change is a numbered `.patch` file under `patches/`, applied by the build. This preserves rebasing onto upstream and keeps the maintenance surface visible.
- **Patch extraction workflow, used identically in Tasks 4 through 9.** `scripts/build.sh` applies all existing patches and then runs `git add -A` inside the vendor checkout. Make the task's edits on top of that state, then `git -C vendor/tobiifree diff` shows **only** the new work. Extract it, then re-run `scripts/build.sh` to prove the whole stack still applies from clean. Never `git checkout -- .` between tasks: that discards earlier patches from the working tree and makes the next diff capture them again.
- **Patches are numbered by dependency order, not by the order tasks are done.** The build applies `0000` through `0008` in filename order every time, so the numbering must be a valid application sequence. It is: `0000` calibration-buffers goes first because it was authored against the bare pin and every later patch is authored on top of it, `0001` single-USB-owner and `0002` write-path touch disjoint functions, `0003` through `0005` touch startup and the poll loop, `0007` builds on `0002`'s `enqueue`, and `0008` builds on `0007`.
- **`validity == 0` means valid.** Inverting this is an easy C bug.
- **Wire framing:** `[u8 msg_type][u32 LE payload_len][payload]`, header 5 bytes. **`response` (0x02) additionally prepends a one-byte `cmd_type`**, so its body starts at offset 6.
- **A client that does not send `subscribe` (0x01) receives zero gaze samples, with no error.** Every connect and every reconnect must send it.
- **Commands:** `subscribe 0x01`, `get_display_area 0x02`, `set_display_area 0x03`, `set_display_area_corners 0x04`, `start_calibration 0x20`, `add_calibration_point 0x21`, `finish_calibration 0x22`, `cal_apply 0x23`, `disconnect 0xFF`.
- **Hardware:** DP-1-2 is 2560x1440 at 597 x 336 mm, X11 offset +4000+0, 45 px per degree at 60 cm. Tracker VID:PID `2104:0313`, bulk EP `0x83` IN / `0x05` OUT.
- **Commit after every task.** Conventional commit prefixes: `feat:`, `fix:`, `test:`, `chore:`.

---

## File Structure

```
tobii-eye-tracker/
  vendor/tobiifree/            git submodule, pinned, never edited
  patches/                     P1..P9, applied by scripts/build.sh
    0000-calibration-buffers.patch
    0001-single-usb-owner.patch
    0002-write-path.patch
    0003-force-display-area.patch
    0004-unplug-recovery.patch
    0005-poll-loop.patch
    0007-device-status-message.patch
    0008-pending-entry-lifetime.patch
  scripts/
    build.sh                   apply patches, nix develop, zig build
    install-udev.sh            99-tobii.rules
  gaze-cal/
    src/proto.c  proto.h       wire framing, bounds checks, struct asserts
    src/client.c client.h      connect, subscribe, read loop, reconnect
    src/display.c              force + readback gate
    src/calibrate.c            stimulus sequence, blob save/load
    src/record.c               trace recorder
    src/main.c                 CLI
    tests/                     unit tests, no hardware needed
    Makefile
  systemd/
    tobiifreed.service
  tools/
    derive_visual_constants.py already exists, normative for spec section 9
    fit_filter.py              refits one-euro constants against a real trace
  traces/                      recorded gaze traces (gitignored except README)
```

`gaze-cal` is one binary with subcommands rather than several tools, because they share the protocol client and are always used together. `proto.c` is separate from `client.c` because Plan 2's OBS plugin will link `proto.c` directly and must not pull in the CLI.

---

## Task 1: Repository skeleton and pinned vendor

**Files:**
- Create: `.gitignore`, `README.md`, `scripts/build.sh`, `traces/README.md`
- Create: `vendor/tobiifree` (git submodule)

**Interfaces:**
- Consumes: nothing
- Produces: `scripts/build.sh` builds `vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed`; `$TOBIIFREE` env convention pointing at `vendor/tobiifree`

- [ ] **Step 1: Initialise the repo and ignore build output**

```bash
cd /home/jason/Documents/tobii-eye-tracker
git init
cat > .gitignore <<'EOF'
zig-out/
zig-cache/
.zig-cache/
gaze-cal/build/
traces/*
!traces/README.md
.superpowers/
result
EOF
mkdir -p traces
echo "Recorded gaze traces. Gitignored: they are large and machine-specific." > traces/README.md
```

- [ ] **Step 2: Pin tobiifree as a submodule**

```bash
git submodule add https://github.com/Aetherall/tobiifree.git vendor/tobiifree
cd vendor/tobiifree && git checkout $(git rev-parse HEAD) && cd ../..
git -C vendor/tobiifree rev-parse HEAD > vendor/PINNED_COMMIT
```

- [ ] **Step 3: Write the build script**

```bash
cat > scripts/build.sh <<'EOF'
#!/usr/bin/env bash
# Apply the patch series to the pinned vendor checkout, then build tobiifreed.
# Never edits vendor/ in place: patches are reapplied from a clean checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V="$ROOT/vendor/tobiifree"

git -C "$V" checkout -- . 2>/dev/null || true
git -C "$V" clean -fd 2>/dev/null || true

shopt -s nullglob
for p in "$ROOT"/patches/*.patch; do
  echo "applying $(basename "$p")"
  git -C "$V" apply --check "$p" || { echo "PATCH FAILED: $p" >&2; exit 1; }
  git -C "$V" apply "$p"
done

# Stage everything the patches did. New edits then show up as UNSTAGED changes,
# so `git diff` yields only the incremental patch for the task being worked on.
# Without this, extracting a patch would re-capture every earlier patch too.
git -C "$V" add -A

if command -v nix >/dev/null; then
  nix develop "$V" --command bash -c "cd '$V/applications/tobiifreed' && zig build -Doptimize=ReleaseSafe"
else
  echo "nix not found; falling back to system zig (must be 0.15.x)" >&2
  zig version | grep -q '^0\.15\.' || { echo "wrong zig version: $(zig version)" >&2; exit 1; }
  (cd "$V/applications/tobiifreed" && zig build -Doptimize=ReleaseSafe)
fi
echo "built: $V/applications/tobiifreed/zig-out/bin/tobiifreed"
EOF
chmod +x scripts/build.sh
```

- [ ] **Step 4: Verify the script fails cleanly with no patches present**

Run: `./scripts/build.sh`
Expected: either a successful build of unmodified upstream, or a clear toolchain error naming the Zig version. Do not proceed past a silent failure.

- [ ] **Step 5: Commit**

```bash
git add .gitignore README.md scripts/build.sh traces/README.md vendor/ .gitmodules
git commit -m "chore: pin tobiifree and add patch-applying build script"
```

---

## Task 2: udev rules and unprivileged device access

**Files:**
- Create: `scripts/install-udev.sh`

**Interfaces:**
- Consumes: `vendor/tobiifree/assets/99-tobii.rules`
- Produces: unprivileged read/write on `2104:0313`

Spec section 3 step 0. Without this, `LibusbTransport.init` fails with `DeviceNotFound` or `ClaimInterface` and it looks like broken hardware rather than permissions.

- [ ] **Step 1: Write the installer**

```bash
cat > scripts/install-udev.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
sudo cp "$ROOT/vendor/tobiifree/assets/99-tobii.rules" /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger
echo "Installed. UNPLUG AND REPLUG the tracker now, then re-run scripts/check-device.sh"
EOF
chmod +x scripts/install-udev.sh
```

- [ ] **Step 2: Write the access check**

```bash
cat > scripts/check-device.sh <<'EOF'
#!/usr/bin/env bash
# Confirms the tracker is present AND writable without root.
set -euo pipefail
line=$(lsusb | grep -i '2104:0313') || { echo "FAIL: tracker not enumerated"; exit 1; }
echo "found: $line"
bus=$(echo "$line" | sed 's/Bus \([0-9]*\).*/\1/')
dev=$(echo "$line" | sed 's/.*Device \([0-9]*\):.*/\1/')
node="/dev/bus/usb/$bus/$dev"
ls -l "$node"
[ -w "$node" ] && echo "PASS: writable without root" || { echo "FAIL: not writable, udev rule not applied or device not replugged"; exit 1; }
EOF
chmod +x scripts/check-device.sh
```

- [ ] **Step 3: Run the installer, replug, then verify**

Run: `./scripts/install-udev.sh`, physically replug the tracker, then `./scripts/check-device.sh`
Expected: `PASS: writable without root`

- [ ] **Step 4: Commit**

```bash
git add scripts/install-udev.sh scripts/check-device.sh
git commit -m "feat: udev rule installer and device access check"
```

---

## Task 3: THE SPIKE, prove gaze streams at all

**Files:**
- Create: `scripts/spike-first-sample.sh`

**Interfaces:**
- Consumes: `tobiifreed` from Task 1, device access from Task 2
- Produces: a yes/no answer to spec section 13 item 1

**This task gates the entire project.** If it fails, stop and report before writing any more code. It also resolves whether one-time Windows provisioning is required, which spec section 13 flags as unresolved because the absence of provisioning code in a read-only driver proves nothing.

- [ ] **Step 1: Generate the daemon config with the correct monitor geometry**

The shipped default is `w_mm: 800, h_mm: 340`, which is the tobiifree author's Acer Predator X34P ultrawide, not DP-1-2. Measure your monitor's active area with a tape measure before running this.

```bash
./vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed --init-config
python3 - <<'EOF'
import json, os, pathlib
p = pathlib.Path.home() / ".config/tobii.json"
c = json.loads(p.read_text())
c["display_area"] = {
    "w_mm": 597.0,     # DP-1-2 active area width, MEASURE YOURS
    "h_mm": 336.0,     # active area height
    "z_mm": 0.0,       # tracker plane to screen plane, MEASURE
    "tilt": 0.0,       # screen tilt in degrees, MEASURE
    "cx": "c",         # horizontal tracker position relative to screen
    "cy": "b - 10",    # vertical, MEASURE the mm below the screen edge
}
p.write_text(json.dumps(c, indent=2))
print(p.read_text())
EOF
```

`cx` and `cy` accept a number or an anchor expression, and the grammar is narrow:
`evalAnchorExpr` (`main.zig`) reads exactly ONE anchor character (`t`, `b`, `l`, `r` or
`c`) followed by an optional signed offset. Words like `"center"` and `"bottom"` parse to
null, and the value then falls back silently to the `DisplayArea` default of -750, which
is a wrong-but-plausible geometry of exactly the kind Task 5 exists to escape. Write `"c"`
or `"b - 10"`, never `"center"`.

- [ ] **Step 2: Write the spike script**

```bash
cat > scripts/spike-first-sample.sh <<'EOF'
#!/usr/bin/env bash
# Runs the daemon for 15 s and reports whether ANY gaze sample arrives.
# This is the go/no-go for the whole project.
set -uo pipefail
BIN="$(cd "$(dirname "$0")/.." && pwd)/vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed"
LOG=$(mktemp)
"$BIN" > "$LOG" 2>&1 &
PID=$!
sleep 15
kill $PID 2>/dev/null; wait $PID 2>/dev/null
echo "--- daemon log ---"; cat "$LOG"; echo "------------------"
if grep -q "gaze #" "$LOG"; then
  echo "PASS: gaze samples observed"
  grep -m3 "gaze #" "$LOG"
  exit 0
fi
grep -qi "failed to open USB"    "$LOG" && echo "FAIL: USB open failed (permissions? see Task 2)"
grep -qi "failed to connect"     "$LOG" && echo "FAIL: TTP handshake failed (possible Windows provisioning requirement)"
grep -qi "claim_interface"       "$LOG" && echo "FAIL: another process owns the device"
echo "FAIL: no gaze samples in 15 s"
exit 1
EOF
chmod +x scripts/spike-first-sample.sh
```

- [ ] **Step 3: Run the spike**

Run: `./scripts/spike-first-sample.sh`
Expected on success: `PASS: gaze samples observed` plus three lines like `gaze #1: vL=0 vR=0 x=0.512 y=0.433`

The daemon logs at `.log_level = .debug` (`main.zig:19-21`), and `drainGaze` logs the first three samples and then every 500th, so a working device produces visible output within a second.

- [ ] **Step 4: If it FAILED, stop here**

Record the exact log output and report. Do not continue to Task 4. The most likely causes in order: udev not applied or device not replugged (Task 2); another process holding the device, since libusb claims interface 0 exclusively; genuine Windows provisioning requirement, in which case the project needs a Windows machine once before it can proceed.

- [ ] **Step 5: If it PASSED, record the baseline**

```bash
./scripts/spike-first-sample.sh > docs/spike-baseline.txt 2>&1
git add scripts/spike-first-sample.sh docs/spike-baseline.txt
git commit -m "feat: bring-up spike, gaze confirmed streaming on Linux"
```

---

## Task 4: P7, enlarge the calibration buffers

**Files:**
- Create: `patches/0000-calibration-buffers.patch`
- Modify (via patch): `vendor/tobiifree/driver/src/tobiifree_core.zig:1051`, `vendor/tobiifree/applications/tobiifreed/src/server.zig:18`, `vendor/tobiifree/applications/tobiifreed/src/main.zig` (`sendResult`)

**Interfaces:**
- Consumes: build from Task 1
- Produces: a daemon that can carry a multi-kilobyte calibration blob without corrupting memory

This is first among the patches because every later calibration task depends on it. `session_out` is `[512]u8` (`tobiifree_core.zig:1051`) while `build_cal_apply` (`:566`) writes envelope plus header plus 2 plus `blob_len` through a raw pointer, so any blob past roughly 478 bytes corrupts adjacent globals. `Client.buf` is `[4096]u8` (`server.zig:18`) and wipes itself when full.

- [ ] **Step 1: Write a failing size assertion**

Add to `vendor/tobiifree/driver/src/tobiifree_core.zig` inside the existing test block:

```zig
test "session_out can hold a maximum calibration blob" {
    const MAX_BLOB: usize = 4096;
    const TTP_OVERHEAD: usize = 34; // envelope + header + 2-byte prefix
    try std.testing.expect(session_out.len >= MAX_BLOB + TTP_OVERHEAD);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd vendor/tobiifree/driver && zig build test`
Expected: FAIL, `expected true, found false`, because 512 is less than 4130.

- [ ] **Step 3: Apply the minimal fix**

```zig
// tobiifree_core.zig:1051
var session_out: [8192]u8 = undefined;
```

```zig
// server.zig:18, inside Client
buf: [65536]u8,
```

In `server.zig`'s read path, replace the `space == 0` branch that sets `client.buf_len = 0` with a disconnect, because silently discarding a partial message is what makes an oversized command vanish without an error:

```zig
if (space == 0) {
    log.err("client message exceeds buffer, disconnecting", .{});
    self.removeClient(slot);
    return;
}
```

In `main.zig`'s `sendResult`, size the stack buffer to `proto.HEADER_SIZE + 1 + 4096` and add a guard bounding the payload against `core.scratch_size()`:

```zig
var buf: [proto.HEADER_SIZE + 1 + 4096]u8 = undefined;
const max_payload = core.scratch_size();
if (payload.len > max_payload) {
    log.err("response payload {d} exceeds maximum {d}", .{ payload.len, max_payload });
    sendResult(client_fd, cmd_type, is_ws, false, &.{});
    return;
}
std.debug.assert(max_payload + proto.HEADER_SIZE + 1 <= buf.len);
```

Do not mirror this buffer against `Client.buf`. Inbound is capped by whatever a client may send, hence 65536, while outbound is capped by `out_scratch` at 4096, so the two are bounded by different things and must not be sized alike.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd vendor/tobiifree/driver && zig build test`
Expected: PASS

- [ ] **Step 5: Extract the patch and rebuild from clean**

```bash
git -C vendor/tobiifree diff > patches/0000-calibration-buffers.patch
./scripts/build.sh   # re-applies the whole stack from clean, then re-stages
```
Expected: `applying 0000-calibration-buffers.patch` then a successful build.

- [ ] **Step 6: Commit**

```bash
git add patches/0000-calibration-buffers.patch
git commit -m "fix: enlarge calibration buffers (P7), 512B session_out overflowed on any real blob"
```

---

## Task 5: P4, force the display area

**Files:**
- Create: `patches/0003-force-display-area.patch`
- Modify (via patch): `vendor/tobiifree/applications/tobiifreed/src/main.zig` argument parsing and startup

**Interfaces:**
- Consumes: Task 4's build
- Produces: `tobiifreed --force-display-area` writes the config to the device unconditionally

`isReset()` is `w < 50 or h < 50` (`tracker.zig:41-45`) and the daemon only writes config when that is true. The CLI accepts only `--init-config` and `--ws` (`main.zig:377,380`). So once the device holds any plausible geometry, including the shipped 800x340 default, a corrected config is silently ignored forever and every calibration afterwards is computed in the wrong frame.

- [ ] **Step 1: Add the flag to argument parsing**

In `main.zig`, alongside the existing `--init-config` and `--ws` branches:

```zig
var force_display = false;
// ... inside the arg loop:
} else if (std.mem.eql(u8, arg, "--force-display-area")) {
    force_display = true;
}
```

- [ ] **Step 2: Use it at startup**

Replace the `isReset()` branch:

```zig
if (force_display or tracker.display.isReset()) {
    if (force_display) log.info("forcing display area from config", .{});
    if (!tracker.setDisplayArea(display)) {
        log.err("failed to set display area from config", .{});
        std.process.exit(1);
    }
} else {
    log.info("device display area preserved from previous session", .{});
}
```

Note the failure is now fatal rather than a warning. A daemon that silently runs with the wrong geometry produces plausible-but-wrong gaze, which spec section 11 exists to prevent.

- [ ] **Step 3: Verify the flag takes effect**

Run: `./scripts/build.sh && ./vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed --force-display-area 2>&1 | head -20`
Expected: a line `forcing display area from config`, then `display_area 597x336mm ...` matching your `~/.config/tobii.json`, not `preserved from previous session`.

- [ ] **Step 4: Verify it persists across a restart without the flag**

Run the daemon again with no flag.
Expected: `device display area preserved from previous session`, and the logged dimensions still 597x336. This confirms spec section 3's claim that display area is stored on the device.

- [ ] **Step 5: Extract the patch and commit**

```bash
git -C vendor/tobiifree diff > patches/0003-force-display-area.patch
./scripts/build.sh   # re-applies the whole stack from clean, then re-stages
git add patches/0003-force-display-area.patch
git commit -m "fix: add --force-display-area (P4), stale geometry was unfixable"
```

---

## Task 6: P1, single owner for the USB transport

**Files:**
- Create: `patches/0001-single-usb-owner.patch`
- Modify (via patch): `vendor/tobiifree/applications/tobiifreed/src/main.zig` (USB thread, command forwarding)

**Interfaces:**
- Consumes: Task 5's build
- Produces: calibration commands that cannot race gaze polling

`usbThreadFn` loops `tracker.poll()` forever while the main thread reaches `drainReads` then `recv` then `core.feed_usb_in` (`tracker.zig:288,294`). Two threads, one bulk endpoint, one set of global protocol state, no mutex. Pausing around only the three state machines is insufficient: `get_display_area`, both setters and `add_calibration_point` mutate the same globals (`main.zig:184-203`), and the pending routing entry is installed *after* the send (`:193-195`), so the USB thread can consume the response first.

- [ ] **Step 1: Add a pause-and-ack handshake**

```zig
// near the other globals in main.zig
var usb_pause_req: std.atomic.Value(bool) = std.atomic.Value(bool).init(false);
var usb_paused: std.atomic.Value(bool) = std.atomic.Value(bool).init(false);

fn usbThreadFn() void {
    log.info("USB thread started", .{});
    while (!quit) {
        if (usb_pause_req.load(.acquire)) {
            usb_paused.store(true, .release);
            while (usb_pause_req.load(.acquire) and !quit) std.Thread.sleep(200_000); // 0.2ms
            usb_paused.store(false, .release);
            continue;
        }
        tracker.poll();
    }
    log.info("USB thread stopped", .{});
}

fn usbPause() void {
    usb_pause_req.store(true, .release);
    var spins: u32 = 0;
    while (!usb_paused.load(.acquire) and spins < 5000) : (spins += 1) std.Thread.sleep(200_000);
    if (spins >= 5000) log.err("USB thread failed to pause", .{});
}

fn usbResume() void {
    usb_pause_req.store(false, .release);
}
```

- [ ] **Step 2: Wrap every forwarded command, not just the state machines**

In `forwardCommand`, before the switch on `cmd`:

```zig
    .subscribe, .disconnect => return, // handled locally, not forwarded
    else => {},
}
usbPause();
defer usbResume();
```

so that all of `get_display_area`, `set_display_area`, `set_display_area_corners`, `start_calibration`, `add_calibration_point`, `finish_calibration` and `cal_apply` execute with the USB thread parked.

- [ ] **Step 3: Register the pending entry before sending**

In the request path at `main.zig:193-195`, move `addPending(...)` above the `transport.send(...)` call so the routing table is populated before any response can arrive.

- [ ] **Step 4: Verify with a command loop under load**

Run the daemon, then in another terminal issue 200 `get_display_area` commands back to back while gaze is streaming:

```bash
python3 - <<'EOF'
import socket, struct, os, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(os.path.expanduser("~/.local/share/tobiifree/gaze.sock"))  # confirm path from daemon log
s.sendall(bytes([0x01]) + struct.pack("<I", 0))          # subscribe
ok = 0
for i in range(200):
    s.sendall(bytes([0x02]) + struct.pack("<I", 0))      # get_display_area
    time.sleep(0.01)
print("sent 200 display-area queries with gaze streaming")
EOF
```
Expected: the daemon log shows no `unknown cmd`, no `pending table full`, and no state-machine timeouts. Before this patch the same loop produces intermittent failures.

- [ ] **Step 5: Extract the patch and commit**

```bash
git -C vendor/tobiifree diff > patches/0001-single-usb-owner.patch
./scripts/build.sh   # re-applies the whole stack from clean, then re-stages
git add patches/0001-single-usb-owner.patch
git commit -m "fix: pause USB thread around all forwarded commands (P1)"
```

---

## Task 7: P2+P3, one write path for every socket write

**Files:**
- Create: `patches/0002-write-path.patch`
- Modify (via patch): `vendor/tobiifree/applications/tobiifreed/src/server.zig`, `.../main.zig`

**Interfaces:**
- Consumes: Task 6's build
- Produces: a daemon that never disconnects on `EAGAIN` and never leaves a partial frame on the wire

These are one patch, not two. `server.zig:162` is `_ = std.posix.write(client.fd, &msg) catch { self.removeClient(slot); };` on a non-blocking fd, so it both disconnects on `EAGAIN` and discards the short-write count. Fixing them separately is unsafe: a short write followed by `EAGAIN` leaves a frame prefix on the wire, which is exactly the desync that looping is meant to prevent.

- [ ] **Step 1: Add a per-client output queue**

```zig
// in Client
out: [131072]u8,
out_len: usize,
out_off: usize,
```

- [ ] **Step 2: Replace every write site with one function**

```zig
/// Queue a complete framed message. Returns false if the queue is full.
fn enqueue(client: *Client, msg: []const u8) bool {
    if (client.out_len - client.out_off + msg.len > client.out.len) return false;
    if (client.out_off > 0 and client.out_off == client.out_len) {
        client.out_len = 0;
        client.out_off = 0;
    }
    @memcpy(client.out[client.out_len..][0..msg.len], msg);
    client.out_len += msg.len;
    return true;
}

/// Drain as much as the socket will take. Never partial-drops a frame.
fn flush(self: *Server, slot: *?Client) void {
    const client = &(slot.* orelse return);
    while (client.out_off < client.out_len) {
        const n = std.posix.write(client.fd, client.out[client.out_off..client.out_len]) catch |err| {
            if (err == error.WouldBlock) return;      // resume on POLLOUT
            self.removeClient(slot);
            return;
        };
        if (n == 0) { self.removeClient(slot); return; }
        client.out_off += n;
    }
    client.out_len = 0;
    client.out_off = 0;
}
```

- [ ] **Step 3: Change `broadcastGaze` to drop only whole frames**

```zig
for (&self.clients) |*slot| {
    const client = &(slot.* orelse continue);
    if (!client.subscribed) continue;
    if (client.out_off < client.out_len) {
        // a frame is mid-flight; drop this gaze sample rather than interleave
        self.dropped += 1;
        self.flush(slot);
        continue;
    }
    if (!enqueue(client, &msg)) self.dropped += 1;
    self.flush(slot);
}
```

Apply the same `enqueue` plus `flush` to the response writes at `main.zig:143,240,249`. Responses are never dropped: if `enqueue` fails for a response, disconnect the client instead.

- [ ] **Step 4: Test with a deliberately stalled reader**

```bash
python3 - <<'EOF'
import socket, struct, os, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(os.path.expanduser("~/.local/share/tobiifree/gaze.sock"))
s.sendall(bytes([0x01]) + struct.pack("<I", 0))
time.sleep(5)                     # stall: never read, fill the socket buffer
got = 0
s.setblocking(False)
try:
    while True:
        b = s.recv(65536)
        if not b: break
        got += len(b)
except BlockingIOError:
    pass
print("still connected, drained", got, "bytes after a 5 s stall")
# a full 397-byte frame boundary must be intact
EOF
```
Expected: the client is **still connected** after the stall and the byte count is a whole multiple of 397 (5-byte header plus 392-byte `GazeSample`). Before this patch the daemon disconnects during the stall.

- [ ] **Step 5: Extract the patch and commit**

```bash
git -C vendor/tobiifree diff > patches/0002-write-path.patch
./scripts/build.sh   # re-applies the whole stack from clean, then re-stages
git add patches/0002-write-path.patch
git commit -m "fix: single queued write path, no EAGAIN disconnect, no partial frames (P2+P3)"
```

---

## Task 8: P6 and P5, real poll loop and unplug recovery

**Files:**
- Create: `patches/0005-poll-loop.patch`, `patches/0004-unplug-recovery.patch`
- Modify (via patch): `vendor/tobiifree/applications/tobiifreed/src/main.zig`

**Interfaces:**
- Consumes: Task 7's build
- Produces: a daemon that sleeps instead of spinning, and survives a replug

- [ ] **Step 0: Declare the three symbols Task 9 will fill in**

Task 8's recovery path calls `device_present`, `broadcastStatus()` and `applySavedCalibration()`, which Task 9 implements. Add them as stubs now so this task compiles standalone, and Task 9 replaces the bodies:

```zig
var device_present: std.atomic.Value(bool) = std.atomic.Value(bool).init(true);
fn broadcastStatus() void {}              // filled in by Task 9 (P8)
fn applySavedCalibration() void {}        // filled in by Task 9 (P8/P9)
```

These are one task because P5's recovery needs P6's loop to notice anything. `main.zig:469-482` has no `poll()` at all despite its comment, only `std.Thread.sleep(1_000_000)`, so the notify pipe is dead weight, every sample gains up to 1 ms, and the CPU wakes 1000 times a second on a machine being tuned for latency. Separately, `LIBUSB_ERROR_NO_DEVICE` returns immediately rather than consuming the 100 ms timeout, so on unplug `while (!quit) tracker.poll()` spins a core at `.log_level = .debug`.

- [ ] **Step 1: Replace the sleep loop with poll**

```zig
while (!quit) {
    var fds = [_]std.posix.pollfd{
        .{ .fd = notify_pipe[0], .events = std.posix.POLL.IN, .revents = 0 },
        .{ .fd = server.listen_fd, .events = std.posix.POLL.IN, .revents = 0 },
    };
    _ = std.posix.poll(&fds, 50) catch 0;   // 50 ms cap so shutdown stays responsive
    drainGaze();
    server.acceptClients();
    server.readCommands();
    server.flushAll();                       // POLLOUT-driven resume from Task 7
    if (ws) |*w| { w.acceptClients(); w.readClients(); }
}
```

- [ ] **Step 2: Verify CPU drops**

Run the daemon, then `top -b -n 3 -p $(pgrep tobiifreed) | grep tobiifreed`
Expected: well under 1% of a core. Before this patch it sits noticeably higher from the 1 kHz wakeups.

- [ ] **Step 3: Classify USB errors instead of collapsing them**

In `libusb_transport.zig`, return a tagged result rather than `null` for everything:

```zig
pub const RecvResult = union(enum) { data: usize, timeout, fatal };

fn recvTimeout(self: *LibusbTransport, buf: []u8, timeout_ms: c_uint) RecvResult {
    var transferred: c_int = 0;
    const r = c.libusb_bulk_transfer(self.usb_handle, EP_IN, buf.ptr, @intCast(buf.len), &transferred, timeout_ms);
    if (r == 0 and transferred > 0) return .{ .data = @intCast(transferred) };
    if (r == -7) return .timeout;                       // LIBUSB_ERROR_TIMEOUT
    if (r == -1 or r == -4 or r == -5) return .fatal;   // IO, NO_DEVICE, NOT_FOUND
    return .timeout;
}
```

- [ ] **Step 4: Add teardown, backoff and bootstrap replay**

```zig
fn usbThreadFn() void {
    var backoff_ms: u64 = 100;
    while (!quit) {
        if (usb_pause_req.load(.acquire)) { /* Task 6 pause block */ continue; }
        switch (tracker.pollResult()) {
            .ok => { backoff_ms = 100; },
            .timeout => {},
            .fatal => {
                log.err("USB fatal, reinitialising in {d}ms", .{backoff_ms});   // rate-limited: once per attempt
                device_present.store(false, .release);
                broadcastStatus();                                              // Task 9
                transport.deinit();
                std.Thread.sleep(backoff_ms * std.time.ns_per_ms);
                backoff_ms = @min(backoff_ms * 2, 2000);
                transport = LibusbTransport.init() catch continue;
                tracker = Tracker.init(.{ .send_fn = transportSend, .recv_fn = transportRecv }) catch continue;
                if (!tracker.setDisplayArea(display)) log.err("display area replay failed", .{});
                applySavedCalibration();                                        // Task 9
                device_present.store(true, .release);
                broadcastStatus();
            },
        }
    }
}
```

The bootstrap replay is mandatory. `ExecStartPost` does **not** re-run for an in-process reconnect, so without this the daemon comes back uncalibrated while reporting success.

- [ ] **Step 5: Test a physical replug**

Run the daemon, confirm gaze is streaming, physically unplug the tracker, wait 10 s watching `top`, then replug.
Expected: CPU stays under 1% during the gap, the log shows at most a handful of `USB fatal, reinitialising` lines rather than a flood, and gaze resumes within about 2 s of replug.

- [ ] **Step 6: Extract both patches and commit**

```bash
git -C vendor/tobiifree diff -- applications/tobiifreed/src/main.zig > patches/0005-poll-loop.patch
git -C vendor/tobiifree diff -- driver/src/libusb_transport.zig driver/src/tracker.zig > patches/0004-unplug-recovery.patch
./scripts/build.sh   # re-applies the whole stack from clean, then re-stages
git add patches/0004-unplug-recovery.patch patches/0005-poll-loop.patch
git commit -m "fix: poll-driven main loop and USB unplug recovery with bootstrap replay (P5, P6)"
```

---

## Task 9: P8 and P9, device status message and pending-entry lifetime

**Files:**
- Create: `patches/0007-device-status-message.patch`, `patches/0008-pending-entry-lifetime.patch`
- Modify (via patch): `vendor/tobiifree/driver/src/daemon_protocol.zig`, `.../main.zig`, `.../server.zig`

**Interfaces:**
- Consumes: Task 8's build
- Produces: `Srv.status = 0x04` carrying device presence and calibration state; responses that cannot reach the wrong client

Spec section 11 requires the socket to carry a device-present flag, but `daemon_protocol.zig:21-28` defines only `gaze`, `response`, `display_area`, `err`. Without it, the plugin cannot distinguish unplugged hardware from a healthy daemon with stale gaze, and spec section 11's governing principle fails. Separately, `main.zig:89-143` keys pending entries on a bare `client_fd` while `server.zig:168-174` closes the fd without purging them, so a reused fd number can receive another client's response.

- [ ] **Step 1: Add the status opcode and payload**

```zig
// daemon_protocol.zig, in Srv
status = 0x04,

/// [u8 0x04][u32 LE 3][u8 device_present][u8 calibration_applied][u8 protocol_version]
pub fn encodeStatus(buf: *[HEADER_SIZE + 3]u8, present: bool, cal: bool) void {
    encodeHeader(buf[0..HEADER_SIZE], @intFromEnum(Srv.status), 3);
    buf[HEADER_SIZE] = @intFromBool(present);
    buf[HEADER_SIZE + 1] = @intFromBool(cal);
    buf[HEADER_SIZE + 2] = 1; // protocol version
}
```

- [ ] **Step 2: Emit it on connect and on every transition**

Call `broadcastStatus()` from: the accept path immediately after the client is registered, the `.fatal` branch in Task 8's USB thread both before teardown and after recovery, and after `cal_apply` succeeds.

- [ ] **Step 3: Key pending entries on slot plus generation**

```zig
const Pending = struct { slot: u8, gen: u32, request_id: u32, cmd_type: u8 };
```

Increment a per-slot `gen` counter in `removeClient`, and in `onResponse` drop any pending entry whose `gen` no longer matches the live client in that slot. Route responses through `enqueue` from Task 7 rather than writing directly from the USB callback.

- [ ] **Step 4: Test that a reconnect does not receive a stale response**

```bash
python3 - <<'EOF'
import socket, struct, os, time
p = os.path.expanduser("~/.local/share/tobiifree/gaze.sock")
a = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); a.connect(p)
a.sendall(bytes([0x02]) + struct.pack("<I", 0))   # request, then vanish
a.close()
time.sleep(0.05)
b = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); b.connect(p)
b.sendall(bytes([0x01]) + struct.pack("<I", 0))
b.settimeout(2.0)
data = b.recv(65536)
# first message must be status (0x04) or gaze (0x01); never a response (0x02)
print("first opcode:", hex(data[0]))
assert data[0] in (0x01, 0x04), "received another client's response"
print("PASS")
EOF
```
Expected: `PASS`. Before this patch the second client can receive the first client's `display_area` response.

- [ ] **Step 5: Extract both patches and commit**

```bash
git -C vendor/tobiifree diff -- driver/src/daemon_protocol.zig applications/tobiifreed/src/main.zig > patches/0007-device-status-message.patch
git -C vendor/tobiifree diff -- applications/tobiifreed/src/server.zig > patches/0008-pending-entry-lifetime.patch
./scripts/build.sh   # re-applies the whole stack from clean, then re-stages
git add patches/0007-device-status-message.patch patches/0008-pending-entry-lifetime.patch
git commit -m "feat: device status message and pending-entry generation keys (P8, P9)"
```

---

## Task 10: gaze-cal protocol layer

**Files:**
- Create: `gaze-cal/src/proto.h`, `gaze-cal/src/proto.c`, `gaze-cal/tests/test_proto.c`, `gaze-cal/Makefile`

**Interfaces:**
- Consumes: the wire format from Global Constraints
- Produces: `gz_frame_parse()`, `gz_encode_cmd()`, `struct gz_gaze_sample`. **Plan 2's OBS plugin links this file directly**, so it must not depend on anything CLI-specific.

- [ ] **Step 1: Write the failing tests**

```c
/* gaze-cal/tests/test_proto.c */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../src/proto.h"

static void test_struct_size_matches_wire(void) {
    assert(sizeof(struct gz_gaze_sample) == 392);
}

static void test_field_offsets(void) {
    /* A total-size assert cannot catch reordered fields. Pin each one. */
    assert(offsetof(struct gz_gaze_sample, present_mask)        == 0);
    assert(offsetof(struct gz_gaze_sample, frame_counter)       == 4);
    assert(offsetof(struct gz_gaze_sample, validity_L)          == 8);
    assert(offsetof(struct gz_gaze_sample, validity_R)          == 12);
    assert(offsetof(struct gz_gaze_sample, timestamp_us)        == 16);
    assert(offsetof(struct gz_gaze_sample, gaze_point_2d_norm)  == 40);
    assert(offsetof(struct gz_gaze_sample, gaze_point_2d_L_norm)== 56);
    assert(offsetof(struct gz_gaze_sample, gaze_point_2d_R_norm)== 72);
}

static void test_encode_subscribe(void) {
    unsigned char buf[16];
    size_t n = gz_encode_cmd(buf, sizeof buf, GZ_CMD_SUBSCRIBE, NULL, 0);
    assert(n == 5);
    assert(buf[0] == 0x01);
    assert(buf[1] == 0 && buf[2] == 0 && buf[3] == 0 && buf[4] == 0);
}

static void test_response_body_starts_at_offset_six(void) {
    /* response (0x02) prepends a one-byte cmd_type. Parsing from 5 misaligns every f64. */
    unsigned char wire[5 + 1 + 8] = {0x02, 9,0,0,0, 0x02};
    double v = 1.5; memcpy(wire + 6, &v, 8);
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == (int)sizeof wire);
    assert(f.type == 0x02);
    assert(f.cmd_type == 0x02);
    assert(f.body_len == 8);
    double out; memcpy(&out, f.body, 8);
    assert(out == 1.5);
}

static void test_rejects_absurd_length(void) {
    /* After a desync the length field is not a length. Bound it or the reader hangs. */
    unsigned char wire[5] = {0x01, 0xFF,0xFF,0xFF,0xFF};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
}

static void test_rejects_wrong_length_for_type(void) {
    unsigned char wire[5 + 4] = {0x01, 4,0,0,0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == GZ_ERR_DESYNC);
}

static void test_incomplete_frame_returns_zero(void) {
    unsigned char wire[3] = {0x01, 0, 0};
    struct gz_frame f;
    assert(gz_frame_parse(wire, sizeof wire, &f) == 0);
}

int main(void) {
    test_struct_size_matches_wire();
    test_field_offsets();
    test_encode_subscribe();
    test_response_body_starts_at_offset_six();
    test_rejects_absurd_length();
    test_rejects_wrong_length_for_type();
    test_incomplete_frame_returns_zero();
    printf("all proto tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd gaze-cal && make test`
Expected: FAIL, `proto.h: No such file or directory`

- [ ] **Step 3: Write the implementation**

```c
/* gaze-cal/src/proto.h */
#ifndef GZ_PROTO_H
#define GZ_PROTO_H
#include <stddef.h>
#include <stdint.h>

#define GZ_HEADER_SIZE   5
#define GZ_MAX_PAYLOAD   65536
#define GZ_ERR_DESYNC    (-1)

enum { GZ_CMD_SUBSCRIBE = 0x01, GZ_CMD_GET_DISPLAY_AREA = 0x02,
       GZ_CMD_SET_DISPLAY_AREA = 0x03, GZ_CMD_SET_DISPLAY_AREA_CORNERS = 0x04,
       GZ_CMD_START_CAL = 0x20, GZ_CMD_ADD_CAL_POINT = 0x21,
       GZ_CMD_FINISH_CAL = 0x22, GZ_CMD_CAL_APPLY = 0x23, GZ_CMD_DISCONNECT = 0xFF };
enum { GZ_SRV_GAZE = 0x01, GZ_SRV_RESPONSE = 0x02, GZ_SRV_DISPLAY_AREA = 0x03,
       GZ_SRV_STATUS = 0x04, GZ_SRV_ERR = 0xFF };

/* validity == 0 means VALID. Do not invert. */
struct gz_gaze_sample {
    uint32_t present_mask, frame_counter, validity_L, validity_R;
    int64_t  timestamp_us;
    double   pupil_L_mm, pupil_R_mm;
    double   gaze_point_2d_norm[2], gaze_point_2d_L_norm[2], gaze_point_2d_R_norm[2];
    double   eye_origin_L_mm[3], eye_origin_R_mm[3];
    double   trackbox_eye_pos_L[3], trackbox_eye_pos_R[3];
    double   gaze_point_3d_L_mm[3], gaze_point_3d_R_mm[3];
    double   eye_origin_L_display_mm[3], eye_origin_R_display_mm[3];
    double   trackbox_eye_pos_L_display[3], trackbox_eye_pos_R_display[3];
    double   eye_origin_raw_L_mm[3], eye_origin_raw_R_mm[3];
    double   gaze_point_2d_unfiltered[2];
};

struct gz_frame {
    uint8_t type;
    uint8_t cmd_type;          /* valid only when type == GZ_SRV_RESPONSE */
    const unsigned char *body;
    size_t body_len;
};

/* Returns bytes consumed, 0 if incomplete, GZ_ERR_DESYNC if unparseable. */
int gz_frame_parse(const unsigned char *buf, size_t len, struct gz_frame *out);
size_t gz_encode_cmd(unsigned char *buf, size_t cap, uint8_t cmd,
                     const void *payload, size_t payload_len);
#endif
```

```c
/* gaze-cal/src/proto.c */
#include <string.h>
#include "proto.h"

_Static_assert(sizeof(struct gz_gaze_sample) == 392,
               "GazeSample must match the Zig extern struct exactly");

static int expected_len_ok(uint8_t type, uint32_t len) {
    switch (type) {
    case GZ_SRV_GAZE:         return len == sizeof(struct gz_gaze_sample);
    case GZ_SRV_DISPLAY_AREA: return len == 9 * sizeof(double);
    case GZ_SRV_STATUS:       return len == 3;
    case GZ_SRV_RESPONSE:     return len >= 1 && len <= GZ_MAX_PAYLOAD;
    case GZ_SRV_ERR:          return len == 4;
    default:                  return 0;
    }
}

int gz_frame_parse(const unsigned char *buf, size_t len, struct gz_frame *out) {
    if (len < GZ_HEADER_SIZE) return 0;
    uint8_t type = buf[0];
    uint32_t plen;
    memcpy(&plen, buf + 1, 4);              /* little endian, matches x86-64 */
    if (plen > GZ_MAX_PAYLOAD) return GZ_ERR_DESYNC;
    if (!expected_len_ok(type, plen)) return GZ_ERR_DESYNC;
    if (len < GZ_HEADER_SIZE + plen) return 0;

    out->type = type;
    if (type == GZ_SRV_RESPONSE) {
        out->cmd_type = buf[GZ_HEADER_SIZE];
        out->body     = buf + GZ_HEADER_SIZE + 1;
        out->body_len = plen - 1;
    } else {
        out->cmd_type = 0;
        out->body     = buf + GZ_HEADER_SIZE;
        out->body_len = plen;
    }
    return (int)(GZ_HEADER_SIZE + plen);
}

size_t gz_encode_cmd(unsigned char *buf, size_t cap, uint8_t cmd,
                     const void *payload, size_t payload_len) {
    if (cap < GZ_HEADER_SIZE + payload_len) return 0;
    buf[0] = cmd;
    uint32_t n = (uint32_t)payload_len;
    memcpy(buf + 1, &n, 4);
    if (payload_len) memcpy(buf + GZ_HEADER_SIZE, payload, payload_len);
    return GZ_HEADER_SIZE + payload_len;
}
```

```makefile
# gaze-cal/Makefile
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2 -g
SRC      = src/proto.c src/client.c src/display.c src/calibrate.c src/record.c src/main.c
BUILD    = build

$(BUILD)/gaze-cal: $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm
$(BUILD):
	mkdir -p $(BUILD)
test: | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/test_proto tests/test_proto.c src/proto.c && $(BUILD)/test_proto
clean:
	rm -rf $(BUILD)
.PHONY: test clean
```

Note `-Werror` with `offsetof` requires `#include <stddef.h>` in the test.

- [ ] **Step 4: Run to verify it passes**

Run: `cd gaze-cal && make test`
Expected: `all proto tests passed`

- [ ] **Step 5: Commit**

```bash
git add gaze-cal/src/proto.h gaze-cal/src/proto.c gaze-cal/tests/test_proto.c gaze-cal/Makefile
git commit -m "feat: gaze-cal wire protocol with offset asserts and desync bounds"
```

---

## Task 11: gaze-cal client with mandatory subscribe and watchdog

**Files:**
- Create: `gaze-cal/src/client.h`, `gaze-cal/src/client.c`, `gaze-cal/tests/test_client.c`

**Interfaces:**
- Consumes: `gz_frame_parse`, `gz_encode_cmd` from Task 10
- Produces: `gz_client_connect()`, `gz_client_poll()`, `gz_client_send()`, `struct gz_client`. Plan 2's plugin reuses this.

- [ ] **Step 1: Write the failing tests**

```c
/* gaze-cal/tests/test_client.c */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../src/client.h"

/* Feed bytes through the client's incremental reader without a real socket. */
static void test_subscribe_is_first_bytes_out(void) {
    struct gz_client c; gz_client_init(&c);
    unsigned char out[16];
    size_t n = gz_client_take_outbound(&c, out, sizeof out);
    assert(n == 5 && out[0] == GZ_CMD_SUBSCRIBE);
}

static void test_partial_frame_is_buffered(void) {
    struct gz_client c; gz_client_init(&c);
    unsigned char half[100] = {GZ_SRV_GAZE, 0x88, 0x01, 0, 0};   /* 392 payload */
    assert(gz_client_feed(&c, half, sizeof half) == 0);          /* 0 frames yet */
}

static void test_desync_requests_reconnect(void) {
    struct gz_client c; gz_client_init(&c);
    unsigned char bad[5] = {0x77, 0xFF, 0xFF, 0xFF, 0xFF};
    assert(gz_client_feed(&c, bad, sizeof bad) == GZ_CLIENT_RECONNECT);
}

static void test_watchdog_fires_without_gaze(void) {
    struct gz_client c; gz_client_init(&c);
    c.last_gaze_ns = 0;
    assert(gz_client_watchdog(&c, 1500000000ULL) == GZ_CLIENT_RECONNECT);
    assert(gz_client_watchdog(&c, 500000000ULL) == 0);
}

int main(void) {
    test_subscribe_is_first_bytes_out();
    test_partial_frame_is_buffered();
    test_desync_requests_reconnect();
    test_watchdog_fires_without_gaze();
    printf("all client tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd gaze-cal && $CC -std=c11 -o build/test_client tests/test_client.c src/client.c src/proto.c`
Expected: FAIL, `client.h: No such file or directory`

- [ ] **Step 3: Write the implementation**

```c
/* gaze-cal/src/client.h */
#ifndef GZ_CLIENT_H
#define GZ_CLIENT_H
#include <stdint.h>
#include "proto.h"

#define GZ_CLIENT_RECONNECT (-2)
#define GZ_WATCHDOG_NS      1000000000ULL   /* 1 s with no valid gaze frame */

struct gz_client {
    int fd;
    unsigned char in[262144];
    size_t in_len;
    unsigned char out[4096];
    size_t out_len, out_off;
    uint64_t last_gaze_ns;
    struct gz_gaze_sample latest;
    int have_latest;
    uint8_t device_present, calibration_applied;
};

void   gz_client_init(struct gz_client *c);          /* queues subscribe */
int    gz_client_connect(struct gz_client *c, const char *path);
size_t gz_client_take_outbound(struct gz_client *c, unsigned char *buf, size_t cap);
int    gz_client_feed(struct gz_client *c, const unsigned char *b, size_t n); /* frames, or GZ_CLIENT_RECONNECT */
int    gz_client_watchdog(const struct gz_client *c, uint64_t now_minus_last_ns);
int    gz_client_send(struct gz_client *c, uint8_t cmd, const void *p, size_t n);
void   gz_client_close(struct gz_client *c);
#endif
```

```c
/* gaze-cal/src/client.c  (excerpt: init, feed, watchdog) */
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "client.h"

void gz_client_init(struct gz_client *c) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    /* Mandatory. A client that does not subscribe receives zero samples, silently. */
    c->out_len = gz_encode_cmd(c->out, sizeof c->out, GZ_CMD_SUBSCRIBE, NULL, 0);
}

int gz_client_connect(struct gz_client *c, const char *path) {
    gz_client_init(c);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    c->fd = fd;
    return 0;
}

size_t gz_client_take_outbound(struct gz_client *c, unsigned char *buf, size_t cap) {
    size_t n = c->out_len - c->out_off;
    if (n > cap) n = cap;
    memcpy(buf, c->out + c->out_off, n);
    c->out_off += n;
    if (c->out_off == c->out_len) { c->out_off = 0; c->out_len = 0; }
    return n;
}

int gz_client_feed(struct gz_client *c, const unsigned char *b, size_t n) {
    if (c->in_len + n > sizeof c->in) return GZ_CLIENT_RECONNECT;
    memcpy(c->in + c->in_len, b, n);
    c->in_len += n;

    int frames = 0;
    size_t off = 0;
    for (;;) {
        struct gz_frame f;
        int r = gz_frame_parse(c->in + off, c->in_len - off, &f);
        if (r == 0) break;
        if (r == GZ_ERR_DESYNC) return GZ_CLIENT_RECONNECT;
        switch (f.type) {
        case GZ_SRV_GAZE:
            memcpy(&c->latest, f.body, sizeof c->latest);
            c->have_latest = 1;
            frames++;
            break;
        case GZ_SRV_STATUS:
            c->device_present       = f.body[0];
            c->calibration_applied  = f.body[1];
            break;
        default:
            break;   /* skip unknown/irrelevant types by length; both clients share the daemon */
        }
        off += (size_t)r;
    }
    memmove(c->in, c->in + off, c->in_len - off);
    c->in_len -= off;
    return frames;
}

int gz_client_watchdog(const struct gz_client *c, uint64_t since_last_ns) {
    (void)c;
    return since_last_ns > GZ_WATCHDOG_NS ? GZ_CLIENT_RECONNECT : 0;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd gaze-cal && make test && $CC -std=c11 -Wall -Werror -o build/test_client tests/test_client.c src/client.c src/proto.c && ./build/test_client`
Expected: `all client tests passed`

- [ ] **Step 5: Commit**

```bash
git add gaze-cal/src/client.h gaze-cal/src/client.c gaze-cal/tests/test_client.c
git commit -m "feat: gaze-cal client with mandatory subscribe, desync detection and watchdog"
```

---

## Task 12: Display area force and readback gate

**Files:**
- Create: `gaze-cal/src/display.c`
- Modify: `gaze-cal/src/main.c` (add the `display` subcommand)

**Interfaces:**
- Consumes: `gz_client_*` from Task 11
- Produces: `gz_display_verify()`, which blocks the pipeline on a mismatch

Spec section 3 step 3. `get_display_area` returns **nine f64 corner coordinates** (`tl`, `tr`, `bl` in tracker-space mm), not the `w_mm/h_mm/z_mm/tilt` the config accepts, so the assertion needs a conversion.

- [ ] **Step 1: Write the failing test**

```c
/* append to gaze-cal/tests/test_proto.c */
static void test_corners_to_rect(void) {
    double corners[9] = {
        -298.5, 336.0, 0.0,    /* tl x,y,z */
         298.5, 336.0, 0.0,    /* tr */
        -298.5,   0.0, 0.0,    /* bl */
    };
    struct gz_rect r = gz_corners_to_rect(corners);
    assert(fabs(r.w_mm - 597.0) < 0.5);
    assert(fabs(r.h_mm - 336.0) < 0.5);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd gaze-cal && make test`
Expected: FAIL, `gz_corners_to_rect` undeclared

- [ ] **Step 3: Implement the conversion and the gate**

`struct gz_rect` and `gz_corners_to_rect` go in **`proto.h`/`proto.c`**, not `display.c`. The test target compiles only `tests/test_proto.c` plus `src/proto.c`, so defining them in `display.c` would fail to link, and Plan 2's plugin needs the conversion too.

```c
/* add to gaze-cal/src/proto.h */
struct gz_rect { double w_mm, h_mm, z_mm, tilt_deg; };
struct gz_rect gz_corners_to_rect(const double c[9]);
```

```c
/* add to gaze-cal/src/proto.c; add #include <math.h> at the top */
struct gz_rect gz_corners_to_rect(const double c[9]) {
    struct gz_rect r;
    r.w_mm = fabs(c[3] - c[0]);
    r.h_mm = fabs(c[1] - c[7]);
    r.z_mm = c[2];
    r.tilt_deg = atan2(c[8] - c[2], r.h_mm) * 180.0 / M_PI;
    return r;
}

/* Blocking gate: returns 0 only if the device matches the expected geometry. */
int gz_display_verify(const double got[9], struct gz_rect want, double tol_mm) {
    struct gz_rect r = gz_corners_to_rect(got);
    int ok = fabs(r.w_mm - want.w_mm) <= tol_mm && fabs(r.h_mm - want.h_mm) <= tol_mm;
    fprintf(stderr, "display area: device %.1fx%.1fmm z=%.1f tilt=%.2f | expected %.1fx%.1f | %s\n",
            r.w_mm, r.h_mm, r.z_mm, r.tilt_deg, want.w_mm, want.h_mm, ok ? "OK" : "MISMATCH");
    if (!ok) fprintf(stderr,
        "REFUSING TO CALIBRATE. Run: tobiifreed --force-display-area, then retry.\n"
        "Calibration is computed in the frame the display area defines; a wrong\n"
        "frame produces plausible-but-wrong gaze that no later step can correct.\n");
    return ok ? 0 : -1;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd gaze-cal && make test`
Expected: `all proto tests passed`

- [ ] **Step 5: Verify against real hardware**

Run: `./gaze-cal/build/gaze-cal display`
Expected: `display area: device 597.0x336.0mm ... | OK`. If it reports MISMATCH, run `tobiifreed --force-display-area` and retry, which is exactly the loop Task 5 exists to make possible.

- [ ] **Step 6: Commit**

```bash
git add gaze-cal/src/display.c gaze-cal/tests/test_proto.c
git commit -m "feat: display area readback gate with corner-to-rect conversion"
```

---

## Task 13: Calibration sequence, blob persistence, and the persistence experiment

**Files:**
- Create: `gaze-cal/src/calibrate.c`
- Modify: `gaze-cal/src/main.c` (add `calibrate` and `--apply-saved`)

**Interfaces:**
- Consumes: `gz_client_*`, `gz_display_verify`
- Produces: `~/.local/share/tobii-gaze/calibration.bin`; **an answer to spec section 13 item 2**

**PRECONDITION, and it cannot be checked by code.** Calibration is computed in the
display-area frame, so it is only as correct as that frame. Task 5 wrote
`~/.config/tobii.json` with a **measured** `w_mm` 597 and `h_mm` 336, but `z_mm`, `tilt`,
`cx` and `cy` are still the daemon's shipped template defaults, not measurements. Measure
them physically before calibrating: `z_mm` is the tracker plane to screen plane distance,
`tilt` is screen tilt in degrees, and `cy` is how far below the screen edge the tracker
sits. Then apply them with `tobiifreed --force-display-area` and confirm the readback.
`gz_display_verify` will NOT catch this, because it compares the device against the
config's own values and therefore passes cleanly with a wrong `z_mm`. Calibrating first
produces a plausible-but-wrong frame, which is the exact failure Task 5 exists to escape.

- [ ] **Step 1: Implement the calibration sequence**

Nine stimulus points in a 3x3 grid at normalised 0.1/0.5/0.9. For each: display it, wait 1200 ms for the eye to settle, then send `add_calibration_point` (0x21) with two f64 in normalised screen coordinates.

```c
static const double PTS[9][2] = {
    {0.1,0.1},{0.5,0.1},{0.9,0.1},
    {0.1,0.5},{0.5,0.5},{0.9,0.5},
    {0.1,0.9},{0.5,0.9},{0.9,0.9},
};

int gz_calibrate(struct gz_client *c) {
    if (gz_client_send(c, GZ_CMD_START_CAL, NULL, 0) < 0) return -1;
    for (int i = 0; i < 9; i++) {
        gz_show_stimulus(PTS[i][0], PTS[i][1]);   /* full-screen X11 dot */
        gz_sleep_ms(1200);
        double p[2] = { PTS[i][0], PTS[i][1] };
        if (gz_client_send(c, GZ_CMD_ADD_CAL_POINT, p, sizeof p) < 0) return -1;
    }
    /* finish_calibration returns the blob in its response body */
    return gz_client_send(c, GZ_CMD_FINISH_CAL, NULL, 0);
}
```

`gz_show_stimulus` and `gz_sleep_ms` are defined here, since nothing else provides them:

```c
/* gaze-cal/src/calibrate.c, above gz_calibrate */
#include <time.h>
#include <stdio.h>

void gz_sleep_ms(unsigned ms) {
    struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

/* Draws a single filled dot at normalised (nx, ny) on a fullscreen override-redirect
   X11 window. Reuses one window across all nine points so nothing flickers. */
void gz_show_stimulus(double nx, double ny);   /* implemented in stimulus.c */
```

Create `gaze-cal/src/stimulus.c` with a plain Xlib window: `XOpenDisplay`, a window sized to DP-1-2 at offset +4000+0 with `override_redirect = True`, `XFillArc` for a 20 px dot, `XFlush`. No toolkit, no compositing, because the tracker only needs a point to look at.

- [ ] **Step 2: Save the returned blob**

```c
/* gaze-cal/src/calibrate.c */
#include <stdint.h>
#include <sys/stat.h>

static uint32_t crc32c(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static const char *blob_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(buf, cap, "%s/.local/share/tobii-gaze", home);
    mkdir(buf, 0755);
    snprintf(buf, cap, "%s/.local/share/tobii-gaze/calibration.bin", home);
    return buf;
}

int gz_blob_save(const unsigned char *blob, size_t n) {
    char path[512]; blob_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t len = (uint32_t)n, crc = crc32c(blob, n);
    int ok = fwrite(&len, 4, 1, f) == 1 && fwrite(&crc, 4, 1, f) == 1
          && fwrite(blob, 1, n, f) == n;
    fclose(f);
    fprintf(stderr, "saved %zu byte calibration blob (crc %08x) to %s\n", n, crc, path);
    return ok ? 0 : -1;
}

/* Returns blob length, or -1. Refuses a blob whose CRC does not match, because a
   corrupt blob applied to the device is worse than no calibration: it is wrong
   rather than absent, and nothing downstream can detect it. */
int gz_blob_load(unsigned char *out, size_t cap) {
    char path[512]; blob_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t len = 0, crc = 0;
    if (fread(&len, 4, 1, f) != 1 || fread(&crc, 4, 1, f) != 1 || len > cap) { fclose(f); return -1; }
    if (fread(out, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    if (crc32c(out, len) != crc) { fprintf(stderr, "calibration blob CRC mismatch, refusing\n"); return -1; }
    return (int)len;
}
```

- [ ] **Step 3: Implement `--apply-saved`**

```c
int gz_apply_saved(struct gz_client *c) {
    unsigned char blob[65536];
    int n = gz_blob_load(blob, sizeof blob);
    if (n < 0) { fprintf(stderr, "no valid saved calibration\n"); return 1; }
    if (gz_client_send(c, GZ_CMD_CAL_APPLY, blob, (size_t)n) < 0) return 1;
    /* Wait for the response so systemd sees a real result, not just a successful write. */
    if (gz_await_response(c, GZ_CMD_CAL_APPLY, 3000) != 0) {
        fprintf(stderr, "cal_apply rejected by daemon\n");
        return 1;
    }
    fprintf(stderr, "applied %d byte calibration\n", n);
    return 0;
}
```

`gz_await_response` polls `gz_client_feed` until a `GZ_SRV_RESPONSE` with the matching `cmd_type` arrives or the timeout expires; add it to `client.c` alongside the other client functions. Exiting non-zero matters: `ExecStartPost` failure is how the operator learns calibration did not apply.

- [ ] **Step 4: Run a calibration and check accuracy**

Run: `./gaze-cal/build/gaze-cal calibrate`
Then: `./gaze-cal/build/gaze-cal preview` and look at each of the nine points in turn.
Expected: the reported gaze lands within roughly 45 px (one degree) of each point. Larger error means the display area is wrong, not the calibration.

- [ ] **Step 5: THE PERSISTENCE EXPERIMENT, resolves spec section 13 item 2**

```bash
./gaze-cal/build/gaze-cal preview --sample 200 > /tmp/before.txt   # look at screen centre
# physically unplug the tracker, wait 5 s, replug, wait for the daemon to recover
./gaze-cal/build/gaze-cal preview --sample 200 > /tmp/after.txt    # look at screen centre again
python3 - <<'EOF'
import statistics as st
def mid(p):
    xs=[];ys=[]
    for l in open(p):
        try: x,y=map(float,l.split()[:2]); xs.append(x); ys.append(y)
        except ValueError: pass
    return st.median(xs), st.median(ys)
b, a = mid("/tmp/before.txt"), mid("/tmp/after.txt")
dx, dy = (a[0]-b[0])*2560, (a[1]-b[1])*1440
print("shift after replug: %.0f px, %.0f px" % (dx, dy))
print("CALIBRATION PERSISTED" if abs(dx)<45 and abs(dy)<45 else "CALIBRATION LOST -- --apply-saved is mandatory")
EOF
```

Record the answer in the spec's section 13. If calibration persisted, `--apply-saved` is harmless insurance. If it did not, it is load-bearing and Task 8's bootstrap replay must be verified too.

- [ ] **Step 6: Commit**

```bash
git add gaze-cal/src/calibrate.c gaze-cal/src/main.c
git commit -m "feat: calibration sequence, blob persistence, and replug persistence test"
```

---

## Task 14: systemd units

**Files:**
- Create: `systemd/tobiifreed.service`, `scripts/install-systemd.sh`
- Modify (via patch, append to `patches/0007-device-status-message.patch`): `sd_notify` on socket bind

**Interfaces:**
- Consumes: the daemon binary, `gaze-cal --apply-saved`
- Produces: a daemon that starts with the session and applies calibration before declaring readiness

`Type=simple` would run `ExecStartPost` immediately after `execve`, while the daemon is still blocked in `LibusbTransport.init` and `Tracker.init`, so `gaze-cal` would hit `ENOENT` on a socket that does not exist yet and fail the unit.

- [ ] **Step 1: Add the readiness notification to the daemon**

After `Server.init` binds the socket, in `main.zig`:

```zig
if (std.posix.getenv("NOTIFY_SOCKET")) |_| {
    // sd_notify("READY=1") over the datagram socket
    notifyReady();
}
```

- [ ] **Step 2: Write the unit**

```ini
# systemd/tobiifreed.service
[Unit]
Description=Tobii ET5 gaze daemon
After=graphical-session.target

[Service]
Type=notify
ExecStart=%h/Documents/tobii-eye-tracker/vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed
ExecStartPost=%h/Documents/tobii-eye-tracker/gaze-cal/build/gaze-cal --apply-saved
Restart=always
RestartSec=2
Nice=-5

[Install]
WantedBy=graphical-session.target
```

`Nice=-5` rather than `SCHED_FIFO`: the thread's duty cycle is well under 1%, so a modest priority bump removes tail risk without needing `CAP_SYS_NICE`. **Do not pin CPU affinity**, since osu's affinity is unknown and CachyOS's scheduler already favours threads that sleep and wake briefly.

- [ ] **Step 3: Install and verify readiness ordering**

```bash
mkdir -p ~/.config/systemd/user
cp systemd/tobiifreed.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now tobiifreed
systemctl --user status tobiifreed
```
Expected: `active (running)`, and the journal shows the `--apply-saved` line **after** `listening on ...`, not before.

- [ ] **Step 4: Verify recovery**

Run: `systemctl --user restart tobiifreed && sleep 3 && ./gaze-cal/build/gaze-cal preview --sample 50`
Expected: gaze resumes and is still calibrated.

- [ ] **Step 5: Commit**

```bash
git add systemd/ scripts/install-systemd.sh
git commit -m "feat: systemd user unit with Type=notify so calibration applies after socket bind"
```

---

## Task 15: Trace recorder

**Files:**
- Create: `gaze-cal/src/record.c`
- Modify: `gaze-cal/src/main.c` (add `record`)

**Interfaces:**
- Consumes: `gz_client_*`
- Produces: `traces/*.csv` with per-eye raw samples and both clocks

Spec section 8 requires the design to consume the **per-eye** fields rather than `gaze_point_2d_norm`, which is already filtered on-device. The recorder captures both so the device's own group delay can be measured by cross-correlation.

- [ ] **Step 1: Write the recorder**

Both clocks are required: spec section 8 clamps `dt` from device deltas with a host fallback on drift, and neither rule can be validated without both recorded.

```c
/* gaze-cal/src/record.c */
#include <stdio.h>
#include <time.h>
#include "client.h"

static uint64_t mono_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

int gz_record(struct gz_client *c, const char *path, unsigned seconds) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "host_ns,device_us,frame_counter,present_mask,validity_L,validity_R,"
               "lx,ly,rx,ry,combined_x,combined_y,unfiltered_x,unfiltered_y,"
               "pupil_L,pupil_R\n");

    uint64_t deadline = mono_ns() + (uint64_t)seconds * 1000000000ULL;
    unsigned char buf[65536];
    unsigned long rows = 0;
    while (mono_ns() < deadline) {
        ssize_t n = read(c->fd, buf, sizeof buf);
        uint64_t host = mono_ns();          /* stamp immediately, before parsing */
        if (n <= 0) { if (errno == EAGAIN) { gz_sleep_ms(1); continue; } break; }
        if (gz_client_feed(c, buf, (size_t)n) == GZ_CLIENT_RECONNECT) break;
        if (!c->have_latest) continue;
        const struct gz_gaze_sample *s = &c->latest;
        fprintf(f, "%llu,%lld,%u,%u,%u,%u,"
                   "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.3f,%.3f\n",
                (unsigned long long)host, (long long)s->timestamp_us,
                s->frame_counter, s->present_mask, s->validity_L, s->validity_R,
                s->gaze_point_2d_L_norm[0], s->gaze_point_2d_L_norm[1],
                s->gaze_point_2d_R_norm[0], s->gaze_point_2d_R_norm[1],
                s->gaze_point_2d_norm[0],   s->gaze_point_2d_norm[1],
                s->gaze_point_2d_unfiltered[0], s->gaze_point_2d_unfiltered[1],
                s->pupil_L_mm, s->pupil_R_mm);
        c->have_latest = 0;
        rows++;
    }
    fclose(f);
    fprintf(stderr, "recorded %lu samples to %s\n", rows, path);
    return 0;
}
```

Note it records `gaze_point_2d_L_norm` and `_R_norm` (which spec section 8 says to consume), **and** `gaze_point_2d_norm` (which is already filtered on-device). Logging both is what lets Task 16 measure the device's own group delay by cross-correlation, which spec section 10 needs and cannot otherwise obtain.

- [ ] **Step 2: Record a real osu session**

Run: `./gaze-cal/build/gaze-cal record traces/osu-$(date +%Y%m%d).csv` then play osu for at least five minutes, covering easy and hard maps.

- [ ] **Step 3: Verify the trace is usable**

```bash
python3 - <<'EOF'
import csv, statistics as st
rows = list(csv.DictReader(open("traces/osu-20260726.csv")))
print("samples:", len(rows))
d = [int(rows[i]["device_us"]) - int(rows[i-1]["device_us"]) for i in range(1, len(rows))]
d = [x for x in d if 0 < x < 100000]
print("median inter-sample dt: %.2f ms  -> %.1f Hz" % (st.median(d)/1000, 1e6/st.median(d)))
inv = sum(1 for r in rows if r["validity_L"] != "0" or r["validity_R"] != "0")
print("invalid samples: %.1f%%" % (100*inv/len(rows)))
EOF
```
Expected: **median dt near 30.3 ms, i.e. 33 Hz**, and an invalid fraction in the low single digits, most of it blinks.

**This expectation was corrected during bring-up and is now measured, not assumed.** The plan originally said 7.5 ms / 133 Hz, taken from Tobii's marketing via a web search. The device actually delivers 33.2 Hz: `frame_counter` advances by exactly 4 on every sample (331 of 331 observed), dt clusters tightly at 30.27 ms with p10 29.85 and p90 30.55. The sensor counts at 133 Hz internally and ships every fourth frame. tobiifree's own `sdk/src/gusb.ts:99` says "~33Hz", so the code was right and the document was wrong.

Setting the two `0x04` bytes in the 20-byte subscribe payload to `0x01` was tested as a possible decimation divisor and produced **zero frames**, so the rate is not trivially recoverable. Unlocking 133 Hz would require decoding that payload properly, of which only `stream_id` at bytes 9..10 is currently understood. Out of scope for Phase 1.

Also measured during bring-up, for Task 16 and Plan 2: **per-eye L-R offset is 67 x 40 px** (78 px combined), against spec section 8's estimate of "up to 45 px". The monocular fusion EMA is more load-bearing than the spec describes.

- [ ] **Step 4: Commit**

```bash
git add gaze-cal/src/record.c gaze-cal/src/main.c
git commit -m "feat: gaze trace recorder capturing per-eye fields and both clocks"
```

---

## Task 16: Refit the filter constants against the real trace

**Files:**
- Create: `tools/fit_filter.py`

**Interfaces:**
- Consumes: `traces/*.csv` from Task 15
- Produces: fitted `minCutoff`, `beta`, `dCutoff` **in degrees**, plus measured noise, saccade and blink statistics. **These feed Plan 2 directly.**

Spec section 8 declares `beta` unset because 1.5 belongs to normalised units and the design changed to degrees. Spec section 13 ranks every constant by how badly a wrong simulation damages it.

- [ ] **Step 1: Measure the real signal characteristics**

```python
# tools/fit_filter.py, part 1
# Convert to degrees: x_deg = x_norm * 2560 / 45, y_deg = y_norm * 1440 / 45
# Report, from valid samples only:
#   - fixation tremor: stdev of position during runs where speed < 5 deg/s
#   - saccade peak velocity and duration distribution
#   - blink duration distribution and inter-blink interval
#   - per-eye L-R offset: mean, stdev, and drift rate (validates the 2 s EMA tau)
```

- [ ] **Step 2: Grid-search the one euro constants**

```python
# tools/fit_filter.py, part 2
# Objective, on held-out trace segments:
#   minimise  w1 * (steady-state stdev during fixations)
#           + w2 * (settling time to within 0.5 deg after a saccade)
# Sweep minCutoff in [0.2, 3.0], beta in [0.001, 1.0] (DEGREE units), dCutoff in [1, 8].
# Report the Pareto front, not a single point: smoothing and lag trade directly.
```

- [ ] **Step 3: Run the fit**

Run: `python3 tools/fit_filter.py traces/osu-20260726.csv`
Expected output: measured tremor in degrees, a saccade velocity distribution, and a Pareto front of `(minCutoff, beta, dCutoff)` with settling time and steady-state error for each.

- [ ] **Step 4: Update the spec with real numbers**

Replace spec section 8's provisional constants with the chosen fit, and replace section 13 item 4 with the measured characteristics. Note explicitly which simulation assumptions were wrong, since that is the most transferable output of this whole phase.

- [ ] **Step 5: Commit**

```bash
git add tools/fit_filter.py docs/superpowers/specs/2026-07-26-tobii-obs-overlay-design.md
git commit -m "feat: refit one-euro constants against a real gaze trace"
```

---

## Phase 1 exit criteria

All must hold before Plan 2 is written:

- [ ] `./scripts/spike-first-sample.sh` passes, so gaze streams on Linux
- [ ] All nine patches apply cleanly to the pinned vendor commit and build
- [ ] `cd gaze-cal && make test` passes
- [ ] `gaze-cal display` reports OK against measured monitor geometry
- [ ] Calibration lands within about one degree at all nine grid points
- [ ] The replug experiment has a recorded answer, either way
- [ ] Unplug during streaming keeps daemon CPU under 1% and recovers within about 2 s
- [ ] A five-minute osu trace exists with a confirmed sample rate
- [ ] `tools/fit_filter.py` has produced fitted constants in degrees

## What Plans 2 and 3 will cover

**Plan 2, OBS filter plugin.** Register as `OBS_SOURCE_TYPE_FILTER` with `obs_source_skip_video_filter`, one analytic fragment shader with eleven discrete `float4` uniforms because GL array uniforms are silently broken, the gaze-centred exclusion mask, the sRGB save/restore contract, the thread-ownership split from spec section 7, and the thin-versus-wide ring decision made against a real OBS render. Blocked on Task 16's constants and on the ring decision.

**Plan 3, verification.** Spec section 12: the photodiode input-to-photon test as the primary instrument, GPU timer queries as the primary number for the overlay arm, a positive control at ten times the pixel area, randomised block ordering, and fault-injection arms. Blocked on Plan 2.
