// vendor.oculus.hardware.sensors@1.0 root interfaces, laid out like the interface library in the user's firmware:
// method order from each BpHw<X> vtable (vtable.py), argument and return types from the BnHw<X> stubs' parcel
// reads/writes (hidlsig.py). Structs the stand-ins never look inside are left opaque.
#pragma once
#include "oculus_composer.h"   // MACVR_IBASE_OVERRIDES
#include <fmq/MQDescriptorBase.h>
#include <hidl/MQDescriptor.h>

namespace vendor::oculus::hardware::sensors::V1_0 {
using ::android::sp;
using ::android::hardware::Return;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_handle;
template <typename T> using MQ = ::android::hardware::MQDescriptor<T, ::android::hardware::kSynchronizedReadWrite>;

enum class Result : uint32_t { OK = 0 };
enum class FrameType : uint32_t {};
enum class StreamCommand : uint32_t {};
enum class UtilityFrequency : uint32_t {};
enum class ZapMessageClass : uint8_t {};
enum class TimeDomain : uint8_t {};

// opaque (only ever passed through by reference)
struct ChannelSettings; struct FrameRateSettings; struct MotionStreamProperties;
// A stream's notification setup (libvrsensors-hidlwrapper StreamHandle::prepareStream, libeventflaghelper): the event
// flag is a 4-byte ashmem shared by a client's composite stream; the reader wakes the first bit after reading and
// waits for the second, which the writer wakes after writing
struct FmqConfig { hidl_handle eventFlag; uint32_t readNotification, writeNotification; };
static_assert(sizeof(FmqConfig) == 24, "FmqConfig is 24 bytes");
struct ControllerWirelessFreqBlocklist; struct SecureHostInfo; struct SecureDeviceInfo; struct MotionSensorProperties;
struct IadProperties; struct ControllerAddr;
struct ImuData; struct MagData; struct IadData; struct ExternalTimestampData; struct ZapMessage; struct CurlData;
struct ControllerImuData; struct ButtonData; struct PrecisionPadData; struct MultiTouchData; struct StylusData;
struct WirelessDeviceStats; struct PoseInput; struct ControllerInputADCData; struct ControllerCollisionEvent;
// element types of vectors handed back empty (complete only so hidl_vec<> compiles; never serialized)
struct CameraProperties { uint32_t id; hidl_string a, b; };   // 40 bytes (libvrsensors-hidlwrapper reads id@0, strings@8,@24)
// 448 bytes, plain data (the interface library reads count * 0x1c0; Meta's OVR::Sensors type has the same layout).
// Fields from libcmsservice-headset (ControllerGlue::assignSlot / initializeRemoteLocked, RemoteDevice::processStateChange):
//   @0x08 u64 address, @0x10 u8 connected, @0x12 u8 (and-ed with connected), @0x14 float battery 0-100,
//   @0x18 char[16] serial, @0x28 char[] firmware version, @0xa8 char[] (a further string),
//   @0x128 u64 flags: 0x40 controller, 0x20 left / 0x10 right (one slot each), 0x200 Constellation (Touch), 0x400 self-tracked (Pro); trackingservice aborts without one
struct PairedControllerInfo {
    uint8_t b[0x1c0];
    uint64_t& addr() { return *reinterpret_cast<uint64_t*>(b + 0x08); }
    uint8_t& connected() { return b[0x10]; }
    float& battery() { return *reinterpret_cast<float*>(b + 0x14); }
    char* serial() { return reinterpret_cast<char*>(b + 0x18); }
    char* firmware() { return reinterpret_cast<char*>(b + 0x28); }
    uint64_t& flags() { return *reinterpret_cast<uint64_t*>(b + 0x128); }
};
static_assert(sizeof(PairedControllerInfo) == 0x1c0, "PairedControllerInfo is 448 bytes");

#define MACVR_IFACE(X) struct X : public ::android::hidl::base::V1_0::IBase { static const char* descriptor; MACVR_IBASE_OVERRIDES };
MACVR_IFACE(ISensorClient)
MACVR_IFACE(IZapperCallback)
MACVR_IFACE(IFrameConfigCallback)

enum class CameraSyncMode : uint32_t {};
enum class ControllerType : uint32_t {};
struct FrameSet; struct CameraStreamMetadata; struct CameraStreamConfiguration; struct MuxState; struct ExposureGainSettings;
struct Resolution { uint8_t opaque[16]; };
// libvrsensors-hidlwrapper's conversion: a 16-bit status, then the factory calibration document
struct ControllerCalibrationData { uint16_t status; hidl_string json; };
static_assert(sizeof(ControllerCalibrationData) == 24, "ControllerCalibrationData is 24 bytes");
struct ControllerLedConfig; struct ThumbstickADCRange; struct SimpleHapticIntensity { uint8_t v; };
struct AdvertisingControllerInfo { uint8_t opaque[64]; };

struct ICameraStream : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    using getMetadata_cb = std::function<void(Result, const CameraStreamMetadata&)>;
    using prepareStream_cb = std::function<void(Result, const MQ<FrameSet>&, const FmqConfig&)>;
    using getConfiguration_cb = std::function<void(Result, const CameraStreamConfiguration&)>;
    using setConfigUpdateHandling_cb = std::function<void(Result, const MuxState&)>;
    virtual Return<void> getMetadata(getMetadata_cb cb) = 0;
    virtual Return<Result> writeSessionOcalData(const hidl_vec<uint8_t>& d) = 0;
    virtual Return<void> prepareStream(const sp<ISensorClient>& c, const MQ<FrameSet>& q, const FmqConfig& f, prepareStream_cb cb) = 0;
    virtual Return<void> getConfiguration(const sp<ISensorClient>& c, getConfiguration_cb cb) = 0;
    virtual Return<void> setConfigUpdateHandling(const sp<ISensorClient>& c, const sp<IFrameConfigCallback>& f, setConfigUpdateHandling_cb cb) = 0;
    virtual Return<Result> streamControl(const sp<ISensorClient>& c, StreamCommand cmd) = 0;
    virtual Return<Result> startOverridingExposureSettings(const sp<ISensorClient>& c) = 0;
    virtual Return<Result> stopOverridingExposureSettings(const sp<ISensorClient>& c) = 0;
    virtual Return<void> setExposureGain(const sp<ISensorClient>& c, const ExposureGainSettings& s) = 0;
    virtual Return<void> setPhaseOffset(uint64_t ns) = 0;
    virtual Return<uint32_t> getFrameRate() = 0;
    virtual Return<Result> setCameraSyncMode(CameraSyncMode m) = 0;
    virtual Return<Result> setResolution(const hidl_vec<Resolution>& r) = 0;
    MACVR_IBASE_OVERRIDES
};
struct ICameraStreamController : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    virtual Return<Result> startOverridingExposureSettings() = 0;
    virtual Return<Result> stopOverridingExposureSettings() = 0;
    virtual Return<void> setExposureGain(uint8_t cam, uint32_t frame, const ExposureGainSettings& s) = 0;
    MACVR_IBASE_OVERRIDES
};
MACVR_IFACE(ICameraStreamControlTarget)
struct IControllerStreamingClient : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    using getCalibrationData_cb = std::function<void(const ControllerCalibrationData&)>;
    using getLedConfig_cb = std::function<void(const ControllerLedConfig&, bool)>;
    using getThumbstickMaxADCRange_cb = std::function<void(const ThumbstickADCRange&, bool)>;
    using getThumbstickDeadzone_cb = std::function<void(float, bool)>;
    virtual Return<void> dispose() = 0;
    virtual Return<void> getCalibrationData(const ControllerAddr& a, getCalibrationData_cb cb) = 0;
    virtual Return<void> enable(const ControllerAddr& a) = 0;
    virtual Return<void> disable(const ControllerAddr& a) = 0;
    virtual Return<bool> controlInputADCStreaming(const ControllerAddr& a, bool on) = 0;
    virtual Return<bool> setLedConfig(const ControllerLedConfig& c) = 0;
    virtual Return<void> getLedConfig(const ControllerAddr& a, getLedConfig_cb cb) = 0;
    virtual Return<bool> setTransmitPowerBoost(int8_t b) = 0;
    virtual Return<bool> setSimpleHaptics(const ControllerAddr& a, uint8_t v) = 0;
    virtual Return<bool> setMultiSimpleHaptics(const ControllerAddr& a, const ::android::hardware::hidl_array<SimpleHapticIntensity, 6>& v) = 0;
    virtual Return<bool> setBufferedHaptics(const ControllerAddr& a, double t, uint8_t k, bool loop, const hidl_vec<uint8_t>& d) = 0;
    virtual Return<bool> appendBufferedHaptics(const ControllerAddr& a, uint8_t k, const hidl_vec<uint8_t>& d) = 0;
    virtual Return<bool> appendPCMHaptics(const ControllerAddr& a, const hidl_vec<int8_t>& d) = 0;
    virtual Return<bool> setThumbstickMaxADCRange(const ControllerAddr& a, const ThumbstickADCRange& r) = 0;
    virtual Return<void> getThumbstickMaxADCRange(const ControllerAddr& a, getThumbstickMaxADCRange_cb cb) = 0;
    virtual Return<bool> resetThumbstickMaxADCRange(const ControllerAddr& a) = 0;
    virtual Return<bool> setThumbstickDeadzone(const ControllerAddr& a, float d) = 0;
    virtual Return<void> getThumbstickDeadzone(const ControllerAddr& a, getThumbstickDeadzone_cb cb) = 0;
    virtual Return<bool> resetThumbstickDeadzone(const ControllerAddr& a) = 0;
    virtual Return<bool> sleepController(const ControllerAddr& a) = 0;
    virtual Return<bool> wakeController(const ControllerAddr& a) = 0;
    MACVR_IBASE_OVERRIDES
};
struct IControllerManagementClient : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    struct FWUpdateCallbackConfig;
    using getAdvertisingControllers_cb = std::function<void(const hidl_vec<AdvertisingControllerInfo>&)>;
    virtual Return<void> dispose() = 0;
    virtual Return<void> getAdvertisingControllers(getAdvertisingControllers_cb cb) = 0;
    virtual Return<Result> enterDMM(const ControllerAddr& a) = 0;
    virtual Return<Result> exitDMM(const ControllerAddr& a) = 0;
    virtual Return<Result> pairController(const ControllerAddr& a) = 0;
    virtual Return<Result> unpairController(const ControllerAddr& a) = 0;
    virtual Return<Result> updateFirmware(const ControllerAddr& a, ControllerType t, const FWUpdateCallbackConfig& c) = 0;
    MACVR_IBASE_OVERRIDES
};

struct ICameraProvider : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    using getProperties_cb = std::function<void(Result, const hidl_vec<CameraProperties>&)>;
    using getCalibrationData_cb = std::function<void(Result, const hidl_handle&)>;
    using getChannels_cb = std::function<void(const ChannelSettings&)>;
    virtual Return<void> getProperties(getProperties_cb cb) = 0;
    virtual Return<void> getCalibrationData(uint32_t id, getCalibrationData_cb cb) = 0;
    virtual Return<Result> applyMuxMode(const hidl_string& mode, uint8_t flags) = 0;
    virtual Return<sp<ICameraStream>> getStream(FrameType t) = 0;
    virtual Return<sp<ICameraStream>> getStreamByPurpose(const hidl_string& a, const hidl_string& b) = 0;
    virtual Return<sp<ICameraStreamController>> getStreamController(const hidl_string& a, const hidl_string& b) = 0;
    virtual Return<Result> controlStreams(const hidl_vec<sp<ICameraStreamControlTarget>>& t, StreamCommand c) = 0;
    virtual Return<UtilityFrequency> getUtilityFrequency() = 0;
    virtual Return<Result> setUtilityFrequency(UtilityFrequency f) = 0;
    virtual Return<void> getChannels(getChannels_cb cb) = 0;
    virtual Return<Result> setChannels(const ChannelSettings& s) = 0;
    virtual Return<Result> setFrameRate(const FrameRateSettings& s) = 0;
    virtual Return<bool> getRawImageMode() = 0;
    MACVR_IBASE_OVERRIDES
};

#define MACVR_STREAM(name, T) virtual Return<Result> name(const MQ<T>& q, const sp<ISensorClient>& c, const FmqConfig& f) = 0;
struct IControllerProvider : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    using getPairedControllers_cb = std::function<void(const hidl_vec<PairedControllerInfo>&)>;
    using getWirelessFreqBlocklist_cb = std::function<void(const ControllerWirelessFreqBlocklist&, bool)>;
    using getHostInfo_cb = std::function<void(const SecureHostInfo&)>;
    virtual Return<void> getPairedControllers(getPairedControllers_cb cb) = 0;
    virtual Return<bool> setWirelessFreqBlocklist(const ControllerWirelessFreqBlocklist& b) = 0;
    virtual Return<void> getWirelessFreqBlocklist(getWirelessFreqBlocklist_cb cb) = 0;
    virtual Return<bool> clearWirelessFreqBlocklist() = 0;
    virtual Return<void> getHostInfo(getHostInfo_cb cb) = 0;
    virtual Return<bool> allowDevice(const SecureDeviceInfo& d) = 0;
    virtual Return<sp<IControllerManagementClient>> getManagementClient() = 0;
    virtual Return<sp<IControllerStreamingClient>> getStreamingClient() = 0;
    virtual Return<Result> prepareStateStream(const MQ<PairedControllerInfo>& q, const sp<ISensorClient>& c, const FmqConfig& f) = 0;
    MACVR_STREAM(prepareCurlStream, CurlData)
    MACVR_STREAM(prepareImuStream, ControllerImuData)
    MACVR_STREAM(prepareInputStream, ButtonData)
    MACVR_STREAM(preparePrecisionPadStream, PrecisionPadData)
    MACVR_STREAM(prepareMultiTouchStream, MultiTouchData)
    MACVR_STREAM(prepareStylusStream, StylusData)
    MACVR_STREAM(prepareStatsStream, WirelessDeviceStats)
    MACVR_STREAM(preparePoseStream, PoseInput)
    MACVR_STREAM(prepareInputADCStream, ControllerInputADCData)
    MACVR_STREAM(prepareCollisionEventStream, ControllerCollisionEvent)
    MACVR_IBASE_OVERRIDES
};

#define MACVR_MOTION(X, Data) \
struct X : public ::android::hidl::base::V1_0::IBase { \
    static const char* descriptor; \
    using getProperties_cb = std::function<void(Result, const MotionSensorProperties&)>; \
    virtual Return<void> getProperties(getProperties_cb cb) = 0; \
    virtual Return<Result> prepareStream(const MQ<Data>& q, const sp<ISensorClient>& c, const FmqConfig& f) = 0; \
    virtual Return<Result> streamControl(const sp<ISensorClient>& c, StreamCommand cmd) = 0; \
    virtual Return<Result> configureStream(const MotionStreamProperties& p) = 0; \
    MACVR_IBASE_OVERRIDES };
MACVR_MOTION(IImu, ImuData)
MACVR_MOTION(IMag, MagData)

struct IIad : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    using getProperties_cb = std::function<void(const IadProperties&)>;
    virtual Return<void> getProperties(getProperties_cb cb) = 0;
    virtual Return<Result> prepareStream(const MQ<IadData>& q, const sp<ISensorClient>& c, const FmqConfig& f) = 0;
    virtual Return<Result> streamControl(const sp<ISensorClient>& c, StreamCommand cmd) = 0;
    MACVR_IBASE_OVERRIDES
};

struct IExternalTimingProvider : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    virtual Return<Result> prepareExternalTimestampDataStream(const MQ<ExternalTimestampData>& q, const sp<ISensorClient>& c, const FmqConfig& f) = 0;
    virtual Return<Result> externalTimestampDataStreamControl(const sp<ISensorClient>& c, StreamCommand cmd) = 0;
    MACVR_IBASE_OVERRIDES
};

struct IZapper : public ::android::hidl::base::V1_0::IBase {
    static const char* descriptor;
    virtual Return<Result> prepareStream(const MQ<ZapMessage>& q, const sp<ISensorClient>& c, ZapMessageClass k) = 0;
    virtual Return<Result> streamControl(const sp<ISensorClient>& c, ZapMessageClass k, StreamCommand cmd) = 0;
    virtual Return<Result> registerPulsaRpcClient(uint8_t id, const sp<IZapperCallback>& cb) = 0;
    virtual Return<Result> unregisterPulsaRpcClient(uint8_t id) = 0;
    virtual Return<Result> sendPulsaRpcMessage(const sp<IZapperCallback>& cb, const ControllerAddr& a, uint32_t k, const hidl_vec<uint8_t>& msg) = 0;
    virtual Return<int64_t> getTimeTranslation(int64_t t, TimeDomain from, TimeDomain to) = 0;
    MACVR_IBASE_OVERRIDES
};
}
