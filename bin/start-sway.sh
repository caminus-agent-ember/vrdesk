#!/bin/bash
# Start a headless sway compositor with two virtual outputs.
# Fully detached (setsid) so it survives the shell that launched it.
# Usage: start-sway.sh [socket-name]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/bin/sway-env.sh"

SOCK="${1:-vrdesk}"
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=2
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER_ALLOW_SOFTWARE=1
export SWAYSOCK="$XDG_RUNTIME_DIR/sway-ipc.$(id -u).$SOCK.sock"

mkdir -p "$HERE/run" "$HERE/logs"
if [ -S "$SWAYSOCK" ] && swaymsg -t get_version >/dev/null 2>&1; then
  echo "sway already running on $SWAYSOCK"
  exit 0
fi
rm -f "$SWAYSOCK"

setsid nohup sway -c "$HERE/sway.conf" -d > "$HERE/logs/sway.log" 2>&1 < /dev/null &
echo $! > "$HERE/run/sway.pid"

for i in $(seq 1 100); do
  if [ -S "$SWAYSOCK" ] && swaymsg -t get_version >/dev/null 2>&1; then
    echo "sway up: SWAYSOCK=$SWAYSOCK pid=$(cat "$HERE/run/sway.pid")"
    exit 0
  fi
  sleep 0.1
done
echo "sway failed to start; tail of log:" >&2
tail -20 "$HERE/logs/sway.log" >&2
exit 1
