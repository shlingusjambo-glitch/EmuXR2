# Emulator desktop controller

Run from the repository:

```sh
./tools/emulator-control/run.sh
```

Open http://127.0.0.1:8766 in a desktop browser. The server binds only to loopback.
It streams one eye of the actual emulator display as continuous VP8 video at a target of 30 frames per second. It reads the emulator's shared framebuffer directly and keeps browser playback close to the live edge.

- **Control emulator**: move the guest mouse, click, scroll, and type. Escape releases capture.
- Hold the right mouse button and move to turn your head, or select **Turn head**.
- Arrow buttons turn the head by ten degrees. **Center head** resets orientation.
- **Send Esc** sends Escape to the guest instead of releasing browser capture.
- Browsers without pointer lock use movement while hovering over the display.
- Input releases on focus loss, page exit, or a three-second client timeout.

Requires a running Horizon emulator with root adb, the project's Android NDK, and
Python with imageio-ffmpeg. The launcher uses the bundled Python when available; otherwise
install imageio-ffmpeg in your Python environment. `EMUXR_PYTHON` overrides Python;
`ANDROID_SDK_ROOT` and `ANDROID_NDK_HOME` override the SDK and NDK paths.
Use `--port 8767` to choose another loopback port.

The server installs the current pose helper as a reversible bind mount and restarts
the pose service. This works again after an emulator reboot by restarting the
controller. Stopping the server removes the temporary head orientation and destroys
its virtual input device. It does not boot or shut down the emulator.

The video stream has no audio. Special browser/OS shortcuts may remain reserved.
