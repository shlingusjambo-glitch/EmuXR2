// MacVR stand-in for the headset mount (proximity) sensor: always worn, since the real headset is on the user's
// head whenever MacVR streams to it.
#include "oculus_sensors_java.h"
#include <log/log.h>
#include <thread>
using namespace vendor::oculus::hardware::sensors_java::V1_0;
using ::android::hardware::Return;

struct MacVRPowerstate : public IPowerstate {
    Return<void> registerPowerstateCallback(const ::android::sp<IPowerstateCallback>& cb) override {
        ALOGI("powerstate: callback registered, reporting mounted");
        ::android::sp<IPowerstateCallback> c = cb;
        std::thread([c] { usleep(200000); c->notifyPowerstateEvent(PowerstateEvent::SYSTEM_UP).isOk(); c->notifyPowerstateEvent(PowerstateEvent::PROX_ON).isOk(); }).detach();
        return {};
    }
    Return<void> unregisterPowerstateCallback(const ::android::sp<IPowerstateCallback>&) override { return {}; }
    Return<bool> isMounted() override { return true; }
    Return<bool> setBackoff(uint32_t) override { return true; }
    Return<void> setEnabled(bool) override { return {}; }
};

void registerPowerstate() {
    static ::android::sp<MacVRPowerstate> p = new MacVRPowerstate;
    ALOGI("powerstate: %d", p->registerAsService());
}
