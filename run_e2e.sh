#!/bin/bash
# run_e2e.sh — end-to-end proof for the vrdesk virtual-desktop stack.
#
# Proves, without a headset attached:
#   1. headless sway is up with >=2 virtual outputs
#   2. a real app window exists on a virtual monitor and grim can screenshot it (non-blank)
#   3. the bridge streams real frames over TCP to a client (non-blank pixels, both outputs)
#   4. a controller-style pointer event injected by the client moves sway's focus to the
#      monitor the event was addressed to (i.e. the click lands where the headset aimed it)
#
# Exit 0 only if every check passes. Usage: ./run_e2e.sh [-v]
set -u
# The tool shell does not source sway-env.sh, so put sway's tools on PATH and point at its socket.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HERE/root/usr/bin:$PATH"
export LD_LIBRARY_PATH="$HERE/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export SWAYSOCK="${SWAYSOCK:-$XDG_RUNTIME_DIR/sway-ipc.$(id -u).vrdesk.sock}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE" || exit 1
. bin/sway-env.sh
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
export SWAYSOCK="${SWAYSOCK:-$XDG_RUNTIME_DIR/sway-ipc.$(id -u).vrdesk.sock}"
PORT="${PORT:-5000}"
mkdir -p logs run tmp

FAIL=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=$((FAIL+1)); }
cleanup() {
  [ -n "${BPID:-}" ] && kill "$BPID" 2>/dev/null
  wait 2>/dev/null
}
trap cleanup EXIT

echo "== 1. compositor =="
if ! swaymsg -t get_version >/dev/null 2>&1; then
  echo "sway not running; starting it"
  ./bin/start-sway.sh || { fail "could not start sway"; exit 1; }
fi
NOUT=$(swaymsg -t get_outputs -r | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
if [ "$NOUT" -ge 2 ]; then pass "sway up with $NOUT virtual outputs"; else fail "expected >=2 outputs, got $NOUT"; fi

echo "== 2. a real app on a virtual monitor =="
# Remove any leftover probe windows so nothing overlaps the test window.
for w in vrdesk-test vrdesk-e2e; do
  swaymsg -t get_tree -r | grep -q "\"name\": \"$w\"" && swaymsg "[title=\"$w\"] kill" >/dev/null 2>&1
done
sleep 0.4
# Wait for the kill loop above to actually take effect: the old node can linger in get_tree for a
# few hundred ms after `kill`, and matching it made the poll below break on a dying window, so the
# later snapshot caught the gap between the old window dying and the new one mapping.
for _ in $(seq 1 40); do
  swaymsg -t get_tree -r | grep -q '"name": "vrdesk-e2e"' || break
  sleep 0.1
done
# Always launch a fresh window (the old one was just killed), then wait for it to be STABLE: the same
# pid present on two consecutive polls, so we never snapshot a node that is still going away.
swaymsg exec "xterm -title vrdesk-e2e -e bash -c 'echo VRDESK-E2E-OK; sleep 900'" >/dev/null
prev=""
for _ in $(seq 1 80); do
  cur=$(swaymsg -t get_tree -r | python3 -c '
import json, sys
t = json.load(sys.stdin)
pids = []
def walk(n):
    if n.get("name") == "vrdesk-e2e":
        pids.append(n.get("pid"))
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c)
walk(t)
print(pids[0] if len(pids) == 1 else "")
' 2>/dev/null)
  if [ -n "$cur" ] && [ "$cur" = "$prev" ]; then break; fi
  prev="$cur"
  sleep 0.25
done
# Pin the window to HEADLESS-2 so the test never depends on which monitor sway picked.
# (A floating window can be re-homed when the focused workspace changes, which made the
# frame/click assertions flaky.)
swaymsg '[title="vrdesk-e2e"] move to output HEADLESS-2' >/dev/null 2>&1
sleep 0.5
swaymsg -t get_tree -r > tmp/tree.json
read -r WIN_X WIN_Y WIN_W WIN_H WIN_OUT < <(python3 - <<'PY'
import json
t = json.load(open("tmp/tree.json"))
found = {}
def walk(n, out=None):
    if n.get("type") == "output":
        out = n.get("name")
    if n.get("name") == "vrdesk-e2e":
        r = n["rect"]
        found.update(x=r["x"], y=r["y"], w=r["width"], h=r["height"], out=out)
    for c in n.get("nodes", []) + n.get("floating_nodes", []):
        walk(c, out)
walk(t)
f = found
print(f.get("x", -1), f.get("y", -1), f.get("w", 0), f.get("h", 0), f.get("out", ""))
PY
)
if [ "${WIN_X:--1}" -ge 0 ]; then
  pass "window 'vrdesk-e2e' mapped at $WIN_X,$WIN_Y ${WIN_W}x${WIN_H} on $WIN_OUT"
else
  fail "window 'vrdesk-e2e' not found"
fi

# grim screenshot of that output must contain more than one colour
if [ -n "${WIN_OUT:-}" ]; then
  grim -o "$WIN_OUT" tmp/e2e-shot.png 2>/dev/null
  COLORS=$(python3 -c '
from PIL import Image
im = Image.open("tmp/e2e-shot.png").convert("RGB")
print(len(im.getcolors(maxcolors=1 << 20) or []))')
  if [ "${COLORS:-0}" -gt 1 ]; then pass "grim captured $WIN_OUT with $COLORS distinct colours (tmp/e2e-shot.png)"; else fail "grim screenshot of $WIN_OUT is blank"; fi
fi

echo "== 3+4. bridge: frames to client, injected click lands =="
# Kill any leftover bridge and WAIT for the port to be released, otherwise the new
# instance dies with "bind: Address already in use" and the whole run is bogus.
pkill -f 'bin/vrdesk-bridge' 2>/dev/null
for _ in $(seq 1 40); do
  pgrep -f 'bin/vrdesk-bridge' >/dev/null 2>&1 || break
  sleep 0.1
done
pkill -9 -f 'bin/vrdesk-bridge' 2>/dev/null
for _ in $(seq 1 40); do
  if ! (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
  exec 3>&- 2>/dev/null
  sleep 0.1
done
rm -f logs/bridge.log
./bin/vrdesk-bridge --port "$PORT" -v > logs/bridge.log 2>&1 &
BPID=$!
for _ in $(seq 1 60); do grep -q 'listening on' logs/bridge.log && break; sleep 0.25; done
if grep -q 'listening on' logs/bridge.log; then
  pass "bridge listening on :$PORT"
else
  fail "bridge did not start"; cat logs/bridge.log; exit 1
fi

# Which output index does the bridge give the monitor holding our window?
OID=$(python3 - "$WIN_OUT" <<'PY'
import re, sys
want = sys.argv[1]
oid = None
for line in open("logs/bridge.log"):
    m = re.search(r"output (\d+) name=(\S+)", line)
    if m and m.group(2) == want:
        oid = int(m.group(1))
print(-1 if oid is None else oid)
PY
)
if [ "$OID" -ge 0 ]; then pass "window's monitor $WIN_OUT is bridge output $OID"; else fail "could not map $WIN_OUT to a bridge output"; fi

# Normalised position of the window centre within its output.
read -r NX NY < <(python3 - "$WIN_X" "$WIN_Y" "$WIN_W" "$WIN_H" "$WIN_OUT" <<'PY'
import json, subprocess, sys
x, y, w, h, name = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
outs = json.loads(subprocess.run(["swaymsg", "-t", "get_outputs", "-r"], capture_output=True, text=True).stdout)
o = next(o for o in outs if o["name"] == name)["rect"]
print(round((x - o["x"] + w / 2) / o["width"], 4), round((y - o["y"] + h / 2) / o["height"], 4))
PY
)
echo "aiming at normalised $NX,$NY on output $OID"

# The headless seat has capabilities=0, so sway never gives a window keyboard focus;
# what a click CAN be proven to do is move the compositor's active output/workspace to
# the monitor the ray was aimed at. Assert exactly that.
# Start on a different monitor than the one we aim at, so a successful click must change it.
OTHER=$(swaymsg -t get_outputs -r | python3 -c '
import json,sys
outs=json.load(sys.stdin)
print(next((o["name"] for o in outs if o["name"] != "'"$WIN_OUT"'"), ""))')
if [ -n "$OTHER" ]; then swaymsg "focus output $OTHER" >/dev/null 2>&1; fi
sleep 0.4
active_output() {
  swaymsg -t get_outputs -r | python3 -c '
import json,sys
outs=json.load(sys.stdin)
print(next((o["name"] for o in outs if o.get("focused")), ""))'
}
BEFORE=$(active_output)
echo "active output before click: $BEFORE"

# Ask for more frames than we strictly need: the bridge interleaves both monitors and
# the frame captured right after the window maps can still be uniform, so a 2-frame
# sample made the "frame is blank" check flaky. The client keeps the LAST frame per
# output, so a longer window of frames converges on painted content.
timeout 30 python3 -u tools/vrclient.py --port "$PORT" --frames 8 --timeout 10 \
  --save tmp/client-frame --save-output "$OID" --click "$OID:$NX:$NY" > tmp/client.log 2>&1
CEXIT=$?
cat tmp/client.log
if [ $CEXIT -eq 0 ]; then pass "client received frames from the bridge"; else fail "client failed (exit $CEXIT)"; fi

# The frame the client decoded must be real content, not a blank buffer.
FRAME="tmp/client-frame.out$OID.ppm"
if [ -f "$FRAME" ]; then
  COLORS=$(python3 -c '
from PIL import Image
im = Image.open("'"$FRAME"'").convert("RGB")
print(len(im.getcolors(maxcolors=1 << 20) or []))')
  if [ "${COLORS:-0}" -gt 1 ]; then pass "streamed frame has $COLORS distinct colours ($FRAME)"; else fail "streamed frame is blank"; fi
else
  fail "client saved no frame"
fi

# The bridge writes the pointer line from its own event loop; give it a moment
# instead of sampling the log once (a single grep raced the flush and flaked).
for _ in $(seq 1 30); do grep -q 'pointer out=' logs/bridge.log && break; sleep 0.1; done
AFTER=$(active_output)
echo "active output after click:  $AFTER"
if grep -q 'pointer out=' logs/bridge.log; then pass "bridge injected the pointer event"; else fail "bridge logged no pointer event"; fi
if [ "$AFTER" = "$WIN_OUT" ] && [ "$AFTER" != "$BEFORE" ]; then
  pass "injected click landed on $WIN_OUT (the monitor the ray was aimed at)"
else
  fail "injected click did not activate the aimed monitor (before='$BEFORE' after='$AFTER' want='$WIN_OUT')"
fi

echo
if [ "$FAIL" -eq 0 ]; then echo "E2E RESULT: PASS"; exit 0; else echo "E2E RESULT: FAIL ($FAIL checks)"; exit 1; fi
