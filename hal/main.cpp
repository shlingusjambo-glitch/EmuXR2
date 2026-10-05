// macvr-hal: hosts MacVR's stand-ins for the Quest's vendor HALs inside the emulator.
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>
void registerComposer();
void registerPowerstate();
void registerSensors();
void registerDeviceCert();
void registerVsync();
int main() {
    ::android::hardware::configureRpcThreadpool(4, true);
    registerComposer();
    registerPowerstate();
    registerSensors();
    registerDeviceCert();
    registerVsync();
    ::android::hardware::joinRpcThreadpool();
}
