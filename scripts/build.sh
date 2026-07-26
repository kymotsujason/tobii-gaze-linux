#!/usr/bin/env bash
# Apply the patch series to the pinned vendor checkout, then build tobiifreed.
# Never edits vendor/ in place: patches are reapplied from a clean checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
V="$ROOT/vendor/tobiifree"

if [ ! -e "$V/.git" ]; then
  echo "vendor/tobiifree is not initialised; run: git submodule update --init --recursive" >&2
  exit 1
fi

PIN="$(git -C "$ROOT" rev-parse ":vendor/tobiifree")"
if ! git -C "$V" cat-file -e "$PIN^{commit}" 2>/dev/null; then
  echo "pinned commit $PIN is missing from vendor/tobiifree; run: git submodule update --init --recursive" >&2
  exit 1
fi
if [ -f "$ROOT/vendor/PINNED_COMMIT" ] && [ "$(cat "$ROOT/vendor/PINNED_COMMIT")" != "$PIN" ]; then
  echo "note: vendor/PINNED_COMMIT is stale (says $(cat "$ROOT/vendor/PINNED_COMMIT"), gitlink says $PIN)" >&2
fi
if [ "$(git -C "$V" rev-parse HEAD)" != "$PIN" ]; then
  echo "vendor/tobiifree was at $(git -C "$V" rev-parse --short HEAD), resetting to pinned $PIN" >&2
fi
git -C "$V" checkout --force --detach "$PIN"
git -C "$V" clean -fd

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

# `nix develop` is the intended toolchain path, but an installed nix is not
# necessarily a working one: a glibc 2.44 upgrade made /usr/bin/nix and
# nix-daemon segfault before main (mimalloc interposes free, glibc frees a
# pointer mimalloc never allocated). So probe that nix runs, rather than
# trusting `command -v`. The probe costs ~0.5s and nix caches the evaluation.
NIX_USABLE=0
if command -v nix >/dev/null 2>&1; then
  # Run the probe under an intermediate shell whose stderr is redirected. The
  # "Segmentation fault (core dumped)" notice is printed by whichever shell waits
  # on the dying process, so without this it comes from *this* script and reads
  # like the build itself crashed.
  # The trailing `exit` matters: with a single command, bash execs it in place and
  # the segfault is then reported by *this* shell, unredirected.
  if bash -c 'nix develop "$1" --command true; exit $?' _ "$V" >/dev/null 2>&1; then
    NIX_USABLE=1
  else
    echo "warning: nix is installed but 'nix develop' fails; falling back to a direct zig build" >&2
  fi
fi

if [ "$NIX_USABLE" = 1 ]; then
  nix develop "$V" --command bash -c "cd '$V/applications/tobiifreed' && zig build -Doptimize=ReleaseSafe"
else
  ZIG_BIN="${ZIG:-}"
  if [ -z "$ZIG_BIN" ]; then
    for candidate in /nix/store/*-zig-0.15.*/bin/zig; do
      if [ -x "$candidate" ]; then ZIG_BIN="$candidate"; break; fi
    done
  fi
  if [ -z "$ZIG_BIN" ] && command -v zig >/dev/null 2>&1; then
    ZIG_BIN="$(command -v zig)"
  fi
  if [ -z "$ZIG_BIN" ]; then
    echo "no zig found; set ZIG=/path/to/zig (must be 0.15.x)" >&2
    exit 1
  fi
  "$ZIG_BIN" version | grep -q '^0\.15\.' || {
    echo "wrong zig version: $("$ZIG_BIN" version) at $ZIG_BIN (need 0.15.x)" >&2
    exit 1
  }
  echo "using zig: $ZIG_BIN" >&2

  # A zig from /nix/store stamps the store's glibc as the output binary's
  # PT_INTERP, and that loader does not search /usr/lib. libusb must therefore
  # come from the store too, or the build succeeds and the daemon then dies at
  # exec with "libusb-1.0.so.0: cannot open shared object file".
  case "$ZIG_BIN" in
    /nix/store/*)
      if [ -z "${PKG_CONFIG_PATH:-}" ]; then
        for pcdir in /nix/store/*libusb*-dev/lib/pkgconfig; do
          if [ -f "$pcdir/libusb-1.0.pc" ]; then export PKG_CONFIG_PATH="$pcdir"; break; fi
        done
        if [ -z "${PKG_CONFIG_PATH:-}" ]; then
          echo "store zig selected but no store libusb found; set PKG_CONFIG_PATH" >&2
          exit 1
        fi
        echo "using libusb from: $PKG_CONFIG_PATH" >&2
      fi
      ;;
  esac
  (cd "$V/applications/tobiifreed" && "$ZIG_BIN" build -Doptimize=ReleaseSafe)
fi
echo "built: $V/applications/tobiifreed/zig-out/bin/tobiifreed"
