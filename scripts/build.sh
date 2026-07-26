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

PIN="$(git -C "$ROOT" rev-parse :vendor/tobiifree)"
RECORDED="$(cat "$ROOT/vendor/PINNED_COMMIT")"
if [ "$PIN" != "$RECORDED" ]; then
  echo "gitlink $PIN != vendor/PINNED_COMMIT $RECORDED; these must match" >&2
  exit 1
fi

CUR="$(git -C "$V" rev-parse HEAD)"
if [ "$CUR" != "$PIN" ]; then
  echo "vendor/tobiifree HEAD $CUR != pinned $PIN; resetting to the pin" >&2
fi
git -C "$V" reset --hard "$PIN"
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

if command -v nix >/dev/null; then
  nix develop "$V" --command bash -c "cd '$V/applications/tobiifreed' && zig build -Doptimize=ReleaseSafe"
else
  echo "nix not found; falling back to system zig (must be 0.15.x)" >&2
  zig version | grep -q '^0\.15\.' || { echo "wrong zig version: $(zig version)" >&2; exit 1; }
  (cd "$V/applications/tobiifreed" && zig build -Doptimize=ReleaseSafe)
fi
echo "built: $V/applications/tobiifreed/zig-out/bin/tobiifreed"
