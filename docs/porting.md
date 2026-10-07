# Porting to newer Horizon OS versions (and other hosts)

EmuXR2 was built against Horizon v54 (Quest 2, Android 12, API 32) on an Apple silicon Mac. This is where that
version and that host are assumed, and how to re-derive each piece. Start every port with:

```sh
python3 tools/ota_check.py <dir with img/>   # read-only: lists what still holds and what changed
```

## What depends on the Horizon version

| Piece | v54 assumption | When it changes |
| --- | --- | --- |
| Android base | API 32; `patch.sh` boots the Quest's system, system_ext and product partitions on the API 32 emulator image, and copies that image's vendor partition from fixed offsets (`dd ... skip=$((4096+5607424))`) | A newer Android needs the matching `system-images/android-<api>` image, its vendor partition offsets (`lpdump.py` prints them) and a `build_super.py` run against it |
| Meta's vendor HALs (`hal/`) | `vendor.oculus.hardware.{graphics.composer@1.1, sensors@1.0, sensors_java@1.0, devicecert@1.0}`; struct layouts recovered from v54's interface libraries | New interface versions: regenerate layouts with `hidlsig.py` / `vtable.py` / `structinfo.py` from the new firmware's libraries and extend `hal/` (the old versions keep working for v54) |
| Display mesh (`bridge/flat_mesh.py`) | Header magic `0x56347807`, device type 270, lens separation and panel sizes copied from the v54 runtime's own emulator mesh; loaded through `debug.oculus.distortionFileName` | Compare against the new runtime's mesh header (dump it with `debug.oculus.distortion.log`); if the property is gone, the compositor needs another way to draw undistorted eyes (docs uses the emulator device type) |
| Swapchain size clamp (`egl/egl_macvr.c`) | Hooks `AImageReader_newWithUsage` imported by `libvrruntimeservice.so` | Check the import still exists (`ota_check.py`) |
| Unknown Sources (`bridge/unknown_sources.py`) | OCMS's kiosk library (`q4b_kiosk_enabled`, user `kiosk_user`, `library_database` schema) | If kiosk mode or the schema changed, use the route docs takes: patch OCMS to resolve an empty store fetch |
| Tracking (`input/Injector.java`) | Meta's `TrackingDataInjection` service (`libtrackinginjection-service.so`) | Check the service's interface; the packet the host sends needn't change |
| Frosted glass | Not in the v54 runtime | Newer runtimes blur panel glass only with `oculus_xrruntime:oculus_frosted_glass` and for known devices (see docs's notes); without it panel backgrounds may draw dark |
| `patch.sh` / `patch-extra.sh` edits | Paths of the files it rewrites (build.prop, compatibility matrix, sepolicy, init.ranchu.rc, 64-bit media daemons) | `ota_check.py` lists missing ones |

Everything else is version-independent: the Vulkan and GLES shims (`vk/`, `egl/`) wrap Android interfaces, the stream
(`bridge/`) only needs the compositor's stamp and the display, and the host side doesn't look at Horizon at all.

## Building another version beside v54

```sh
tools/extract_fs.sh ~/MacVRFirmware64            # after payload-dumper-go into ~/MacVRFirmware64/img
cp -cR ~/MacVRFirmware/{aosp,angle} ~/MacVRFirmware64/          # emulator-side caches, version independent
./patch.sh ~/MacVRFirmware64 ~/Library/Android/sdk/system-images/android-32/google_apis/arm64-v8a/system.img
# its own emulator profile (a copy-on-write clone of v54's, so v54's user data is never touched); sysdir likewise
cd ~/.android/avd && cp -cR horizon.avd horizon64.avd && sed s/horizon.avd/horizon64.avd/ horizon.ini > horizon64.ini
EMUXR2_FIRMWARE=~/MacVRFirmware64 EMUXR2_AVD=horizon64 ./boot.sh
```

Meta revises some HAL interfaces without changing their version number. Diff each interface library's `BpHw*` vtables
(`vtable.py`) and `BnHw*` stubs (`hidlsig.py`) against the previous firmware's; `hal/build.sh` picks the layout by a
method only the newer revision has (`MACVR_SENSORS_REV`, `MACVR_COMPOSER_REV`).

### v64 (50837850062000150) status

Works: boot, the sensors (revision 2) and composer (revision 2) stand-ins, tracking service (new DSP entry points in
`compat/hexagon.c`), the VR runtime and the new home environment (it needs `VK_KHR_depth_stencil_resolve`, which
`vk/` now claims to Meta's apps). Open:
- The display comes out as two 800-pixel-wide eyes, upside down: v64's compositor takes the display layout from
  somewhere new (it loads the flat mesh, but the panel geometry differs). Next step: find the source of its display
  size and scan-out orientation.
- The device certificate HAL's native library is gone but `com.oculus.companion.server` still asks for it through Java
  and crash-loops; needs a stand-in that doesn't link Meta's library.
- Unknown Sources: OCMS no longer has `q4b_kiosk_enabled`.

## What depends on the host (Mac)

| Piece | Mac assumption | Linux / Windows |
| --- | --- | --- |
| CPU | arm64 guest runs natively on Apple silicon (Hypervisor.framework) | An x86-64 PC can't run the arm64 guest natively: it needs an x86_64 Android image with arm64 binary translation for Meta's code (docs's approach, below) |
| GPU | gfxstream on MoltenVK (Vulkan composition, `-feature GuestUsesAngle,VulkanNativeSwapchain`) | gfxstream on the native Vulkan driver (NVIDIA: Vulkan 1.3); ASTC is native there, so the ASTC hiding in `egl/gles_macvr.c` may be unnecessary |
| Capture and encode | gRPC frames into a file (`bridge/grpc_follow.py`), VideoToolbox (`bridge/vtenc.c`) | Same gRPC capture; encode with NVENC (`h264_nvenc` through PyAV, or a small native helper like `vtenc.c`) |
| Tools | e2fsprogs from Homebrew, `sips`, `taskpolicy` | e2fsprogs from the distribution; paths in `patch.sh` / `e2put.sh` (`/opt/homebrew/...`) need a variable |

## A Linux PC (x86-64, NVIDIA)

docs runs Horizon on x86-64 Windows like this: an x86_64 Android 14 emulator image, Meta's arm64 apps and libraries
running through Google's arm64 translator (the one Android's x86 images use for arm64 apps), patched where Meta's code
crosses into the x86 system (binder, EGL/Vulkan windows, library paths), and the host GPU through gfxstream. That is
also the route on Linux (KVM instead of WHPX; NVIDIA's Vulkan driver). EmuXR2's pieces that carry over: the Quest
streaming client and protocol, the injector protocol, the flat mesh idea, `ota_check.py`. What doesn't: the arm64 boot
of the Quest partitions on an arm64 emulator image, which needs Apple silicon (or another arm64 host).

Needed to start: SSH access to the PC (with KVM, the Android SDK emulator and NVIDIA's driver installed), and the OTA
to run there.
