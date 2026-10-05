// vendor.oculus.hardware.sensors_java@1.0 (IPowerstate: the headset mount sensor), laid out like the interface
// library in the user's firmware (vtable.py / hidlsig.py).
#pragma once
#include "oculus_composer.h"   // MACVR_IBASE_OVERRIDES
namespace vendor::oculus::hardware::sensors_java::V1_0 {
using ::android::hardware::Return;
enum class PowerstateEvent : uint32_t { SYSTEM_UP = 0, SYSTEM_DOWN = 1, PROX_ON = 2, PROX_OFF = 3 };
struct IPowerstateCallback : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    virtual Return<void> notifyPowerstateEvent(PowerstateEvent e) = 0;
    MACVR_IBASE_OVERRIDES
};
struct IPowerstate : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    virtual Return<void> registerPowerstateCallback(const ::android::sp<IPowerstateCallback>& cb) = 0;
    virtual Return<void> unregisterPowerstateCallback(const ::android::sp<IPowerstateCallback>& cb) = 0;
    virtual Return<bool> isMounted() = 0;
    virtual Return<bool> setBackoff(uint32_t ms) = 0;
    virtual Return<void> setEnabled(bool on) = 0;
    MACVR_IBASE_OVERRIDES
};
}
