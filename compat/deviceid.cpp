// The emulator kernel must remain ranchu. Meta's device library interprets
// ranchu as Sekiu, which this Quest 2 tracking engine does not support.
#include <string>
namespace OVR { namespace OS {
std::string GetDeviceType() { return "Hollywood"; }
} }
