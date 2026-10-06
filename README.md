# EmuXR2

Runs the real Meta Quest 2 system software (Horizon OS v54: VrShell, ShellEnv, SystemUX and the rest)
on an Apple Silicon Mac, inside the Android emulator, from **your own** Quest 2 firmware image.
Nothing is recreated: the shell, home environment, menus and apps are Meta's own code booting unmodified.
EmuXR2 only supplies the hardware the emulator doesn't have.

EmuXR2 ships **no** Meta code or assets. You provide a Quest 2 OTA zip you legally own; everything taken
from it stays on your machine.

## How it works

The Quest's `system`, `system_ext` and `product` partitions are booted on top of the emulator's own
kernel and vendor layer (Android 12L, API 32, arm64 on Hypervisor.framework). On top of that:

| Piece | What it fixes |
| --- | --- |
| `patch.sh` | builds the bootable disk: debuggable props, permissive SELinux (the Quest platform policy and emulator vendor policy don't line up), flattened-APEX props, 64-bit AOSP media daemons (Apple Silicon can't run 32-bit ARM) |
| `egl/` | EGL/GLES shim over ANGLE: drops context attributes ANGLE rejects, ES 3.2 shaders to 3.1 |
| `vk/` | Vulkan HAL wrapper: `VK_KHR_external_memory_fd` over AHardwareBuffers, with shadow copies for layered and mipmapped images, so the VR runtime can share swapchains with apps |
| `hal/` | stand-ins for the Quest's vendor HALs (display controller, sensors, ...) |
| `build_super.py`, `lpdump.py` | rewrite the emulator's dynamic-partition `super` image |
| `vtable.py`, `hidlsig.py` | recover HIDL interface layouts from the interface libraries in your firmware |

## Use

```sh
./patch.sh ~/MacVRFirmware ~/Library/Android/sdk/system-images/android-32/google_apis/arm64-v8a/system.img
./boot.sh
```

`~/MacVRFirmware/img/` holds the partitions extracted from your OTA (`payload-dumper-go`).

## License

GPL-3.0-only. See `LICENSE`.

## Status (2026-10-05)

Boots the Quest's Android to `sys.boot_completed`. Meta's VR runtime, VrShell, ShellEnv, SystemUX, Horizon and Store
all run. Tracking accepts a stationary desktop pose through Meta's `TrackingDataInjection` service.
The Bubbles home, universal dock, coach cards and guest mouse cursor render in both eyes.
Boots show a live emulator window by default (`HEADLESS=1` hides it).

Settings now renders correctly in both eyes (`screenshots/34-settings-user-confirmed.png`).
The compositor's external-image programs retained optional vertex sampler2D uniforms on
unit0, overlapping the panel's external sampler and rejecting draws. The GLES shim now
initializes those optional samplers on distinct units when external programs are linked or
loaded from cache; runtime assignments still take precedence. Ordinary home shaders retain
their original defaults. `persist.macvr.samplerdefaults=0` disables this compatibility fix.

App Library's panel chrome also renders; content availability and loading still need testing.
Full OS usability, desktop input, networking, application lifecycle and sustained stability
remain in progress. See `PROGRESS.md` for evidence and remaining verification.

## Runtime controls

After the live window has finished starting, `./launch.sh settings`,
`./launch.sh library`, or `./launch.sh browser https://example.com` opens a panel.
Browser HTTPS page loading is verified. Connect the guest to the emulator's
`AndroidWifi` network in Settings; its saved connection survives cold boots.

`./input.sh` creates a guest Bluetooth mouse/keyboard and reads commands from stdin:
`m 20 0` moves, `c` clicks, `d`/`u` holds/releases the left button, `r` right-clicks,
`w 1` scrolls, and `k 108 1`/`k 108 0` presses/releases Linux KEY_DOWN.
Keep the process running while using it. Host mouse/keyboard forwarding is still
in progress.


## Quest streaming

Start the assembled guest with `./start-emulator.sh`, then `./start-streamer.sh`.
After shim/HAL or compatibility script changes, rebuild the image with `./boot.sh`.
The streamer uses `.venv/bin/python3` automatically. Set it up once with:

```sh
python3 -m venv .venv
.venv/bin/python3 -m pip install av numpy
```

The guest and Quest client must use matching bridge support. The client source is
in the parent VR4Mac project's `android/` folder. Its decoder must preserve the
captured XR pose timestamp separately from decoder presentation timestamps.

Left controller Menu drives guest Oculus/Home directly. The headset's own right
Oculus button remains available to its system. Tracking disconnects release guest
controller input. The desktop pose service is stopped while the streamer owns
head tracking, so it cannot overwrite headset poses.

Streaming negotiates up to 72 fps from the headset's reported refresh rate;
`EMUXR2_FPS=30 ./start-streamer.sh` caps it for slower machines. Quest 1 encodes
2432×1344 H.264. A 72 fps configuration is a target; source rendering, capture,
and decoder delivery rates must be measured separately. A static frozen display
retains its captured pose timestamp, even on keep-alive repeats.

When no guest account exists, the streamer selects Horizon's native local-account
mode so Library can finish loading offline. `EMUXR2_LOCAL_ACCOUNT=0` opts out;
`adb -s emulator-5554 shell oculuspreferences --setc local_account_mode_enabled_v1 false`
restores the normal account-backed mode (restart SystemUX afterward). This does
not provide online account features or app entitlements. `./launch.sh library`
opens Library; `./launch.sh installed` opens its app-list route.

`EMUXR2_PYTHON` and `EMUXR2_SERIAL` override Python and emulator selection.
`EMUXR2_IDLE_FPS=72` is a decoder-load diagnostic that resends frames while XR
tracking is absent. Those frames have timestamp zero and are never presented as
tracked video; leave the default idle rate of 1 for ordinary use.

The guest Touch renderer needs float buffer textures that the emulator's ANGLE
ES 3.1 backend does not implement. The GLES shim translates the controller's
float buffer samplers to shader storage buffers and preserves backing-buffer
updates. R32F, RG32F, RGB32F and RGBA32F are supported by this compatibility path;
integer buffer textures and the full ES 3.2 feature set are not implemented.
After installing the rebuilt firmware, `egl/tests/run_texture_buffer.sh` checks
actual GPU pixels before and after a buffer update. Controller models and pointer
rays have been verified in the guest after a cold boot. Current unique captured
frame throughput with those models is around 35–40 fps on this Mac, even though
Quest1 hardware decoding has separately sustained approximately 72 fps.

The injector consumes the newest complete queued pose snapshot when input outruns
the guest. `bridge/live_smoke.py --controllers --tracking-hz 144 --seconds 60`
exercises that overload and reports captured-pose age as well as frame count.

A short left Menu press also returns to the guest desktop through Android Home,
so the native in-game exit dialog cannot strand you when it is absent from the
casting capture. Holding Menu retains the guest's native recenter behavior. This
fallback runs off the pose thread and never synthesizes a Home press when a
controller disconnects. Set guest property `persist.emuxr2.home_fallback` to `0`
to use only the native guest button flow.

Launch an already installed VR app directly with `./launch.sh app <package>`.
For example, `./launch.sh app com.AnotherAxiom.GorillaTag` resolves its installed
MAIN activity. Native local-account Library currently lists its supported local
apps; it does not expose every sideloaded game in that mode.
