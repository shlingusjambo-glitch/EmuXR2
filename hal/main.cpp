// macvr-hal: hosts MacVR's stand-ins for the Quest's vendor HALs inside the emulator.
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>
void registerComposer();
void registerPowerstate();
void registerSensors();
void registerDeviceCert();   // only while the firmware has the device certificate HAL (hal/build.sh)
void registerVsync();
int main() {
    ::android::hardware::configureRpcThreadpool(4, true);
    registerComposer();
    registerPowerstate();
    registerSensors();
#if MACVR_DEVICECERT
    registerDeviceCert();
#endif
    registerVsync();
    ::android::hardware::joinRpcThreadpool();
}
