# sourced by patch.sh after the base patches
# EGL: MacVR's shim over ANGLE (GLES over the emulated Vulkan): surfaceless contexts, and it drops the
# context/surface attributes ANGLE rejects (Meta's runtime asks for a few)
"$H/egl/build.sh" emu/emu_vendor.img >/dev/null
for l in libEGL_macvr libGLESv1_CM_macvr libGLESv2_macvr; do put emu/emu_vendor.img "$H/egl/out/$l.so" /lib64/egl/$l.so u:object_r:same_process_hal_file:s0; done
# the API 32 image's ANGLE leaks in Meta's compositor (the runtime grows ~400 MB/s and is killed): use the newer
# ANGLE kept in angle/ (from the emulator's API 33 image) when present
[ -f angle/libGLESv2_angle.so ] && for l in libEGL_angle libGLESv1_CM_angle libGLESv2_angle; do put emu/emu_vendor.img angle/$l.so /lib64/egl/$l.so u:object_r:same_process_hal_file:s0; done
x emu/emu_vendor.img /etc/init/hw/init.ranchu.rc work/init.ranchu.rc
# always our EGL shim: with -feature GuestUsesAngle (Vulkan-backed host color buffers) the emulator passes hardwareegl=angle
sed -i '' 's/setprop ro.hardware.egl ${ro.boot.hardwareegl:-emulation}/setprop ro.hardware.egl macvr/' work/init.ranchu.rc
put emu/emu_vendor.img work/init.ranchu.rc /etc/init/hw/init.ranchu.rc
# MacVR's stand-ins for the Quest's vendor HALs (hardware the emulator doesn't have), plus the HIDL interface
# libraries they implement, from the user's own firmware (tools/extract_fs.sh gathers them into fs/vendor/lib64)
"$H/hal/build.sh" "$W" >/dev/null
put emu/emu_vendor.img "$H/hal/out/macvr-hal" /bin/hw/macvr-hal u:object_r:hal_graphics_composer_default_exec:s0
$D -w -R "set_inode_field /bin/hw/macvr-hal mode 0100755" emu/emu_vendor.img >/dev/null 2>&1
for l in $(cd fs/vendor/lib64 && ls vendor.oculus.*.so); do put emu/emu_vendor.img fs/vendor/lib64/$l /lib64/$l u:object_r:vendor_file:s0; done
put emu/emu_vendor.img "$H/hal/macvr-hal.rc" /etc/init/macvr-hal.rc u:object_r:vendor_configs_file:s0
# (without the device certificate HAL when the firmware no longer has it: hal/build.sh then leaves it out)
python3 -c "import re, sys; m = open(sys.argv[1]).read(); print(m if sys.argv[2] == '1' else re.sub(r'\s*<hal format=\"hidl\">\s*<name>vendor\.oculus\.hardware\.devicecert</name>.*?</hal>', '', m, flags=re.S), end='')" \
    "$H/hal/macvr-hal.xml" "$([ -f fs/vendor/lib64/vendor.oculus.hardware.devicecert@1.0.so ] && echo 1)" > work/macvr-hal.xml
put emu/emu_vendor.img work/macvr-hal.xml /etc/vintf/manifest/macvr-hal.xml u:object_r:vendor_configs_file:s0
# Vulkan: MacVR's HAL wrapper over the emulator driver adds VK_KHR_external_memory_fd (over AHardwareBuffer),
# which Meta's runtime uses to share swapchain images
"$H/vk/build.sh" "$W" >/dev/null
put emu/emu_vendor.img "$H/vk/out/vulkan.macvr.so" /lib64/hw/vulkan.macvr.so u:object_r:same_process_hal_file:s0
sed -i '' 's/setprop ro.hardware.vulkan ranchu/setprop ro.hardware.vulkan macvr/' work/init.ranchu.rc
put emu/emu_vendor.img work/init.ranchu.rc /etc/init/hw/init.ranchu.rc
# the camera mux configuration from the user's own firmware, for the sensor HAL stand-in
mkdir -p work/macvr && python3 "$H/hal/muxconfig.py" fs/vendor/etc/cameramuxmode/configv2.json > work/macvr/muxmode.txt
$D -w -R "mkdir /etc/macvr" emu/emu_vendor.img >/dev/null 2>&1 || true
put emu/emu_vendor.img work/macvr/muxmode.txt /etc/macvr/muxmode.txt u:object_r:vendor_configs_file:s0
# Desktop tracking: scoped identity, DSP startup adapter, CPU sets and stationary pose input.
put work/system.img "$H/hal/out/libdeviceid_macvr.so" /system/lib64/libdeviceid_macvr.so
$D -w -R "mkdir /system/lib64/macvr" work/system.img >/dev/null 2>&1 || true
put work/system.img "$H/hal/out/libhexagon.so" /system/lib64/macvr/libhexagon.so
# (each init file goes back where this firmware keeps it: v54 system, later versions partly system_ext)
rcat() { $D -R "stat /system/etc/init/$1.rc" work/system.img 2>/dev/null | grep -q Inode && echo "work/system.img /system/etc/init" || echo "work/system_ext.img /etc/init"; }
for rc in trackingservice-net mrsystemservice; do set -- $(rcat $rc); x $1 $2/$rc.rc work/$rc.rc; done
python3 "$H/compat/install.py" work
for rc in trackingservice-net mrsystemservice; do set -- $(rcat $rc); put $1 work/$rc.rc $2/$rc.rc; done
put work/system.img work/macvr-cpusets.rc /system/etc/init/macvr-cpusets.rc
put work/system.img "$H/compat/pose.sh" /system/etc/macvr-pose.sh
put work/system.img "$H/compat/pose.rc" /system/etc/init/macvr-pose.rc
