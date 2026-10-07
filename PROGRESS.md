# OS usability work

## Verified on 2026-10-05

- Read the supplied prior-agent handoff and continued from commit `c09512b`.
- Fresh boot reaches `sys.boot_completed=1`. TrackingDataInjection is registered.
- Bubbles, the dock, desktop coach cards and a uinput mouse render.
- Dismissed both desktop coach cards using guest mouse events.
- Launched Settings using:
  `adb shell am broadcast -a com.oculus.vrshell.intent.action.LAUNCH -n com.oculus.vrshell/.ShellControlBroadcastReceiver --es intent_data systemux://settings`
- Settings is black in VR, despite its producer frame containing the full UI.
  Evidence: `screenshots/08-settings.png` and `screenshots/12-settings-producer.png`.
- No recorded aborts on the first fresh boot.

## Investigation

`debug.macvr.dumpswap=com.oculus.systemux:SystemUX` enables the existing EGL producer
readback after restarting that process. The app needs write access to the destination.
The 1000x625 raw RGBA frame was pulled from `/data/local/tmp/swap-9240-1000x625.rgba`
and flipped vertically to make `12-settings-producer.png`.

The previous handoff's claim that the black panels are solely caused by being logged
out is contradicted by this Settings capture. Library does separately log authentication
and cursor errors; do not treat those as proof its graphics work.

An opt-in EGL image-import readback was built and verified in `egl/egl_macvr.c`.
It must be disabled during ordinary use. A failed experiment showed that resolving
the original GL function through eglGetProcAddress can recurse through interposed
GLES symbols. The diagnostic now resolves it explicitly from libGLESv2_angle.so.

The compositor's 1000x625 EGLImage readback also contains the full Settings UI:
`screenshots/15-settings-compositor-import.png`. Android surface transport works
for this panel; the loss happens while sampling/drawing it in the compositor.
With `debug.macvr.dumpexternal=1`, glDrawElements/glDrawArrays inspect external
textures on units 0 through 7 (20 records maximum). The latest boot logged texture
41 on units 0 and 6 with no sampler object, LINEAR minification (0x2601), and
CLAMP_TO_EDGE wrapping (0x812f). These states do not support the incomplete-sampler
hypothesis. Next inspect the active external-texture shader and its uniforms.
Runtime warnings include UniformsVS CPU size 2720 vs GPU 2712 and UniformsFS
CPU size 1264 vs GPU 1256; these also occur with working home/dock content and
have not been established as causal.

At this checkpoint debug readbacks are disabled and /data/local/tmp is restored
to mode 771. The live emulator is running through an exec session (54434).
The shim changes are installed in emu/emu_vendor.img and loaded by this boot.
No aborts were recorded in the first diagnostic boot. Diagnostics are not a panel fix.

## Remaining acceptance work

1. Restore Android panel composition and verify Settings, Quick Settings and Library.
2. Make desktop mouse and keyboard input convenient and persistent.
3. Verify network, browser, audio, storage, installed app launching and app lifecycle.
4. Verify available account setup flows without assuming login solves graphics.
5. Repeat cold boot and interaction checks; record unsupported hardware features.

The overall OS usability goal remains incomplete. AgentCollab at localhost:8765 was
unavailable (connection refused) during this investigation.

## Continuation: persistence and depth import

- `boot.sh` now runs a bounded guest `sync` before terminating QEMU. Earlier cache
  renames disappeared across boots because guest filesystem writes had not been
  flushed. The synced experiment survived: `compositor-synced-backup` remains
  under `/data/user/0/com.oculus.systemdriver/files/` after reboot. Earlier claims
  that cache rebuilding had failed were inconclusive until this correction.
- Fresh compositor shaders were captured after clearing the cache with sync.
  The debug capture is controlled by `persist.macvr.shaders=1`, writes to the
  precreated writable `/data/local/tmp/macvr-debug`, and uses a sequence number
  so recycled shader names do not overwrite evidence. Captured proprietary shader
  source stays outside the repository, in `/tmp/emuxr-shaders/macvr-debug`.
- The compositor uses empty `Layout0Type` definitions and explicit sampler
  initialization rather than binding declarations for its layer textures. The
  external fragment source is `shader-2428-3-9-8b30.glsl` in that local directory.
  Opt-in sampler inspection reported every queried sampler at unit 0. This
  observation still needs query-error and assignment verification before changing
  program bindings.
- `eglGetProcAddress` now routes glShaderSource/glCompileShader through the existing
  GLES shim, so extension lookups receive the same adaptation as linked GL calls.
- Tested a runtime-only uniform block size rounding experiment, gated by
  `persist.macvr.rounduniformblocks=1`. It rounds the reported minimum to 16 bytes,
  removes CPU/GPU-size warnings, but does not fix the black Settings panel. The
  experiment is disabled at this checkpoint; do not treat it as a panel fix.
- A boot aborted with exit 134 and host `FATAL: Failed to unbox VkDeviceMemory`.
  The immediately preceding error identified depth AHardwareBuffers imported with
  COLOR_ATTACHMENT usage. `makeShadow` now selects DEPTH_STENCIL_ATTACHMENT for
  depth/stencil formats, and shadow copies use each format's correct aspect(s).
  Vulkan and EGL builds pass (Vulkan has three preexisting warnings).
- With that depth-usage fix, boot reached boot_completed and rendered Bubbles and
  the dock, with Settings still black (`screenshots/20-depth-import-fix.png`). It
  then aborted again at 19:39:02 while a D16 AHardwareBuffer export failed. Therefore
  the crash fix was incomplete and sustained runtime stability is not yet proven.
- Export failure had freed the main Vulkan allocation while leaving its handle in
  the output slot; cleanup could try to free that stale host handle again. The
  failure branch now clears both s->mem and *out. This builds and is installed;
  validation was pending on emulator exec session 20912. The tracked shadow memory
  handle is also now cleared when its driver allocation fails, so freeShared cannot
  destroy a nonexistent host allocation. Both fixes build and are installed.
  The latest verification boot is exec session 79612; revalidate it before relying
  on it. The previous boot reached boot_completed with runtime PID 2329 and no
  matching host fatal observed at its checkpoint; sustained stability is unproven.
- The attempt to disable debug properties ran after the VM had crashed, so it did
  not apply. They were subsequently disabled successfully on the next live guest,
  /data/local/tmp was restored to 771, and sync completed before restart.

Next: verify actual sampler assignments and GL query/draw errors on the external
panel program, then correct the demonstrated failure. Full OS acceptance remains
the five items above.

## Draw-time sampler investigation

- Boot session 5186 reached home and black Settings without a matching host fatal
  at the checkpoint. `glUseProgram` confirmed the external program is linked and
  selected; earlier absence of trace was due to checking before Settings was ready.
- Subsequent boot session 16809 captured sampler values immediately before draw,
  rather than inside glUseProgram. Program 6 has external Layer0Depth/Layer0Color
  at unit 0 and active sampler2D MotionVectorTexture, DistortionLUTTexture,
  CodecCurTexture, CodecPrevTexture also at 0. Direct ANGLE queries agree and report
  no query error. This is actual external-program evidence, unlike the earlier
  unfiltered home/dock capture.
- An opt-in experiment `debug.macvr.separatesamplers=1` moves zero-valued sampler2D
  uniforms in external programs to unit 15. It is not a finished compatibility
  fix: validation and visual testing must establish whether the aliasing causes
  the black panel, then any production change must preserve explicit assignments.
- The newest live-window boot is exec session 77067. Enable dumpexternal after adb,
  launch Settings after the dock has appeared, capture validation before enabling
  separatesamplers, then compare rendering. Goal remains active.
- Validation returned success for external program 6 with overlapping sampler types.
  The sampler-separation experiment changed all four 2D samplers to 15, then the
  entire scene became black while runtime stayed alive. It was disabled and its
  mutation code removed; do not promote this to a fix. The next boot restores
  original sampler behavior and adds opt-in draw-result error capture.
- Latest live boot is exec session 12854, with the reverted experiment and draw
  diagnostics installed. Check after Settings is opened (not only boot_completed).
- Correction: viewing the baseline screenshot 25 showed the whole scene was
  already black BEFORE separatesamplers was enabled. Therefore that experiment's
  effect is inconclusive; the glValidateProgram diagnostic itself is a possible
  interference source. Its draw-time call was removed too. Latest boot session
  returned by the tool should supersede 12854 (which was interrupted during boot).
  Sampler assignment and validation-success logs do not prove a rendering fix.
- Boot session 93576 is running (QEMU PID 69096 at checkpoint). External program6
  draw-result diagnostics report 0x502 (GL_INVALID_OPERATION) on 19/20 captures;
  one capture reported 0x500. Sampler types share unit0, which is a concrete
  candidate for GL_INVALID_OPERATION. Error capture currently follows draw and
  uniform queries, so isolate pre-existing errors before asserting sole cause.
- Screenshot29 is entirely black despite runtime continuing. This persisted after
  removing the validation call and sampler experiment. Earlier session16809 had
  visible home/dock. Diagnostics were enabled earlier in the newer boots; next
  step should first restore a debug-off visible baseline, then distinguish shader
  sampler aliasing from diagnostic interference. Do not claim latest live UI is
  working. No matching host unbox fatal observed in latest boot log.

## User-observed successful Settings boot

The user reports Settings visibly rendered during recent boots, approximately
between the glUseProgram/draw-time tracing change and the sampler experiment.
Treat the black-panel problem as a regression/intermittent failure, not evidence
that the UI has never rendered. No precise successful screenshot was identified.
A debug-off cold boot restored the Bubbles environment (screenshot31), demonstrating
that the entirely black screenshots are not a permanent driver failure.

Testing `persist.macvr.samplerdefaults=1`: initialize optional vertex sampler2D
uniforms MotionVectorTexture/DistortionLUTTexture/CodecCurTexture/CodecPrevTexture
to units8..11 immediately after glLinkProgram/glProgramBinary, compositor only.
Runtime's subsequent assignments take precedence. This avoids per-draw mutation
and also covers cached binaries. Experimental, not yet accepted as production fix.
Latest live-window boot exec session15868; diagnostics should remain off until
Settings's actual display behavior is observed.
- Sampler initialization made all20 captured external draws return GL_NO_ERROR;
  direct queries confirmed optional2D samplers8..11 and external samplers0. This
  is stronger causal evidence than the prior glValidateProgram success report.
- The initial experiment also altered ordinary home programs, and the image was
  entirely black. Restricted initializer to programs declaring
  GL_SAMPLER_EXTERNAL_OES; ordinary2D home/dock programs retain native defaults.
  Latest boot session81223 is testing this scoped version with diagnostics off.
- Scoped initializer succeeded: the user said "it worked" and screenshot34 shows
  the complete Settings UI in both eyes with home and dock intact. Runtime2354,
  samplerdefaults1, dumpexternal unset, no matching host fatal at checkpoint.
- App Library panel chrome now visibly renders (screenshots35/36), but the content
  stays on a spinner. Logs report logged-out LibraryCacheRefresher and an
  AsyncQuery SecurityException/Component access not allowed. This is a separate
  content/account/access issue, not the prior external-panel black draw failure.
- Initializer is now enabled by default in source; persist.macvr.samplerdefaults=0
  opts out. It initializes only compositor programs with active external samplers,
  immediately after linking or cached-binary loading. Ordinary home shaders are
  untouched. Build and diff checks pass; live guest still uses the opt-in1 build.
- Networking was genuinely absent: no saved Wi-Fi networks, no default network,
  IP and DNS ping failed. Scan discovered emulator AP AndroidWifi; connected with
  `cmd wifi connect-network AndroidWifi open`. Android now reports WIFI/VALIDATED,
  default network100, address10.0.2.16 and DNS10.0.2.3. example.com resolved and
  returned an ICMP reply (emulator ping timestamp/payload warnings still present).
  Saved network and filesystem flushed. Browser launch to https://example.com is
  the next application-level network verification; account errors remain separate.
- Connecting network exposed deviceauthserver crashes: containsAlias threw
  FILESYSTEM_ERROR from our HAL's generic Result::ERROR=1 stub. Firmware's generated
  Result.smali proves1 is FILESYSTEM_ERROR, not a generic unavailable code.
  HAL now returns OK,false for key-existence queries (no keys), and explicit
  UNSUPPORTED_COMMAND0x66 for secure-hardware operations. No credentials or
  attestation are synthesized. Build passed with one upstream header warning.
- Latest boot session79484: deployed corrected
  HAL and default-enabled scoped GLES fix; persist.samplerdefaults cleared to test
  default behavior, Wi-Fi saved/synced. Test Settings after startup, network autojoin
  and deviceauth crash behavior. Browser ACTION_VIEW did not resolve; firmware
  exposes PanelActivity/PanelService through com.oculus.vrshell.SHELL_MAIN.
- Quick-settings route test was obscured by deviceauth crash dialog; screenshot37
  is that dialog, not Quick Settings. Overlay also aborted with OpenXR runtime
  swapchain failure while showing dialog, so sustained OS stability is incomplete.
- Cold-boot verification succeeded: screenshot38 shows full Settings plus home/dock,
  and Settings's Wi-Fi tile shows AndroidWifi. Graphics property was cleared, so
  this verifies default-enabled behavior; diagnostics were off. Wi-Fi autojoined and
  default network100 returned. deviceauthserver alias queries return status0 and
  unsupported hardware operations are caught/reported instead of the earlier fatal
  FILESYSTEM_ERROR crash. A separate ShellOverlayVR SIGABRT appeared at20:01:19;
  investigate its swapchain allocation failure next. Full OS goal remains active.

## Overlay depth and usable browser/input

- Overlay crash is traced to a D16 depth swapchain (1024x1024,2 layers): exporting
  its first shadow asks the host for BLOB and fails allocation. An independent
  native AHardwareBuffer probe found D16,D24,D32 all unsupported (allocation7),
  while RGBA8 succeeds. Explicit depth allocation is therefore not a solution.
  Full depth transport needs an actual conversion/staging implementation or driver
  support; do not suppress the error and claim depth is working.
- Browser successfully loaded https://example.com in both eyes (screenshot39).
  Correct launch is ShellControlBroadcastReceiver LAUNCH with intent_data
  com.oculus.browser/.PanelActivity and uri=https://example.com. Added launch.sh
  for Settings, Library and Browser; invocation verified. ACTION_VIEW alone fails.
- Extended uinput helper with keyboard key up/down commands and checked device
  creation. Android dumpsys input reports KEYBOARD|ALPHAKEY|CURSOR|EXTERNAL,
  Generic.kl/Generic.kcm, enabled at /dev/input/event13. Added input/build.sh and
  input.sh to rebuild/deploy/connect this helper. Linux key108 down/up emitted
  during getevent verification. Desktop host event forwarding still needs work.
- Live emulator remains session79484; browser runs with network. No restart was
  needed for browser/input changes. Goal remains active; depth/overlay, Library
  content, host input, audio and application lifecycle still require completion.

## Quest streaming stability (2026-10-06)

- Symptom: Quest 1 stream glitched/froze, no 3DOF, no controllers, shell crash-looped
  (vrshell SIGSEGV fault addr 0x0 on ShellMainVR, 20x) and trackingservice aborted 68x
  ('Controller capabilities must include one of {Selftracked, Constellation}').
- Root cause 1 (proven by disassembly): vrshell's extension-gated loader keeps a NULL
  glTexBufferEXT unless "GL_EXT_texture_buffer" appears in glGetString (libshell.so:
  strstr gate, then eglGetProcAddress into the table; the cursor renderer calls the
  table entry and jumps to address zero). Guest ANGLE resolves the entry (non-NULL,
  verified with a guest probe binary) but does not list the string. Trigger is the
  first controller cursor (CoTextureCursorRendererGLES). Reproduced live: 12 s of
  controller-present injection crash-looped the shell; head-only injection is harmless.
- Root cause 2: bridge/stream.py ignored quest_proto.device_profile, so the Quest 1
  (1216x1344 eyes) was fed 1920x1088 @ 60 fps / 25 Mbps, which its AVC decoder cannot
  sustain. Also fixed: Guest.connect() raced the injector's JVM boot (the adb forward
  accepts while the guest leg is refused, leaving a dead pose socket); start_guest()
  now waits for "Injector: listening" (wait_injector).
- Fixes: stream.py negotiate() applies device_profile (Quest 1: 2432x1344 @ 30 fps /
  20 Mbps with the rendered FOV in CONFIG, 30 fps pacing, reformat rescale) plus the
  readiness wait; egl/gles_macvr.c advertises GL_EXT_texture_buffer to com.oculus.*
  processes (scoped; other apps see the unchanged string); CONTROLLERS now defaults on
  (EMUXR2_CONTROLLERS=0 opts out). New bridge test: test_stream_config.py.
- Verified: 839 controller-present + 737 button/stick/trigger packets, zero crashes or
  aborts, same vrshell PID throughout; clean reboot, zero tombstones since; Quest 1
  encode path benchmarks 124 fps (headroom); bridge tests pass (10 unittest +
  stream_config + touch_controller). New shim confirmed live in the guest by md5 and
  the "advertising GL_EXT_texture_buffer" log line.
- The trackingservice capability abort is NOT reproducible: disassembly shows the check
  reads the paired-controller flags at +0x128 and accepts bit 9 (0x200 Constellation,
  which hal/sensors.cpp sets). The 23:20 loop was transient; no HAL change was needed.
- Deployment note: the live guest runs the super image baked at boot; debugfs edits to
  emu/emu_vendor.img only take effect after a boot.sh reboot (guest page cache + baked
  super — restarting processes alone loads stale code). Rebooted 00:19, fix verified live.
- Still needs the user wearing the headset: end-to-end tracking/video, cursor-model
  visibility (the cursor shader declares `#extension GL_EXT_texture_buffer : require`,
  which ANGLE's compiler may still reject -> invisible cursor, no crash), jitter feel,
  scale feel. The shell currently shows its loading void when idle (no layers submitted;
  panels did not open this boot) — home-content work stays with OS usability, not streaming.
- Follow-up flap found while handing off: with a static scene the emulator display
  freezes (SurfaceFlinger idles with no damage: 0 shm frames in 6 s), so stream.py sent
  nothing at all — the old 1 Hz keep-alive only fires when frames flow — and the Quest
  client hit its 5 s socket timeout and flap-looped through HELLO/CONFIG (decoder
  reconfigured cleanly at 2432x1344, verified in the client log). Session.run() now
  re-sends the last encoded frame at 1 Hz while frozen (Session.emit refactor, newest
  tracking time), keeping the link up with no re-handshake. Covered by a new
  configure+emit socketpair round-trip in test_stream_config.py. While verifying,
  the user put the headset on: 153 flap-reconnects confirmed the diagnosis, and after
  deploying the fix the link held with zero reconnects (1 Hz keep-alive, pose 0
  behind, decode ~1 fps, no drops). Extended once more for a fully frozen display
  (no frame ever sent): the first keep-alive now uses the current display contents
  via Display.current() with the stamp band blanked. Same PIDs (trackingservice,
  vrshell) through all controller bombardments, the reboot, and live Quest use.
- Added start-emulator.sh (fast boot from the assembled sysdir, no rebuild; use
  boot.sh after hal/egl/vk/compat changes) and start-streamer.sh (preflight + exec
  bridge/stream.py in the foreground). Preflight verified live; the reboot path itself
  reuses boot.sh's flags and was not re-run to avoid disrupting the live session.


## Goal continuation, 2026-10-06: flashing navy and invalid competing poses

- User objective is in ~/.codex/attachments/a96199c9-ffb8-4b89-8aee-4f64acc22815/goal-objective.md. User explicitly authorized autonomous work while asleep. Goal remains incomplete.
- AgentCollab localhost:8765 refused all startup/read/claim calls. Existing uncommitted changes predate this work; preserve them.
- Proven live black-scene cause: compat/pose.sh reads FOUR quaternion values from /data/local/tmp/macvr-head, which contained SEVEN position+quaternion values (`0 1.6 0 0 0.0697 0 0.9976`). Invalid quaternion is injected every 50ms, competing with Injector.java. Stopping macvr-pose and injecting identity immediately restored Bubbles and Library chrome. Screenshots goal-single-injector and goal-capture-restored show the result. Both helpers running also means headset pose is repeatedly overwritten, explaining a source of jitter.
- stream.py now stops desktop macvr-pose before starting Injector. compat/pose.sh supports both four and seven component files. Saved head file normalized live. Updated script bind-mounted live and written into work/system.img with e2put; boot.sh rebuild still needed for persistence in sysdir.
- Guest battery was unpowered, mStayOn=false, mWakefulness=Asleep despite svc power stayon true. Stream startup now sets emulated AC power first; live Awake/StayOn=true verified.
- Display now starts `adb emu screenrecord webrtc start 72` and uses returned shm name instead of opening stale shared memory blindly. Counter advanced 105 frames/2s live. This is framebuffer production, not headset decode acceptance.
- Frozen resends now preserve captured tracking timestamp instead of attaching newest pose to stale pixels. Capture watchdog runs even when framebuffer has no updates. Left menu now maps directly to guest Home on press and release, without Back/tap or hold delay; inactive controllers return released values. start-streamer selects .venv automatically with EMUXR2_PYTHON override. start-emulator handles unset EMUARGS and syncs guest before shutdown.
- Tests passed: ten protocol unittests, test_touch_controller, test_stream_config (including real x264 encoding/socket roundtrip and frozen display). Add a regression asserting a nonzero captured timestamp is unchanged on frozen repeat; currently frozen test covers timestamp zero only.
- Library still spins: logs say OCMS empty/uninitialized, unknown-source fetch successfully finds com.AnotherAxiom.GorillaTag. Need inspect native route/filter behavior and get usable local apps without fabricating entitlement/authentication.
- Native Quest renderer clear color (0.02,0.03,0.06) matches reported navy. It discards video if timestamp doesn't match 144-entry history or age >1s. Existing repeated frames with newest timestamp incorrectly rebound to current pose; no wearer verification yet. Quest asleep/disconnected after startup. Decode remains unverified.
- Current emulator boot log ~/MacVRFirmware/boot.log, runtime healthy ~68-71/72 fps in VrApi logs. Streamer exec session 86891 waiting for headset, start-emulator session 43766. Capture restarted live; Injector remains alive (port7791). macvr-pose stopped. Synthetic stationary head packet seq1000 injected for verification. Need restart streamer after further code edits.
- NEXT: release stale input on tracking loss in Injector; move reconnect work away from headset packet reader; add timestamp regression, test 72fps encode feasibility and expose validated rate choice; solve Library local-app filter; deploy full image/reboot and repeat stability checks. Examine proprietary files only outside repo. Goal not complete.

### Autonomous goal continuation: controllers and installed graphics (2026-10-06)

- The previously installed HAL differed from the current build and did not publish paired Touch controllers. Built and installed the current HAL, then verified TrackingService exposes both paired 6DOF remotes. Live testing showed the handedness bits were reversed: 0x20 is Left, 0x10 is Right. Corrected the HAL flags and cold-booted; `/tmp/emuxr2-coldboot-tracking.txt` confirms A000 Left at x=-0.25 and A001 Right at x=+0.25, both valid. Trigger injection reached 0.85 in both guest remotes.
- Merely advertising GL_EXT_texture_buffer exposed a new VrShell abort: its controller morph shaders require samplerBuffer, unsupported by the installed ANGLE/Vulkan ES 3.1 compiler. Confirmed using a real GLES context; glRequestExtensionANGLE cannot enable it. Implemented float buffer-texture compatibility in `egl/texture_buffer_macvr.h`: float samplerBuffer shaders use shader storage buffers, and buffer texture bindings follow the application's sampler unit and backing buffer. Supported formats are R32F/RG32F/RGB32F/RGBA32F. Shader/program metadata is cached to avoid uniform reflection on every input update. Integer formats and general ES 3.2 buffer-texture features remain outside this compatibility path.
- Built and installed both graphics shim libraries into the firmware vendor image. Cold boot loaded the new HAL (md5 e9ea531a80d603d43ec28da93fd3ac96) and shims without hot mounts. Guest controller models and pointer rays are visible; screenshots `goal-buffer-controllers.png` and `goal-coldboot-library-ready.png` show actual rendered models. The latter also shows Library loaded with Settings and Files. No new fatal signals/shader failures in the cold-boot log during the initial 45-second controller session.
- Added an on-device GLES test (`egl/tests/run_texture_buffer.sh`) that renders through the installed shim, reads pixels, updates the buffer and verifies updated pixels. RGBA32F read/update passes with GL error zero (64/128/191/255 then 191/128/191/255). This is real rendering validation, not just source substitution testing.
- Live controller session: 1663 captured frames in 45 seconds at 2432x1344 with exact tracking timestamps, sequential frame IDs and IDR recovery. Current guest fresh-frame throughput with rendered controllers/Library is roughly 35–40 fps, lower than the earlier session without rendered controller geometry. Do not conflate this with the separate Quest1 decoder throughput test (~70–72 decoded fps while XR consumer sleeps). Headset smoothness at 72 unique frames is not established.
- Java injector now drains complete queued pose snapshots before injection, retaining the newest and preserving a partial following packet. Drain is bounded at 64 snapshots. `input/tests/LatestPacketInputTest.java` verifies latest selection, partial preservation and bounded work; helper build includes the new class in DEX. A 144 Hz live tracking overload/pose-age test is in progress. `bridge/live_smoke.py` now reports median/p95 captured pose age and fails a configurable latency threshold.
- Latest Android client APK (unique decoder PTS mapping extracted into VideoTimestampMap) installed successfully on connected Quest1 1PASH9AZ2Y9397 after the cold boot. Quest2 hardware is not attached; its physical interaction remains unverified.
- Earlier Python config/input/recovery/protocol tests pass again. EMUXR2_FPS now validates 30–72 to match the client CONFIG constraints; corrected stale 30-fps profile/controller-support comments.

Current processes: foreground emulator held by exec session 1619; ordinary streamer session 7449; overload smoke session 77104. AgentCollab service remained unavailable (connection refused); no other agents were dispatched. Firmware binaries/proprietary shader dumps remain outside Git.

- 144 Hz overload completed: 2211 captured frames/60 s, median pose age 80.8 ms, p95 127.1 ms (250 ms threshold), no growing backlog. Latest-snapshot draining is installed in the running injector.
- Expanded installed-shim GPU test: all four float formats R32F/RG32F/RGB32F/RGBA32F read and update correctly, GL error zero.
- Quest client now drops untracked/unknown decoder output before SurfaceTexture presentation, preserving the tracked texture while still counting every decoded output for diagnostics. Mapper tests pass; APK rebuilt successfully and installed. No native main.cpp change was needed: PosePrediction already rejects repeated source timestamps and stalls.
- Installed GorillaTag launches and renders actual game hands (screenshots `goal-game-launch.png`, `goal-game-menu.png`). A 60 s moving-controller game stream produced 3100 captured frames (~51.7 fps), median pose age 67.5 ms / p95 90.5 ms. Its backend authentication reported PlayFab errors, so online gameplay was not validated. No bypasses or entitlement changes were made.
- Important menu finding: left Menu reaches ServiceInputManager Home down/up and queues native exit dialog (`systemux://dialog/exit`) in-game, but that overlay is not visible in the capture. Android Home successfully returns the guest to desktop. Added an off-thread short-release desktop fallback in the injector, preserving native long-hold recenter and avoiding navigation on controller loss/timeout. `persist.emuxr2.home_fallback=0` opts out. HomeButton pure-Java tests cover short releases, long holds, repeat release, controller loss, timeout reset. Live fallback transition test ran as session 39098 and is reported below. Streamer now session 56293, emulator still 1619.
- `launch.sh app <package>` now resolves an installed MAIN activity safely and launches it. Native local-account Library does not show every sideloaded game; direct launch supplies a practical route without fabricating entitlements. Corrected installed route to `/library/installed`.
- Live short-menu fallback completed: 774 captured frames/20 s, median pose age 75.4 ms / p95 122.4 ms. VrFocus changed from GorillaTag to ShellEnv/VrShell and screenshot `goal-home-fallback.png` shows the guest dock and controller rays. This verifies actual navigation, not just button-state receipt.
- Final Quest1 hardware decode check is running with `EMUXR2_IDLE_FPS=72`; this diagnostic intentionally emits untracked timestamp-zero frames when XR tracking is absent. The client decodes them but does not present them. It cannot establish in-headset comfort or Quest2 interaction. Diagnostic streamer 76716, device logcat 40788; restore ordinary configuration after collecting results. Goal remains active because physical headset presentation/comfort and Quest2 hardware cannot be verified while the user sleeps.
- Final installed Quest1 client decode diagnostic sustained ~70.4–71.7 decoded fps at 2432x1344 H.264 (approximately 13–14 Mbps) for over 40 seconds. `release_fps=0` is intentional: no XR tracking samples while the headset is unworn, so all timestamp-zero diagnostic output is rejected for presentation. Therefore receive-to-release metrics are undefined/zero in this test, and it proves decoder throughput only. Saved relevant device output to `/tmp/emuxr2-quest-final-decode.log`.
- Final cold boot now runs emulator session 51590 and ordinary streamer session 58130. Installed-shim four-format GPU test passes after reboot, without hot mounts. An additional 60-second/144 Hz controller overload check is running (1664).
- Tried tracked presentation without a wearer using actions actually registered on the Quest: `com.oculus.vrpowermanager.prox_close`, then restoring with prox_far and automation_disable. This woke the device and produced one real tracking sample, but the headset's system shell kept the app paused (`ClearActivity`/VrFocus), so video age stayed unavailable. Do not treat decoder releases as XR presentations. Restored original sleeping state via SLEEP, proximity automation disabled, stay_on_while_plugged_in remains its original 7. Client was force-stopped during the attempt; no new tracked-presentation claim is supported. Physical comfort/Quest2 acceptance remains unverified.
- Final cold-boot 144 Hz soak passed: 2257 captured frames/60 s, median captured-pose age 85.7 ms / p95 130.4 ms (250 ms limit), exact timestamps, ordered frame IDs, fragmented HELLO and IDR recovery. Cold-boot log has no new fatal signals or failed shaders. Guest remains Awake/stay-on, desktop macvr-pose service stopped while streamer owns injection. Ordinary streamer 58130 and foreground emulator 51590 remain running and ready for the headset; Quest is back Asleep with proximity automation disabled. Tests/builds/syntax checks and both repositories' diff whitespace checks pass. No commits/pushes were made; pre-existing unrelated edits were preserved.

### Goal follow-up audit (2026-10-06)

Previous goal turn made concrete progress: graphics/HAL firmware and client APK installed, cold-boot and overload tests completed. Current emulator and ordinary streamer handles revalidated live; Quest1 remains connected but asleep, Quest2 is absent. Added direct live controller-state evidence beyond poses: held trigger=0.85, grip=0.90, stick click and X/Y (left)/A/B (right) all appear in both TrackingService remotes with the correct left/right handedness. The first command output showed uppercase TR/TP/B0/B1/G and both analog values. A later saved snapshot occurred after the client disconnected, so its held-input assertion failed because the controls had already released; collect a synchronized held snapshot next. This proves the native guest receives those controls rather than merely accepting transport packets. Physical Quest2 input and in-headset comfort/navy-flash acceptance remain unverified; the original objective is not complete.
- Synchronized native input check passes: `/tmp/emuxr2-held-inputs.txt` asserts both remotes' trigger=0.85, grip=0.90 and pressed trigger/stick/face/grip controls. After client disconnect, `/tmp/emuxr2-released-inputs.txt` asserts trigger/grip zero, all digital inputs released and both sticks centered at 32768. Native control reception and disconnect recovery are verified on the current firmware. Stream remains ready for a new connection.

### Completion/blocker audit

The previous goal turn made progress by proving full native controller button/analog reception and release on disconnect. The remaining blocker is unchanged across the last three goal turns: Quest2 hardware is not attached, and the connected Quest1 is asleep/unworn; its system UI prevents a tracked-presentation acceptance test through the attempted remote proximity override.

Current authoritative checks: ADB lists only Quest1 plus emulator; Quest1 reports Asleep/proximity false; guest boot complete=1; VrShell PID 2173 remains live; installed HAL MD5 matches the current build; current cold-boot log contains no fatal signals or shader failures; normal streamer handle 58130 remains live and waiting for a client.

Requirement audit:
- Stabilize jitter/lag: invalid quaternion and competing pose writers fixed; reconnect/input backlog and captured-pose identity verified under load (p95 ~130 ms). In-headset comfort and absence of navy flashes are not proven.
- Quest2 controllers: shared protocol mapping, native paired controller states, models/rays, buttons/analogs and disconnect releases verified in guest. Actual Quest2 device/action behavior is unverified because that hardware is absent.
- Left Menu / guest Home: native button press/release and short-release return from installed game to guest desktop verified; right physical system menu is not synthesized by the guest mapping. Physical device acceptance remains unverified.
- Quest1 streaming: installed APK and native-resolution H.264 hardware decode verified; tracked XR presentation is unverified while unworn/system paused.
- Library loading: native local-account Library rendered after cold boot, with Settings/Files. Sideloaded app launch has a direct installed-package route; online account-backed library/entitlements are not manufactured.
- Approximately 72 fps decode: installed Quest1 client sustained approximately 71 decoded fps at 2432x1344. Unique guest rendering is scene-dependent (~35–40 Home/Library, ~52 tested game); do not claim 72 unique displayed frames.

All currently actionable code/build/deployment and regression checks are completed. Repeating those tests or adding speculative changes cannot establish the missing physical acceptance. Goal is incomplete and blocked on external headset availability/tracked presentation, with all fixes retained and the normal emulator/streamer ready.

### Stability, scale and performance (2026-10-06)

- Crash loop root cause: the rebuilt vendor image had fallen back to the API 32 emulator ANGLE, which leaks ~400 MB/s
  inside Meta's compositor (`std::vector` growth in TimeWarp); lmkd killed vrruntimeservice every 10-15 s and took
  VrShell, ShellEnv and Guardian with it. `patch-extra.sh` now installs the newer ANGLE kept in `~/MacVRFirmware/angle`.
  Runtime holds ~50 MB.
- Scale: the guest's raw floor is at y = -1.675 with no Guardian (`RawFloorHeight`), and Quest stage poses were
  injected unshifted, so the wearer stood ~3.2 m tall. `stream.py` adds `GUEST_FLOOR` to head and controller heights;
  the injector and `macvr-pose` idle at raw y 0 (the runtime's standing eye height). IPD is normal (63.5-64.9 mm).
- Near UI swimming: the client predicted position the whole stream latency (up to 100 ms), which is never corrected
  on display. Orientation keeps the full lead; position and controllers use a quarter of it.
- Capture recovery: after a swap error the runtime ignores new capture requests. The helper sends STOP before BEGIN,
  and the streamer restarts the runtime when a second capture restart still yields nothing.
- Performance: native 1920x1088 encode (no 2432x1344 upscale), ANGLE `asyncCommandQueue` (the compositor no longer
  stalls in vkResetFences on every swap), Guardian disabled (it retried spatial anchors ~15/s and leaked), 6 vCPUs,
  console logcat at W. 60 s synthetic soak after a cold boot: median pose age 70 ms, p95 92 ms (was 81/116).

### Native feel: controllers, audio, brightness, environments, recovery (2026-10-06, evening)

- Controllers: `bridge/calibrate_poses.py` runs the client inside the guest, injects known IMU poses and solves Horizon's
  fixed transforms. Head: the injected pose is the eye centre (no offset); app floor 1.675 m below raw y 0. Touch:
  grip = IMU * (60 deg about x, 3 cm down, 4 cm back). `stream.py` injects grip * that^-1; round trip in the guest:
  0.00 cm, <0.6 deg.
- Audio: `input/Audio.java` records the guest's mix through the remote submix (playback moves there while the host
  listens) and serves 10 ms PCM chunks; the streamer forwards them as VR4_AUDIO. Verified end to end.
- Brightness: Horizon's slider (`screen_brightness_for_vr`) now dims the stream (20-100 %, YUV lookup tables, ~6 ms a
  frame only below full); the guest starts at full once.
- Environments: only Bubbles ships in the OS; others come from the store. Environment APKs already on the owner's own
  Quest install and appear in Settings > Environment (`systemux://settings/environment`).
- Client: Android 12 hides `libopenxr_forwardloader.oculus.so` from apps without `<uses-native-library>`; without it the
  Khronos loader can't reach Meta's runtime (the same for any S-targeting game).
- Recovery: no stamped frames for 5 s restarts the capture, then the runtime, on a background thread so keepalives
  hold the headset; the counter only resets after 30 continuous frames. A headset reset during accept no longer kills
  the streamer.
- Performance: asg graphics transport (ring buffer instead of a blocking pipe per Vulkan call), 72 Hz guest display,
  eye buffers capped at 1280x1344 (`debug.oculus.textureWidth/Height`). The compositor still waits on gfxstream round
  trips (buffer acquire, sync creation); typical stream 30-47 unique fps, pose age ~80-110 ms.
- Meta account: blocked. Meta's servers authenticate the headset with its factory device identity certificate
  ("Unable to load insecure device identity certificate"), which an emulator has no genuine copy of.

### Quest 2: true scale, sharper, no black edges, Quest 2 controllers, Unknown Sources (2026-10-06, night)

- Zoomed-in view: the Quest 2 ran the Oct 3 client, which ignores CONFIG "fov" and stretched the 80-degree capture over
  the headset's whole view (and slid it while turning). The client now advertises `features: ["fov"]`; the streamer
  installs this repository's client over USB on a headset without it.
- Black edges: the capture's field of view is 80 degrees plus the request's `video_capture_aspect_ratio_fov`
  (libvrruntimeservice: 80 - stabil crop + adjustment). 9.4 more reaches the edge of Horizon's eye images (wider is
  black inside the capture). The client re-aims each eye from the frame's pose to the current one and stretches edge
  pixels when a turn outruns the frame, instead of showing black.
- Resolution: 2560x1600 panel, capture 1280x1600 per eye (the eye images' shape), Horizon's default eye buffers,
  40 Mbps. Re-measured with `bridge/calibrate_capture.py`: f 662.7 px, centre 674.3/616.6, 776.8.
- `bridge/motion_check.py` turns the head at up to 90 deg/s and checks each captured frame against its pose number:
  UI mean 0.3-0.4 deg off; ~8 % of frames lead their stamp by ~30 ms (Horizon extrapolates the injected pose).
- Controllers: PairedControllerInfo's first word is the controller type; 1 makes the runtime load "Oculus Touch
  Quest 2" (0 was Quest 1, 2 Touch Pro, 4 placeholders). `persist.emuxr2.controller_type` overrides.
- Black blocks in panels: HWUI partial redraws (buffer age) on a swapchain that doesn't keep old pixels; whole-frame
  redraws via `debug.hwui.use_buffer_age=false`, `debug.hwui.use_partial_updates=false`.
- Emulator crash (`unallocate: freeSubblocks.insert`) after a runtime restart: the restarted runtime served panels
  65536x65536 swapchain placeholders before the EGL shim's clamp was hooked; it now hooks at the first context.
- Unknown Sources: SystemUX lists every non-library, non-system app, but first asks OCMS for the library, which threw
  "Invalid credentials or user id" without an account, so the list was always empty. Meta's kiosk mode
  (`q4b_kiosk_enabled`) gives OCMS a local "kiosk_user"; `bridge/unknown_sources.py` (run at stream start) turns it
  on and files the environments under Environments. Gorilla Tag now lists.
- Login: the browser opens without one. Store, TV, Chats, People, Explore show errors or loading screens because they
  are Meta's online services and need a Meta account; see the device certificate note above.

### Headset projection continuation (2026-10-07)

- User confirms Unknown Sources works; no changes made to its implementation.
- Found concrete regression in the new flat-display stream: CONFIG fov held tangents, while the Android client
  expects XrFovf angles in radians and takes tan() in its shader setup. Changed wire FOV to radians and adjusted
  calibrate_display intrinsics to convert angles to tangents explicitly. Added regression checks for both eyes.
- test_stream_config and live_smoke now advertise the fov feature so diagnostics cannot trigger USB client reinstalls.
- Connected headset is Quest 2 1WMHHA641Q2123. Corrected CONFIG verified sent to it. Physical headset is asleep;
  wearer confirmation of zoom/stretch/eye overlap is still required. An async question was sent.
- Baseline synthetic full-resolution/controller test: 744 frames/20 s (37.2 fps), pose age median 97.7 ms, p95 124.2 ms.
  1280x1408 guest eye buffers gave 761 frames/20 s (38.1 fps), 96.0/119.9 ms. Reverted guest texture overrides.
- Display calibration with smaller buffers: left fx/fy 590.7/687.9 vs intended 595.2/694.9, right 591.7/687.9; centres
  within 4 px of intended. Correct geometric FOV is supported to about 1%, unlike the previous wire tangent values.
- Smaller 1920x1200 encoded stream did not materially improve throughput. Removed experiment, restored full resolution.
- Motion diagnostic reported large offsets (mean ~3 deg, 57% UI >2 deg). This requires further controlled investigation;
  the test uses image matching against an animated environment and does not by itself prove the fence cause.
  Temporary fence tracing showed full viewport 2560x1600 with left-eye scissor at fences, not conclusive early publish.
  Removed tracing and restored pre-trace installed EGL binary; debug.macvr.presenttrace=0. No persistent firmware changes.
- Projection/config/protocol tests pass. Goal remains incomplete: native-rate FPS, motion alignment, sustained stability,
  and wearer verification are not established. No commits or pushes. AgentCollab localhost refused connections.

- User confirmed on 2026-10-07 that zoom, eye overlap, and vertical stretch are fixed on Quest 2. Remaining focus is FPS and motion jumps.

### FPS and motion continuation (2026-10-07)

- Committed and pushed the user-confirmed projection fix as `86d7cd0` on `main`.
- Reduced host work without lowering resolution: one owned upright snapshot, Accelerate's byte-preserving portrait rotation (about 2.3 ms versus NumPy's 8 ms locally), shared-input PyAV frames, a reused color converter, and no redundant padding/retained-frame copies. Conversion fell from roughly 5.3 ms to 2.4 ms.
- Native front-buffer rendering retains its contents in our persistent texture. The fully overwritten output window now defaults to destroyed swap buffers, avoiding redundant preservation. The override and optional native/host timing logs remain available.
- Encoded packets retain the captured pose associated with their own encoder PTS, including delayed output. Regression checks cover delayed packets, immediate H.264 decode, stereo seam colors, snapshot ownership/orientation, and counter races.
- Synthetic 30-second full-resolution controller tests improved from 37.2 FPS / 97.7 ms median pose age to 44.2 FPS / 80.8 ms. Real Quest 2 delivery after cold boot ranged approximately 40–68 FPS depending on scene/load, generally 50–65 FPS in the initial Settings observation, with zero steady-state decoder drops. This is not sustained native 72 FPS or a verified headset comfort result. Gorilla Tag's guest log briefly reported 64/72 FPS; its actual captured stereo game scene was inspected.
- Rejected experiments: lower eye-buffer height, VideoToolbox (slower despite successful low-delay configuration), restricted x264 threading, background capture (no meaningful latency gain), first-draw pose latching, duplicate position suppression, and early head-marker publication. Tracking changes and the latch experiment were reverted before the cold boot. No projection changes were made in this pass.
- Motion diagnostics can select a static panel ROI and save JSON, and now calculate turn velocity from actual injection timestamps. Residual motion alignment remains unresolved: baseline mean UI error was about 1.76 degrees; latch/dedup/early-marker trials were worse. Client head/eye disagreement measured below 0.2 degrees in an observed interval, so it does not explain the larger errors.
- A cold boot initially failed the emulator's disk-space check. Deallocated zero ranges in the generated firmware disk, verified its SHA-256 unchanged, and successfully booted. `build_super.py` now makes generated disks sparse on macOS so repeated builds do not retain those zero allocations. A mixed-data/zero test verified byte preservation.
- Cold boot reached `sys.boot_completed=1`; the installed EGL library hash matches the build, and destroyed swap behavior was accepted. The ordinary headset stream was restored. Unknown Sources remains working. Headset confirmation of remaining jumps and sustained Gorilla Tag performance is still required; the original native-feeling goal remains incomplete.
- Subsequent cold-boot observation found recurring `com.oculus.vrshell:Overlay` aborts during swapchain creation (`XR_ERROR_RUNTIME_FAILURE`) and one private shared-memory mapping failure (`errno 13`). These coincide with later performance degradation and are being investigated; cold boot is not evidence of sustained stability. Head/eye orientation agreement remained within 0.2 degrees while observed.
- Traced the overlay abort specifically to the 1024×1024, two-layer D16 depth swapchain: its shadow export requests an unsupported depth Android buffer, falls back to BLOB, and fails (`Height and layers must be 1 ... BLOB`). This matches the older unresolved depth finding above. A byte-packed Vulkan staging prototype was built but discarded without deployment: EGL's separate array/texture-view path also needs real depth transport, so changing allocation alone would not provide correct shared depth. The running guest retains the original Vulkan implementation.

### Sustained-performance resource cleanup (2026-10-07)

- User reports initial improvement followed by worsening FPS/jumps, especially Gorilla Tag; sustained performance remains the target.
- Found leaked deferred Vulkan imports when ANGLE accepts external array storage without binding an image. FreeMemory now releases their Android buffers and generation fd.
- Deleting emulated stereo textures now releases shared buffers, per-layer GL textures, array slots and texture metadata. Generation callbacks retain a reference to their mapping until completion, preventing deletion/reuse from leaving a dangling worker pointer.
- Framebuffer attachment metadata is scoped to its EGL context and removed on deletion, preventing stale entries from consuming the fixed table. GLES resolves the current context through the vendor EGL dependency to respect Android linker namespaces.
- Added egl/tests/run_array_lifetime.sh: 160 real guest stereo imports, draws, fence submissions and immediate deletions; GPU readback checks exact red/green eye pixels, and fd/map counts must remain bounded. Previous HEAD binaries fail the descriptor bound after roughly 13 cycles. Fixed binaries pass with 18 descriptors and four executable mappings throughout; the same test passes after cold boot.
- Installed all three fixed libraries into emu_vendor.img, rebuilt the disk, cold booted successfully and verified installed SHA-256 hashes against the builds. Python bridge suite passes all 12 tests. Boot sync explicitly targets the guest so a connected physical Quest does not cause it to skip syncing recent guest writes.
- This establishes a concrete lifetime fix, not sustained native FPS or resolved motion jumps. A five-minute moving-pose stream measurement is in progress; unsupported shared D16 depth remains unresolved.

- Five-minute full-resolution moving-head/controller stream test completed: 14,305 captured frames (47.7 FPS), pose-age median 78.3 ms / p95 100.0 ms; exact timestamps, sequential IDs and IDR recovery passed. Stream stayed near 48–50 FPS later in the run rather than progressively collapsing. Runtime/game fd counts stayed about 688/172. No fatal swapchain errors appeared in this interval. Gorilla Tag's own 216 native FPS samples had median 28 FPS, so the stream result does not establish smooth gameplay.
- Suppressed per-pose TrackingDataInjection debug logging during normal streaming (hundreds of lines/s, dominant in logcat); warnings/errors remain. EMUXR2_PROFILE=1 restores debug tracking logs. Live setting removed those debug entries; this is overhead reduction, not a claimed FPS solution.

- Warm-game encoder-only VideoToolbox recheck: 4,474 captures/90 s (49.7 FPS) but median/p95 pose age 150.4/182.4 ms, much worse than software's 78.3/100.0 ms. Native Gorilla Tag median remained 28 FPS in 53 observed samples. Trial wrapper was only in /tmp; rejected and restored ordinary libx264 headset streaming. This does not solve the game/compositor bottleneck.

### Current-display reprojection and hardware encoding (2026-10-07)

- Separated the physical Quest client's display view from its future network tracking view. Local shader reprojection and submitted projection orientation now use xrLocateViews at the frame's predictedDisplayTime, avoiding a future-view warp followed by headset correction. Tracking prediction/history matching remains unchanged. APK built and installed on the connected Quest 2; wearer confirmation is pending. Source committed locally in the sibling VR4Mac repository as 944eca7 (no remote configured); its exact patch is saved here as bridge/client-display-time.patch for GitHub/reproducibility.
- Confirmed the smoke helper sent Home at 0.5 s, backgrounding Gorilla Tag. Menu testing is now opt-in (--menu), so controller throughput tests retain the game in front.
- Native diagnosis: array export CPU cost was below 1 ms in steady state, while VrApi frame-retirement waits reached 25–100 ms. Extra post-fence flush was tested but did not consistently improve native FPS and was removed; all native binaries were rebuilt/restored to committed originals. No experimental fence flush is persisted.
- macOS hardware encoder now uses NV12, baseline H.264, one reference frame and AV_CODEC_FLAG_LOW_DELAY together. Verified one output packet and immediate decoder frame for each of twelve consecutive input frames with exact PTS (no reordering/backlog). Hardware open failure falls back to software; EMUXR2_ENCODER=software explicitly selects it. Previous hardware trial lacked this complete configuration and had 150 ms latency.
- New 45 s warm-game hardware trial: 2,614 captures (58.1 FPS), median/p95 pose age 77.5/93.1 ms. Five-minute full-resolution hardware test: 16,928 captures (56.4 FPS), median/p95 78.1/95.2 ms, exact pose timestamps/sequential IDs/IDR recovery. Stream held roughly 55–59 FPS. Recent 78 native Gorilla Tag samples had median 43 FPS, versus 28 FPS in the previous software observation. Different scene/phase details mean these are observed improvements, not a strict controlled claim of native 72 FPS.
- All 14 Python tests pass, including hardware immediate output and unavailable-hardware fallback; software encoder regression also passes. Java input tests pass. Sustained native 72 FPS and wearer comfort are not established. Investigating render-buffer headroom separately with full-size video output.

- Render-buffer trial (1280x1408, full-size output) gave 4,176 captures/75 s (55.7 FPS), median/p95 78.6/95.5 ms; no clear throughput improvement. Restored both texture properties to their original empty values; no resolution reduction persisted.
- Found overdue tick replay in hal/vsync.cpp: delayed clock_nanosleep wakeups caused successive old deadlines to be published in a burst. The new phase-preserving schedule skips missed periods, retries interrupted sleeps, and never publishes a future timestamp. A deterministic 10,000-step scheduling test passes. Under-load probe before: median/p95/max vsync age 7.46/13.78/45.41 ms, 2 of 5,000 samples older than two periods. Live new HAL: 7.50/13.75/17.74 ms, zero samples older than two periods. These separate intervals demonstrate the observed clock behavior, not wearer comfort or a strict FPS A/B.
- HAL refresh state is atomic and advertises only the emulator's actual 72 Hz display mode instead of nonexistent 90/120 Hz modes. Unsupported changes return EINVAL. Added priority -8 to the display HAL service to reduce clock scheduling delays under load; applied it live and installed the new executable/config in emu_vendor.img. Native build passes (one existing inherited-interface warning).

### Streamer survives guest reboots; Ape Sprint (2026-10-07)

- A streamer left running across an emulator reboot never restarted its guest helpers (no injector, so no head pose and nothing drawn), and its 10 s watchdog then killed the VR runtime every 40 s, taking down the running app's VR session each time. The watchdog now re-runs start_guest when the injector is gone, and does not restart the runtime while the headset sends no poses (taken off or asleep): an idle headset is not a hung compositor.
- The downloaded Ape Sprint v7 APK lacks Unity's XR natives (libOculusXRPlugin, libOVRPlugin, libopenxr_loader), so it starts as a flat 2D app and Horizon shows only the loading void (same on a real Quest). The complete v6 from the user's own Quest runs in stereo in the emulator: VrApi FPS 20-24/72, Prd ~60 ms, the same GPU/latency bottleneck as Gorilla Tag.

### Pink textures, in-game menu, native-like frame rates (2026-10-07)

- Pink/magenta patches (cosmetics, Ape Sprint forest): the emulator decodes ASTC with its GPU decoder, which writes magenta for some blocks; its CPU decoder has no switch. The GLES shim hides the ASTC extensions, so Unity decompresses ASTC itself at load. bridge/pink_check.py: Ape Sprint lobby wall 5.1% magenta -> 0%.
- Universal menu over games (instead of Android Home backgrounding the app): VrShell's overlay aborted creating its 2-layer D16 depth swapchain (no depth Android buffers to share). Depth is now kept local to each process (the compositor needs colour only; GL depth views get their own storage), and glFramebufferTextureMultisampleMultiviewOVR, missing in ANGLE, falls back to plain multiview. Restarting the runtime orphans the overlay (it never reconnects): the flat display mesh and its property are now baked into the vendor image (patch.sh, vendor.prop) so nothing restarts the runtime at start, and stream.py respawns the overlay after a watchdog restart. The injector's Android-Home fallback is now opt-in (persist.emuxr2.home_fallback=1). bridge/menu_check.py presses the menu inside Ape Sprint: menu with Resume/Quit and dock over the running game.
- Frame rate: a guest atrace showed the game waiting ~31 ms per frame for GPU completion and the host sample showed why: gfxstream kept every displayable buffer as a GL texture too (no GL/Vulkan interop on macOS), so every Vulkan submit/present touching one read it back and re-uploaded it with glTexSubImage2D (CPU tiling). Booting with `-feature GuestUsesAngle,VulkanNativeSwapchain` makes the host compose in Vulkan with Vulkan-only buffers (guestVulkanOnly). init.ranchu.rc now always selects our EGL shim (the feature passes hardwareegl=angle). The emulator's shared-memory recorder needs GL, so the display is captured from its gRPC screenshot stream into a memory-mapped file (stream.GrpcDisplay; 127.0.0.1 only, console-token auth). Boots are headless by default (WINDOW=1 for a window; the window cost ~20%).
- Ape Sprint (VrApi median): 25 fps windowed -> 30 headless -> 41-55 with Vulkan composition (scene dependent), capture 58 fps alongside. Gorilla Tag: 72/72, vsync-locked. Home 72.
- Rejected (measured): AsyncComposeSupport and no GL pipe checksums (no change), VulkanBatchedDescriptorSetUpdate (guest renders black), polling fence waits (worse: raw Vulkan fence p90 0.98 ms waiting vs 7.5 ms polling), ANGLE asyncCommandQueue off (27 vs 30 fps), MoltenVK asynchronous submits (no change), KosmicKrisp ICD (emulator aborts on a Metal texture-view assertion).
- Ape Sprint from Downloads (v7) lacks Unity's XR natives (libOculusXRPlugin, libOVRPlugin, libopenxr_loader) and runs flat; the complete v6 from the user's Quest runs in VR, online (Photon/PlayFab) per the user.
