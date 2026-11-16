#!/bin/bash
# Self-contained probe: start bridge, connect a client, print capture stats.
set -u
cd "$HOME/vrdesk" || exit 1
. bin/sway-env.sh
export WAYLAND_DISPLAY=wayland-1
pkill -f 'bin/vrdesk-bridge' 2>/dev/null
sleep 0.3
./bin/vrdesk-bridge --port 5000 -v > logs/bridge.log 2>&1 &
BPID=$!
sleep 3
echo "=== stats idle (3s) ==="
grep stats logs/bridge.log | tail -2
echo "=== client: 2 frames ==="
timeout 6 python3 -u tools/vrclient.py --port 5000 --frames 2 --timeout 5
echo "client_exit=$?"
echo "=== stats after client ==="
grep stats logs/bridge.log | tail -2
kill "$BPID" 2>/dev/null
echo "=== full log ==="
cat logs/bridge.log
