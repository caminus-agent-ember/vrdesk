#!/bin/bash
# Start the vrdesk bridge, detached, pointed at the running sway session.
# Usage: start-bridge.sh [port]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/bin/sway-env.sh"
PORT="${1:-5000}"

export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
export SWAYSOCK="$XDG_RUNTIME_DIR/sway-ipc.$(id -u).vrdesk.sock"

mkdir -p "$HERE/run" "$HERE/logs"
if pgrep -f "bin/vrdesk-bridge --port $PORT" >/dev/null 2>&1; then
  echo "bridge already running on port $PORT"
  exit 0
fi

setsid nohup "$HERE/bin/vrdesk-bridge" --port "$PORT" -v \
  > "$HERE/logs/bridge.log" 2>&1 < /dev/null &
echo $! > "$HERE/run/bridge.pid"

# Wired transport: the headset is deliberately off Wi-Fi, so give it a USB path
# back to this port. adb reverse makes the device's 127.0.0.1:PORT reach us,
# which is exactly what the app's DEFAULT_HOST now points at.
if command -v adb >/dev/null 2>&1; then
  for ser in $(adb devices | awk 'NR>1 && $2=="device"{print $1}'); do
    if adb -s "$ser" reverse "tcp:$PORT" "tcp:$PORT" >/dev/null 2>&1; then
      echo "usb tunnel ready on $ser (tcp:$PORT)"
    else
      echo "warning: could not set up adb reverse on $ser" >&2
    fi
  done
fi

for i in $(seq 1 50); do
  if grep -q 'listening on' "$HERE/logs/bridge.log" 2>/dev/null; then
    echo "bridge up on port $PORT pid=$(cat "$HERE/run/bridge.pid")"
    exit 0
  fi
  sleep 0.1
done
echo "bridge failed to start:" >&2
cat "$HERE/logs/bridge.log" >&2
exit 1
