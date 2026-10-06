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
