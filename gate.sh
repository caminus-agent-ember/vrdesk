#!/bin/bash
# Done gate for vrdesk. Order matters: the e2e script kills the bridge on exit
# (trap cleanup + pkill), so the on-device checks MUST run before it.
set -u
cd "$(dirname "$0")" || exit 1
fail=0
ok()   { echo "PASS: $*"; }
bad()  { echo "FAIL: $*"; fail=1; }

echo "== 1. manifest"
M=$(~/android-sdk/build-tools/34.0.0/aapt2 dump xmltree quest/build/vrdesk.apk \
      --file AndroidManifest.xml 2>&1)
echo "$M" | grep -q 'android.app.NativeActivity'            && ok "NativeActivity"      || bad "no NativeActivity"
echo "$M" | grep -q 'android.app.lib_name'                  && ok "lib_name meta-data"  || bad "no lib_name"
echo "$M" | grep -q 'com.oculus.intent.category.VR'         && ok "VR category"         || bad "no VR category"

echo "== 2. bridge up"
if ! ss -ltn | grep -q ':5000'; then
  nohup bin/start-bridge.sh > logs/bridge-restart.out 2>&1 &
  for _ in $(seq 1 40); do ss -ltn | grep -q ':5000' && break; sleep 0.25; done
fi
ss -ltn | grep -q ':5000' && ok "bridge listening on :5000" || bad "bridge not listening"

echo "== 3. fresh launch"
adb shell am force-stop com.caminus.vrdesk
adb logcat -c
adb logcat -b crash -c
adb shell am start -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \
    -n com.caminus.vrdesk/android.app.NativeActivity >/dev/null 2>&1
sleep 12
# NOTE: `logcat -s vrdesk_xr:V` intermittently returns nothing on this device even
# when the lines are present; grep the unfiltered buffer instead.
L=$(adb logcat -d | grep vrdesk_xr)
R=$(echo "$L" | grep -c 'OpenXR runtime')
H=$(echo "$L" | grep -c 'XR_ERROR_HANDLE_INVALID')
N=$(echo "$L" | grep -c 'session is not running')
C=$(adb logcat -b crash -d | wc -c)
P=$(adb shell pidof com.caminus.vrdesk)
[ "$R" -ge 1 ] && ok "OpenXR runtime=$R"        || bad "OpenXR runtime=$R (want >=1)"
[ "$H" -eq 0 ] && ok "no HANDLE_INVALID"        || bad "$H HANDLE_INVALID lines"
[ "$N" -eq 0 ] && ok "no 'session is not running'" || bad "$N 'session is not running' lines"
[ "$C" -eq 0 ] && ok "crash buffer empty"       || bad "crash buffer has $C bytes"
[ -n "$P" ]    && ok "app pid=$P alive"         || bad "app not running"
echo "$L" | grep -q 'new monitor' && ok "monitors arrived from bridge" || bad "no monitors"

echo "== 4. desktop e2e (kills the bridge on exit - must be last)"
if timeout 400 ./run_e2e.sh > /tmp/vrdesk-e2e.log 2>&1; then
  grep -q 'E2E RESULT: PASS' /tmp/vrdesk-e2e.log && ok "e2e PASS" || bad "e2e did not print PASS"
else
  bad "e2e exited non-zero"; tail -15 /tmp/vrdesk-e2e.log
fi

echo "== restoring bridge for interactive use"
if ! ss -ltn | grep -q ':5000'; then
  nohup bin/start-bridge.sh > logs/bridge-restart.out 2>&1 &
  for _ in $(seq 1 40); do ss -ltn | grep -q ':5000' && break; sleep 0.25; done
fi
ss -ltn | grep -q ':5000' && ok "bridge back up" || bad "bridge not restored"

echo
[ "$fail" -eq 0 ] && echo "GATE RESULT: PASS" || echo "GATE RESULT: FAIL"
exit "$fail"
