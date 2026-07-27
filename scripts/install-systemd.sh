#!/usr/bin/env bash
# Render systemd/tobiifreed.service.in into the user unit directory.
# Installs only. Enabling the unit at boot is left to the operator.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEMPLATE="$ROOT/systemd/tobiifreed.service.in"
BIN="$ROOT/vendor/tobiifree/applications/tobiifreed/zig-out/bin/tobiifreed"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
UNIT="$UNIT_DIR/tobiifreed.service"

usage() {
  cat >&2 <<EOF
usage: $(basename "$0") [--print | --uninstall]

  (no argument)  render the unit into $UNIT_DIR and reload the user manager
  --print        write the rendered unit to stdout and exit
  --uninstall    stop, disable and remove the installed unit
EOF
}

case "${1-}" in
  --print) MODE=print ;;
  --uninstall) MODE=uninstall ;;
  "") MODE=install ;;
  *) usage; exit 2 ;;
esac

if [ "$MODE" = uninstall ]; then
  # --now covers the running instance; a unit that was never enabled still
  # needs the explicit stop, hence both.
  systemctl --user disable --now tobiifreed.service 2>/dev/null || true
  systemctl --user stop tobiifreed.service 2>/dev/null || true
  rm -f "$UNIT"
  systemctl --user daemon-reload
  systemctl --user reset-failed tobiifreed.service 2>/dev/null || true
  echo "removed $UNIT"
  exit 0
fi

if [ ! -f "$TEMPLATE" ]; then
  echo "template not found: $TEMPLATE" >&2
  exit 1
fi

# ExecStart points into the build tree on purpose. scripts/build.sh resets the
# submodule with `git clean -fd`, which cannot touch zig-out because it is
# gitignored, so the path survives the patch workflow. A copy under
# ~/.local/bin would instead go silently stale after every rebuild.
if [ ! -x "$BIN" ]; then
  echo "daemon binary not found or not executable: $BIN" >&2
  echo "build it first: scripts/build.sh" >&2
  exit 1
fi

# The path is substituted rather than written as %h/Documents/tobii-eye-tracker
# so the unit follows the checkout instead of assuming where it lives.
render() { sed "s|@TOBIIFREED_BIN@|$BIN|g" "$TEMPLATE"; }

if [ "$MODE" = print ]; then
  render
  exit 0
fi

mkdir -p "$UNIT_DIR"
# Rendered to a temporary file and moved, so an interrupted install cannot
# leave a half-written unit that daemon-reload would then pick up.
TMP="$(mktemp "$UNIT_DIR/.tobiifreed.service.XXXXXX")"
trap 'rm -f "$TMP"' EXIT
render > "$TMP"
chmod 644 "$TMP"
mv "$TMP" "$UNIT"
trap - EXIT

systemctl --user daemon-reload
echo "installed $UNIT"
echo "  ExecStart=$BIN"
echo
echo "start it now:            systemctl --user start tobiifreed"
echo "start with the session:  systemctl --user enable tobiifreed"
echo "remove it again:         scripts/install-systemd.sh --uninstall"
