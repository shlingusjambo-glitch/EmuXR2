# Runtime controls and Quest streaming notes

Detailed notes moved from the README.

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
The streamer uses `bridge/.venv/bin/python3` automatically. Set it up once with:

```sh
python3 -m venv bridge/.venv
bridge/.venv/bin/pip install av numpy grpcio grpcio-tools
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

The macOS capture path uses Accelerate to rotate the portrait framebuffer without
changing its pixel bytes, then reuses the video converter and avoids extra full-frame
copies. On macOS, VideoToolbox uses baseline H.264, a single reference frame and
low-delay mode so every input produces a packet immediately while freeing CPU for
the guest. Hardware initialization failures fall back to software; set
`EMUXR2_ENCODER=software` to compare. Other hosts use software encoding and the NumPy rotation fallback. `EMUXR2_PROFILE=1 ./start-streamer.sh`
reports conversion/encoding/send times and head/eye pose agreement. The compositor's
window buffer need not be preserved because its persistent front texture is copied
in full at each presentation; `debug.macvr.preserve_window=1` restores preservation
for comparison. Native presentation timing is available with `debug.macvr.profile=1`
after restarting the runtime.

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

The left Menu button opens the guest's universal menu over the running app (Resume, Quit and the dock), as on
a Quest. Older builds fell back to Android Home, which sent the app to the background; that fallback is now
opt-in: set guest property `persist.emuxr2.home_fallback` to `1`.

Launch an already installed VR app directly with `./launch.sh app <package>`.
For example, `./launch.sh app com.AnotherAxiom.GorillaTag` resolves its installed
MAIN activity. Sideloaded games list under Library's Unknown Sources filter.

The headset client must reproject decoded frames to the current OpenXR display
pose, rather than the future pose used for network tracking. The corresponding
VR4Mac client source change is saved in `bridge/client-display-time.patch`; apply
it from the sibling VR4Mac checkout with `git apply EmuXR2/bridge/client-display-time.patch`,
then rebuild and install its Android APK. Already-applied patches need no reapplication.
