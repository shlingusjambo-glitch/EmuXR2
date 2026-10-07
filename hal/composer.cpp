// MacVR stand-in for the Quest's display-controller HAL (vendor.oculus.hardware.graphics.composer@1.1).
// The emulator has no Quest panel, backlight or CAC writeback block: refresh-rate queries report the
// MacVR stream rate, everything else succeeds as a no-op.
#include "oculus_composer.h"
#include <log/log.h>
#include <atomic>
using namespace vendor::oculus::hardware::graphics::composer::V1_1;
using ::android::hardware::Return;

static std::atomic<int32_t> gRate{72};
int32_t composerRate() { return gRate.load(); }
struct MacVRComposer : public IComposer {
    Return<bool> isVariableRefreshRateSupported() override { return false; }
    Return<int32_t> setVariableRefreshRateEnabled(bool) override { return 0; }
    Return<int32_t> triggerVsync(int32_t) override { return 0; }
    Return<bool> isDynamicRefreshRateSupported() override { return false; }
    Return<int32_t> getDefaultRefreshRate() override { return 72; }
    Return<int32_t> getActiveRefreshRate() override { return gRate.load(); }
    Return<void> getRefreshRates(getRefreshRates_cb cb) override { cb(::android::hardware::hidl_vec<int32_t>{72}); return {}; }
    Return<int32_t> setRefreshRate(int32_t hz) override { if (hz != 72) return -22; gRate.store(hz); return 0; }
    Return<int32_t> triggerWritebackCac() override { return 0; }
    Return<int32_t> allowTidMaximumPriorityContext(int32_t) override { return 0; }
    Return<int32_t> allowUidHighPriorityContext(int32_t) override { return 0; }
    Return<int32_t> sendBacklightMatrix(const BacklightMatrix&, Device) override { return 0; }
    Return<int64_t> sendBluDebugSpiMessage(const SpiMessage&, Device) override { return 0; }
    Return<void> getBacklightMatrix(Device, getBacklightMatrix_cb cb) override { cb({}); return {}; }
    Return<void> getBroMatrix(Device, getBroMatrix_cb cb) override { cb({}); return {}; }
};

void registerComposer() {
    static ::android::sp<MacVRComposer> c = new MacVRComposer;
    ALOGI("composer: %d", c->registerAsService());
}
