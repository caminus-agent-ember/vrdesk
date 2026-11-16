# vrdesk — virtual desktop for Ubuntu Wayland + Meta Quest 3

Your Linux windows, on virtual monitors floating in front of you in the headset,
pointed at with the Quest controllers.

```
 Quest 3 (OpenXR app)  <-- TCP :5000, raw BGRA frames --  bridge  --  headless sway
   controller ray/trigger  -- MSG_POINTER -->  zwlr_virtual_pointer_v1  -->  real pointer
```

## Pieces

| Path | What it is |
| --- | --- |
| `bin/start-sway.sh` | starts sway headless with 2 virtual outputs (HEADLESS-1 1920x1080, HEADLESS-2 1280x720) |
| `sway.conf` | sway config: floating windows, pixel borders, `mod+Return` → foot |
| `src/bridge.c` → `bin/vrdesk-bridge` | Wayland client of sway: `zwlr_screencopy_v1` frames + `zwlr_virtual_pointer_v1` input, TCP server |
| `tools/vrclient.py` | desktop test client: receives frames, saves PPM, sends clicks |
| `quest/jni/vrdesk_xr.c` | Quest OpenXR + GLES3 app: one textured quad per monitor, controller aim ray + trigger |
| `quest/build-quest.sh` | builds `quest/build/vrdesk.apk` (javac + d8 + aapt2 + apksigner, no gradle) |
| `openxr/lib/arm64-v8a/libopenxr_loader.so` | OpenXR loader built from Khronos OpenXR-SDK 1.1.65 with NDK 26.1 |
| `run_e2e.sh` | end-to-end proof: compositor → app → screenshot → bridge → client frame → injected click |

## Quick start (desktop side)

```bash
cd ~/vrdesk
./bin/start-sway.sh          # headless sway, 2 virtual outputs
./bin/start-bridge.sh 5000   # bridge listening on :5000
./run_e2e.sh                 # prove the whole desktop-side path works
```

`run_e2e.sh` prints `E2E RESULT: PASS` and 10 PASS lines when everything is healthy.

Put real apps on the virtual monitors with sway's own IPC:

```bash
source bin/sway-env.sh
swaymsg 'output HEADLESS-1 focus'
swaymsg exec firefox
swaymsg exec foot
```

## Quick start (headset side)

1. Build and install the APK:

   ```bash
   cd ~/vrdesk/quest
   ./build-quest.sh install          # uses adb serial 2G0YC5ZG7H07NS by default
   ```

2. Tell the app where the bridge is (optional; default is `192.168.1.10:5000`):

   ```bash
   echo -e "host=$(hostname -I | awk '{print $1}')\nport=5000" > /tmp/vrdesk.conf
   adb push /tmp/vrdesk.conf /sdcard/vrdesk.conf
   ```

3. **Wake the Touch controllers** — Horizon OS refuses to launch a controller-driven
   app while no controller is connected (`common_system_dialog_app_launch_blocked_controller_required`).

4. Launch:

   ```bash
   adb shell am start -n com.caminus.vrdesk/.MainActivity
   adb logcat -s vrdesk_xr:V          # watch it connect
   ```

   Expected log lines: `OpenXR runtime: ...`, `swapchain 0: ...`, `connected to bridge ...`.

5. Point a controller at a monitor and pull the trigger. The ray is intersected with
   each monitor quad; the hit point is sent as normalised coordinates, and the bridge
   converts it to a global compositor coordinate and injects a real pointer press.

## Wire format

Server → client, `MSG_FRAME = 1`:

```
u8 type | u16 output_id | u32 width | u32 height | u32 stride | u32 payload_len | BGRA bytes
```

Client → server:

```
MSG_POINTER = 2: u16 output_id | f32 x | f32 y (normalised 0..1) | u8 buttons | u8 action
MSG_SCROLL  = 4: u16 output_id | f32 dx | f32 dy
action: 0 = move, 1 = press, 2 = release
```

## Known limits

- **No keyboard injection.** The `virtual-keyboard-unstable-v1` protocol is gone from
  wayland-protocols 1.47 and sway's headless seat has `capabilities: 0`, so the bridge
  logs `MSG_KEY` and ignores it. Typing needs a different route (e.g. `wtype` against a
  seat that has a keyboard).
- **Pointer only.** Mouse buttons work (left button 272); right/middle are not mapped yet.
- The headless seat has no keyboard capability, so sway never moves keyboard focus to a
  window. Tests therefore assert the *active output*, not keyboard focus.
- `wl_output.geometry` reports 0,0 for headless outputs, so the bridge reads real output
  rects from sway's IPC (`GET_OUTPUTS`) instead.
- Frames are raw BGRA over TCP (no compression): ~3.7 MB/frame at 1280x720, ~8.3 MB at
  1920x1080. Fine on a LAN, not on Wi-Fi far from the router.
