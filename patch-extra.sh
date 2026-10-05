# sourced by patch.sh after the base patches
# EGL: MacVR's shim over ANGLE (GLES over the emulated Vulkan): surfaceless contexts, and it drops the
# context/surface attributes ANGLE rejects (Meta's runtime asks for a few)
"$H/egl/build.sh" emu/emu_vendor.img >/dev/null
for l in libEGL_macvr libGLESv1_CM_macvr libGLESv2_macvr; do put emu/emu_vendor.img "$H/egl/out/$l.so" /lib64/egl/$l.so u:object_r:same_process_hal_file:s0; done
x emu/emu_vendor.img /etc/init/hw/init.ranchu.rc work/init.ranchu.rc
sed -i '' 's/setprop ro.hardware.egl ${ro.boot.hardwareegl:-emulation}/setprop ro.hardware.egl ${ro.boot.hardwareegl:-macvr}/' work/init.ranchu.rc
put emu/emu_vendor.img work/init.ranchu.rc /etc/init/hw/init.ranchu.rc
# MacVR's stand-ins for the Quest's vendor HALs (hardware the emulator doesn't have), plus the HIDL interface
# libraries they implement, copied from the user's own firmware's vendor partition
"$H/hal/build.sh" "$W" >/dev/null
put emu/emu_vendor.img "$H/hal/out/macvr-hal" /bin/hw/macvr-hal u:object_r:hal_graphics_composer_default_exec:s0
$D -w -R "set_inode_field /bin/hw/macvr-hal mode 0100755" emu/emu_vendor.img >/dev/null 2>&1
for l in $(cd fs/vendor/lib64 && ls vendor.oculus.*.so); do put emu/emu_vendor.img fs/vendor/lib64/$l /lib64/$l u:object_r:vendor_file:s0; done
put emu/emu_vendor.img "$H/hal/macvr-hal.rc" /etc/init/macvr-hal.rc u:object_r:vendor_configs_file:s0
put emu/emu_vendor.img "$H/hal/macvr-hal.xml" /etc/vintf/manifest/macvr-hal.xml u:object_r:vendor_configs_file:s0
# Vulkan: MacVR's HAL wrapper over the emulator driver adds VK_KHR_external_memory_fd (over AHardwareBuffer),
# which Meta's runtime uses to share swapchain images
"$H/vk/build.sh" "$W" >/dev/null
put emu/emu_vendor.img "$H/vk/out/vulkan.macvr.so" /lib64/hw/vulkan.macvr.so u:object_r:same_process_hal_file:s0
sed -i '' 's/setprop ro.hardware.vulkan ranchu/setprop ro.hardware.vulkan macvr/' work/init.ranchu.rc
put emu/emu_vendor.img work/init.ranchu.rc /etc/init/hw/init.ranchu.rc
