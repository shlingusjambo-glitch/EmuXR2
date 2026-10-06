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
