// macvr-hal: hosts MacVR's stand-ins for the Quest's vendor HALs inside the emulator.
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>
void registerComposer();
int main() {
    ::android::hardware::configureRpcThreadpool(4, true);
    registerComposer();
    ::android::hardware::joinRpcThreadpool();
}
