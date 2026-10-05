// Interface declarations for vendor.oculus.hardware.graphics.composer@1.0/1.1, matching the generated code in the
// interface libraries of the user's firmware (method order from BpHwComposer's vtable, types from BnHwComposer's
// parcel reads/writes; see vtable.py and hidlsig.py). The libraries themselves come from the user's
// firmware at setup time and are never shipped.
#pragma once
#include <android/hidl/base/1.0/IBase.h>
#include <hidl/HidlSupport.h>

#define MACVR_IBASE_OVERRIDES \
    ::android::hardware::Return<void> interfaceChain(interfaceChain_cb) override; \
    ::android::hardware::Return<void> debug(const ::android::hardware::hidl_handle&, const ::android::hardware::hidl_vec<::android::hardware::hidl_string>&) override; \
    ::android::hardware::Return<void> interfaceDescriptor(interfaceDescriptor_cb) override; \
    ::android::hardware::Return<void> getHashChain(getHashChain_cb) override; \
    ::android::hardware::Return<void> setHALInstrumentation() override; \
    ::android::hardware::Return<bool> linkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>&, uint64_t) override; \
    ::android::hardware::Return<void> ping() override; \
    ::android::hardware::Return<void> getDebugInfo(getDebugInfo_cb) override; \
    ::android::hardware::Return<void> notifySyspropsChanged() override; \
    ::android::hardware::Return<bool> unlinkToDeath(const ::android::sp<::android::hardware::hidl_death_recipient>&) override; \
    __attribute__((warn_unused_result)) ::android::status_t registerAsService(const std::string& serviceName = "default");

namespace vendor::oculus::hardware::graphics::composer {
using ::android::hardware::Return;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_array;
namespace V1_0 {
struct IComposer : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    virtual Return<bool> isVariableRefreshRateSupported() = 0;
    virtual Return<int32_t> setVariableRefreshRateEnabled(bool enabled) = 0;
    virtual Return<int32_t> triggerVsync(int32_t display) = 0;
    MACVR_IBASE_OVERRIDES
};
}
namespace V1_1 {
enum class Device : uint8_t {};
struct BacklightMatrix;
struct SpiMessage;
struct IComposer : public V1_0::IComposer {
    static const char* descriptor;
    using getRefreshRates_cb = std::function<void(const hidl_vec<int32_t>&)>;
    using getBacklightMatrix_cb = std::function<void(const hidl_array<uint8_t, 1153>&)>;
    using getBroMatrix_cb = std::function<void(const hidl_array<uint8_t, 581>&)>;
    virtual Return<bool> isDynamicRefreshRateSupported() = 0;
    virtual Return<int32_t> getDefaultRefreshRate() = 0;
    virtual Return<int32_t> getActiveRefreshRate() = 0;
    virtual Return<void> getRefreshRates(getRefreshRates_cb cb) = 0;
    virtual Return<int32_t> setRefreshRate(int32_t hz) = 0;
    virtual Return<int32_t> triggerWritebackCac() = 0;
    virtual Return<int32_t> allowTidMaximumPriorityContext(int32_t tid) = 0;
    virtual Return<int32_t> allowUidHighPriorityContext(int32_t uid) = 0;
    virtual Return<int32_t> sendBacklightMatrix(const BacklightMatrix& m, Device d) = 0;
    virtual Return<int64_t> sendBluDebugSpiMessage(const SpiMessage& m, Device d) = 0;
    virtual Return<void> getBacklightMatrix(Device d, getBacklightMatrix_cb cb) = 0;
    virtual Return<void> getBroMatrix(Device d, getBroMatrix_cb cb) = 0;
    MACVR_IBASE_OVERRIDES
};
}
}
