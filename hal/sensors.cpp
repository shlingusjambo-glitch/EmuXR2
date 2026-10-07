// EmuXR2 stand-ins for the Quest's sensor hub HAL (vendor.oculus.hardware.sensors@1.0): the emulator has no
// tracking cameras, IMU, magnetometer, IAD sensor or controller radio. Every provider exists and answers, but
// reports no devices and accepts every stream without ever producing data. Head and controller tracking come in
// through Meta's own TrackingDataInjection service instead (see the bridge), so nothing here fakes sensor data.
#include "oculus_sensors.h"
#include <hwbinder/IPCThreadState.h>
#include <log/log.h>
#include <array>
#include <cmath>
#include <cutils/ashmem.h>
#include <cutils/native_handle.h>
#include <sys/mman.h>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <unistd.h>
#include <cutils/properties.h>
#include <fmq/EventFlag.h>
#include <fmq/MessageQueue.h>
#include "touch_calibration.h"
using ::android::hardware::EventFlag;
using ::android::hardware::MessageQueue;
using ::android::hardware::kSynchronizedReadWrite;
using namespace vendor::oculus::hardware::sensors::V1_0;
using ::android::hardware::Return;

// a zeroed struct of a type we don't define, valid to serialize: empty hidl_strings at its string members
// (offsets from structinfo.py over the user's interface library)
template <size_t... Str> struct Zero {
    alignas(16) unsigned char b[4096] = {};
    Zero() { (new (b + Str) hidl_string(), ...); }
    template <typename T> const T& as() { return *reinterpret_cast<const T*>(b); }
};

// ---- camera mux state, built from the firmware's own cameramuxmode config (muxmode.txt, made by muxconfig.py) ----
// Layouts from the interface library's writeEmbeddedToParcel and libsensorclientutils' readers:
//   CameraConfiguration (112 B): cameraId@0 width@4 height@8 stride@0xc pixelFormat@0x10 ... flag@0x28
//                                exposure min/max@0x30/0x38, gain min/max@0x48/0x50, doubles to 0x70
//   MuxState: @0 byte, @8 frame rate (Hz, double), MuxMode@0x10, hidl_vec<SlotState>@0x60
//   MuxMode (0x50): name, configuration, hidl_vec<hidl_string> illuminations, purposes, controlPurposes
struct CamCfg {
    uint32_t cameraId, width, height, stride; uint16_t pixelFormat; uint8_t pad[0x16]; uint8_t flag; uint8_t pad2[7];
    double expMin, expMax, d40, gainMin, gainMax, d58, d60, d68;
};
static_assert(sizeof(CamCfg) == 112, "CameraConfiguration is 112 bytes");
struct PurposeT { hidl_string name; hidl_vec<uint32_t> slots; };
struct MuxModeT { hidl_string name, configuration; hidl_vec<hidl_string> illuminations; hidl_vec<PurposeT> purposes, controls; };
struct SlotT { hidl_vec<CamCfg> cams; };
struct MuxStateT { uint8_t b0; uint8_t pad[7]; double rateHz; MuxModeT mode; hidl_vec<SlotT> slots; };
static_assert(sizeof(MuxModeT) == 0x50 && offsetof(MuxStateT, slots) == 0x60, "MuxState layout");

// the OV7251 tracking cameras: 640x480, 8-bit
static CamCfg camCfg(uint32_t id) {
    CamCfg c{}; c.cameraId = id; c.width = 640; c.height = 480; c.stride = 640; c.pixelFormat = 1;   // visiontypes::PixelFormat::MONO_8
    c.expMin = 0.00001; c.expMax = 0.015; c.gainMin = 1.0; c.gainMax = 16.0;
    return c;
}
static MuxStateT& muxState() {
    static MuxStateT s; static bool done;
    if (done) return s;
    done = true; s.b0 = 1; s.rateHz = 30.0;
    std::vector<SlotT> slots; std::vector<hidl_string> illum; std::vector<PurposeT> purposes, controls;
    FILE* f = fopen("/vendor/etc/macvr/muxmode.txt", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        std::vector<std::string> w; char* save = nullptr;
        for (char* t = strtok_r(line, " \n", &save); t; t = strtok_r(nullptr, " \n", &save)) w.push_back(t);
        if (w.empty()) continue;
        auto nums = [&](size_t from) { std::vector<uint32_t> v; for (size_t i = from; i < w.size(); i++) v.push_back(atoi(w[i].c_str())); return v; };
        if (w[0] == "slot") { SlotT sl; std::vector<CamCfg> cams; for (uint32_t id : nums(1)) cams.push_back(camCfg(id)); sl.cams = cams; slots.push_back(sl); }
        else if (w[0] == "mode" && w.size() > 2) { s.mode.name = w[1]; s.mode.configuration = w[2]; }
        else if (w[0] == "illum") for (size_t i = 1; i < w.size(); i++) illum.push_back(w[i]);
        else if ((w[0] == "purpose" || w[0] == "control") && w.size() > 1) { PurposeT p; p.name = w[1]; p.slots = nums(2); (w[0] == "purpose" ? purposes : controls).push_back(p); }
    }
    if (f) fclose(f); else ALOGW("sensors: no muxmode.txt");
    s.mode.illuminations = illum; s.mode.purposes = purposes; s.mode.controls = controls; s.slots = slots;
    return s;
}

// ---- calibration ----
// There are no camera images here, so tracking never uses these numbers; the tracking engine only needs valid
// documents (schema from its parser: tools strorder.py / strrefs.py): the OV7251 sensors (640x480 fisheye), at
// Quest 2-like corner positions, and an IMU at the device origin with identity rectification and zero bias.
static std::string mat4(double yawDeg, double pitchDeg, double x, double y, double z) {
    double a = yawDeg * M_PI / 180, b = pitchDeg * M_PI / 180;
    double r[3][3] = {{cos(a), sin(a) * sin(b), sin(a) * cos(b)}, {0, cos(b), -sin(b)}, {-sin(a), cos(a) * sin(b), cos(a) * cos(b)}};
    char s[512];
    snprintf(s, sizeof s, "[%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.4f,%.6f,%.6f,%.6f,%.4f,0,0,0,1]",
             r[0][0], r[0][1], r[0][2], x, r[1][0], r[1][1], r[1][2], y, r[2][0], r[2][1], r[2][2], z);
    return s;
}
static std::string cameraJson(uint32_t id) {
    static const double pose[4][5] = {{10, 10, -0.07, 0.03, -0.01}, {-10, 10, 0.07, 0.03, -0.01}, {10, -10, -0.07, -0.03, -0.01}, {-10, -10, 0.07, -0.03, -0.01}};
    const double* p = pose[id % 4];
    char s[2048];
    snprintf(s, sizeof s, "{\"FileFormat\":{\"Version\":\"0\",\"Timestamp\":\"2022-07-01T09:08:03\",\"UnixTime\":1656666483},\"Device\":{\"SerialNumber\":\"0\",\"DeviceType\":\"Hollywood\",\"BuildType\":\"DVT_A\",\"BuildSubType\":\"\"},\"Metadata\":{\"AlgorithmVersion\":0,\"Source\":\"Factory\",\"Tags\":[],\"NamedTags\":{}},\"CameraCalibration\":[{\"Id\":\"%u\",\"SensorType\":\"OV7251\",\"ImageSize\":[640,480],"
        "\"Projection\":{\"Model\":\"Pinhole\",\"Coefficients\":[275.0,275.0,320.0,240.0]},"
        "\"Distortion\":{\"Model\":\"KannalaBrandtK3\",\"Coefficients\":[0.03,-0.01,0.0,0.0]},"
        "\"DeviceFromCamera\":%s,\"Shutter\":{\"Type\":\"Global\"},\"HorizontalFlip\":false,\"VerticalFlip\":false}],"
        "\"Id\":\"%u\",\"SensorType\":\"OV7251\"}", id, mat4(p[0], p[1], p[2], p[3], p[4]).c_str(), id);
    return s;
}
static std::string imuJson() {
    // The firmware's calibration reader requires the document envelope and the
    // legacy "Linear" model names (the C++ model type names are not JSON names).
    return R"json({"FileFormat":{"Version":"0","Timestamp":"2022-07-01T09:08:03","UnixTime":1656666483},"Device":{"SerialNumber":"0","DeviceType":"Hollywood","BuildType":"DVT_A","BuildSubType":""},"Metadata":{"AlgorithmVersion":0,"Source":"Factory","Tags":[],"NamedTags":{}},"ImuCalibration":{"Id":"imu0","SensorType":"ICM42686","DeviceFromImu":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"Accelerometer":{"Model":"Linear","RectificationMatrix":[1,0,0,0,1,0,0,0,1],"Offset":{"Model":"Constant","ConstantOffset":[0,0,0]},"DtAccelRef":0},"Gyroscope":{"Model":"Linear","RectificationMatrix":[1,0,0,0,1,0,0,0,1],"Offset":{"Model":"Constant","ConstantOffset":[0,0,0]},"DtGyroRef":0}}})json";
}

// Touch controller factory calibration in Constellation's format. ponytail: a debug override file (needs permissive
// SELinux) lets the document be tuned without a rebuild.
static std::string touchJson() {
    if (FILE* f = fopen("/data/local/tmp/emuxr2-touch.json", "r")) {
        std::string s; char b[4096]; size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        fclose(f);
        return s;
    }
    return TOUCH_JSON;
}

static int ashmemWith(const std::string& s) {
    int fd = ashmem_create_region("macvr-calibration", s.size() + 1);
    if (fd < 0) return -1;
    void* m = mmap(nullptr, s.size() + 1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m != MAP_FAILED) { memcpy(m, s.c_str(), s.size() + 1); munmap(m, s.size() + 1); }
    ashmem_set_prot_region(fd, PROT_READ);
    return fd;
}

struct Stream : ICameraStream {
    uint32_t type;
    explicit Stream(uint32_t t) : type(t) {}
    Return<void> getMetadata(getMetadata_cb cb) override { ALOGI("sensors: stream %u %s", type, "getMetadata"); Zero<> z; cb(Result::OK, z.as<CameraStreamMetadata>()); return {}; }
    Return<Result> writeSessionOcalData(const hidl_vec<uint8_t>&) override { ALOGI("sensors: stream %u %s", type, "writeSessionOcalData"); return Result::OK; }
    Return<void> prepareStream(const sp<ISensorClient>&, const MQ<FrameSet>& q, const FmqConfig& f, prepareStream_cb cb) override { ALOGI("sensors: stream %u %s", type, "prepareStream"); cb(Result::OK, q, f); return {}; }
    // CameraStreamConfiguration starts with a hidl_vec of 112-byte per-sensor entries (writeEmbeddedToParcel in the
    // interface library); the Quest 2 has four tracking cameras
    Return<void> getConfiguration(const sp<ISensorClient>&, getConfiguration_cb cb) override { ALOGI("sensors: stream %u %s", type, "getConfiguration");
        Zero<> z; auto* v = new (z.b) hidl_vec<CamCfg>();
        v->resize(4); for (uint32_t i = 0; i < 4; i++) (*v)[i] = camCfg(i);
        *reinterpret_cast<double*>(z.b + 0x10) = muxState().rateHz;   // CameraStreamConfiguration: { cameras @0, nominalRateHz @0x10 }
        cb(Result::OK, z.as<CameraStreamConfiguration>()); v->~hidl_vec(); return {}; }
    Return<void> setConfigUpdateHandling(const sp<ISensorClient>&, const sp<IFrameConfigCallback>&, setConfigUpdateHandling_cb cb) override { ALOGI("sensors: stream %u %s", type, "setConfigUpdateHandling");
        cb(Result::OK, *reinterpret_cast<const MuxState*>(&muxState())); return {}; }
    Return<Result> streamControl(const sp<ISensorClient>&, StreamCommand c) override { ALOGI("sensors: stream %u %s", type, "streamControl"); ALOGI("sensors: camera stream %u command %u", type, (unsigned)c); return Result::OK; }
    Return<Result> startOverridingExposureSettings(const sp<ISensorClient>&) override { ALOGI("sensors: stream %u %s", type, "startOverridingExposureSettings"); return Result::OK; }
    Return<Result> stopOverridingExposureSettings(const sp<ISensorClient>&) override { ALOGI("sensors: stream %u %s", type, "stopOverridingExposureSettings"); return Result::OK; }
    Return<void> setExposureGain(const sp<ISensorClient>&, const ExposureGainSettings&) override { ALOGI("sensors: stream %u %s", type, "setExposureGain"); return {}; }
    Return<void> setPhaseOffset(uint64_t) override { ALOGI("sensors: stream %u %s", type, "setPhaseOffset"); return {}; }
    Return<uint32_t> getFrameRate() override { ALOGI("sensors: stream %u %s", type, "getFrameRate"); return 30; }
    Return<Result> setCameraSyncMode(CameraSyncMode) override { ALOGI("sensors: stream %u %s", type, "setCameraSyncMode"); return Result::OK; }
    Return<Result> setResolution(const hidl_vec<Resolution>&) override { ALOGI("sensors: stream %u %s", type, "setResolution"); return Result::OK; }
};
struct StreamController : ICameraStreamController {
    Return<Result> startOverridingExposureSettings() override { return Result::OK; }
    Return<Result> stopOverridingExposureSettings() override { return Result::OK; }
    Return<void> setExposureGain(uint8_t, uint32_t, const ExposureGainSettings&) override { return {}; }
};
static sp<ICameraStream> streamFor(uint32_t t) {
    static std::mutex m; static std::map<uint32_t, sp<ICameraStream>> all;
    std::lock_guard<std::mutex> g(m);
    auto& s = all[t]; if (!s) s = new Stream(t);
    return s;
}

struct Camera : ICameraProvider {
    // the four tracking cameras (ids 0-3); no images ever arrive
    Return<void> getProperties(getProperties_cb cb) override { ALOGI("sensors: camera.%s from pid %d", "getProperties", ::android::hardware::IPCThreadState::self()->getCallingPid());
        hidl_vec<CameraProperties> v; v.resize(4);
        for (uint32_t i = 0; i < 4; i++) { v[i].id = i; v[i].a = "OV7251"; v[i].b = "iot"; }
        cb(Result::OK, v); return {}; }
    // a handle with two ashmem regions, factory then online calibration (libvrsensors-hidlwrapper maps both)
    Return<void> getCalibrationData(uint32_t id, getCalibrationData_cb cb) override {
        std::string j = cameraJson(id);
        native_handle_t* h = native_handle_create(2, 0);
        h->data[0] = ashmemWith(j); h->data[1] = ashmemWith(j);
        cb(Result::OK, hidl_handle(h));
        native_handle_close(h); native_handle_delete(h);
        return {}; }
    Return<Result> applyMuxMode(const hidl_string& m, uint8_t) override { ALOGI("sensors: mux mode %s", m.c_str()); return Result::OK; }
    Return<sp<ICameraStream>> getStream(FrameType t) override { ALOGI("sensors: camera stream %u requested", (unsigned)t); return streamFor((uint32_t)t); }
    Return<sp<ICameraStream>> getStreamByPurpose(const hidl_string& a, const hidl_string& b) override {
        ALOGI("sensors: camera stream for %s/%s requested", a.c_str(), b.c_str()); return streamFor(1000 + std::hash<std::string>()(std::string(a) + "/" + std::string(b)) % 1000); }
    Return<sp<ICameraStreamController>> getStreamController(const hidl_string&, const hidl_string&) override { static sp<StreamController> c = new StreamController; return c; }
    Return<Result> controlStreams(const hidl_vec<sp<ICameraStreamControlTarget>>&, StreamCommand) override { return Result::OK; }
    Return<UtilityFrequency> getUtilityFrequency() override { return UtilityFrequency(0); }
    Return<Result> setUtilityFrequency(UtilityFrequency) override { return Result::OK; }
    Return<void> getChannels(getChannels_cb cb) override { Zero<> z; cb(z.as<ChannelSettings>()); return {}; }
    Return<Result> setChannels(const ChannelSettings&) override { return Result::OK; }
    Return<Result> setFrameRate(const FrameRateSettings&) override { return Result::OK; }
    Return<bool> getRawImageMode() override { return false; }
};

#define STREAM(name, T) Return<Result> name(const MQ<T>&, const sp<ISensorClient>&, const FmqConfig&) override { return Result::OK; }
struct StreamingClient : IControllerStreamingClient {
    Return<void> dispose() override { return {}; }
    // the controller manager only needs a document to hand on (its tracking comes from injection, not the IMU)
    Return<void> getCalibrationData(const ControllerAddr&, getCalibrationData_cb cb) override {
        ControllerCalibrationData d{}; d.json = touchJson(); cb(d); return {}; }
    Return<void> enable(const ControllerAddr&) override { return {}; }
    Return<void> disable(const ControllerAddr&) override { return {}; }
    Return<bool> controlInputADCStreaming(const ControllerAddr&, bool) override { return true; }
    Return<bool> setLedConfig(const ControllerLedConfig&) override { return true; }
    Return<void> getLedConfig(const ControllerAddr&, getLedConfig_cb cb) override { Zero<> z; cb(z.as<ControllerLedConfig>(), false); return {}; }
    Return<bool> setTransmitPowerBoost(int8_t) override { return true; }
    Return<bool> setSimpleHaptics(const ControllerAddr&, uint8_t) override { return true; }
    Return<bool> setMultiSimpleHaptics(const ControllerAddr&, const ::android::hardware::hidl_array<SimpleHapticIntensity, 6>&) override { return true; }
    Return<bool> setBufferedHaptics(const ControllerAddr&, double, uint8_t, bool, const hidl_vec<uint8_t>&) override { return true; }
    Return<bool> appendBufferedHaptics(const ControllerAddr&, uint8_t, const hidl_vec<uint8_t>&) override { return true; }
    Return<bool> appendPCMHaptics(const ControllerAddr&, const hidl_vec<int8_t>&) override { return true; }
    Return<bool> setThumbstickMaxADCRange(const ControllerAddr&, const ThumbstickADCRange&) override { return true; }
    Return<void> getThumbstickMaxADCRange(const ControllerAddr&, getThumbstickMaxADCRange_cb cb) override { Zero<> z; cb(z.as<ThumbstickADCRange>(), false); return {}; }
    Return<bool> resetThumbstickMaxADCRange(const ControllerAddr&) override { return true; }
    Return<bool> setThumbstickDeadzone(const ControllerAddr&, float) override { return true; }
    Return<void> getThumbstickDeadzone(const ControllerAddr&, getThumbstickDeadzone_cb cb) override { cb(0.f, false); return {}; }
    Return<bool> resetThumbstickDeadzone(const ControllerAddr&) override { return true; }
    Return<bool> sleepController(const ControllerAddr&) override { return true; }
    Return<bool> wakeController(const ControllerAddr&) override { return true; }
};
struct ManagementClient : IControllerManagementClient {
    Return<void> dispose() override { return {}; }
    Return<void> getAdvertisingControllers(getAdvertisingControllers_cb cb) override { cb({}); return {}; }
    Return<Result> enterDMM(const ControllerAddr&) override { return Result::OK; }
    Return<Result> exitDMM(const ControllerAddr&) override { return Result::OK; }
    Return<Result> pairController(const ControllerAddr&) override { return Result::OK; }
    Return<Result> unpairController(const ControllerAddr&) override { return Result::OK; }
    Return<Result> updateFirmware(const ControllerAddr&, ControllerType, const FWUpdateCallbackConfig&) override { return Result::OK; }
};

// ---- controllers ----
// Two Quest 2 Touch controllers (type 1; measured by the model the runtime loads), paired and connected, so Meta's controller manager registers them with tracking.
// Their poses and buttons come from the headset through TrackingDataInjection (input/Injector.java), not from here.
static std::array<PairedControllerInfo, 2> touchControllers() {
    std::array<PairedControllerInfo, 2> c{};
    for (int i = 0; i < 2; i++) {
        PairedControllerInfo& p = c[i];
        p.type() = property_get_int32("persist.emuxr2.controller_type", 1);   // 1 Quest 2 Touch, 0 Quest 1, 2 Touch Pro
        p.addr() = 0xC0FFEE00A000ull + i;   // 48-bit like a radio address: the injection service parses ids as signed
        p.connected() = 1;
        p.battery() = 100.f;
        strcpy(p.serial(), i ? "1WMHHEMUXR2R" : "1WMHHEMUXR2L");
        strcpy(p.firmware(), "1.0.0");
        p.flags() = 0x40 | 0x200 | (i ? 0x10 : 0x20);   // controller, Constellation-tracked (Quest 1/2 Touch), hand
    }
    return c;
}
// the connection-state stream: the client's queue, its event flag, and the bit that says "written"
struct StateStream {
    std::unique_ptr<MessageQueue<PairedControllerInfo, kSynchronizedReadWrite>> q;
    EventFlag* flag = nullptr;
    uint32_t written = 0;
};
static std::mutex stateLock;
static std::vector<std::shared_ptr<StateStream>> stateStreams;
static void publishStates() {   // repeated each second: the latest state survives a reader that missed a wake
    for (;;) {
        auto c = touchControllers();
        {
            std::lock_guard<std::mutex> l(stateLock);
            for (auto& s : stateStreams)
                if (s->q->availableToWrite() >= c.size() && s->q->write(c.data(), c.size()) && s->flag) s->flag->wake(s->written);
        }
        sleep(1);
    }
}

struct Controllers : IControllerProvider {
    Return<void> getPairedControllers(getPairedControllers_cb cb) override {
        auto c = touchControllers();
        hidl_vec<PairedControllerInfo> v; v.setToExternal(c.data(), c.size());
        ALOGI("controllers: reporting 2 paired Touch controllers");
        cb(v); return {}; }
    Return<bool> setWirelessFreqBlocklist(const ControllerWirelessFreqBlocklist&) override { return true; }
    Return<void> getWirelessFreqBlocklist(getWirelessFreqBlocklist_cb cb) override { Zero<> z; cb(z.as<ControllerWirelessFreqBlocklist>(), false); return {}; }
    Return<bool> clearWirelessFreqBlocklist() override { return true; }
    Return<void> getHostInfo(getHostInfo_cb cb) override { Zero<> z; cb(z.as<SecureHostInfo>()); return {}; }
    Return<bool> allowDevice(const SecureDeviceInfo&) override { return true; }
    Return<sp<IControllerManagementClient>> getManagementClient() override { return new ManagementClient; }
    Return<sp<IControllerStreamingClient>> getStreamingClient() override { return new StreamingClient; }
    Return<Result> prepareStateStream(const MQ<PairedControllerInfo>& d, const sp<ISensorClient>&, const FmqConfig& f) override {
        auto s = std::make_shared<StateStream>();
        s->q.reset(new MessageQueue<PairedControllerInfo, kSynchronizedReadWrite>(d));
        if (!s->q->isValid()) { ALOGW("controllers: state stream queue invalid"); return Result::OK; }
        if (s->q->getEventFlagWord()) EventFlag::createEventFlag(s->q->getEventFlagWord(), &s->flag);
        else if (f.eventFlag.getNativeHandle() && f.eventFlag->numFds >= 1)
            EventFlag::createEventFlag(dup(f.eventFlag->data[0]), 0, &s->flag);
        s->written = f.writeNotification;
        ALOGI("controllers: state stream (%zu slots, event flag %p, bits %x/%x)", s->q->getQuantumCount(), s->flag, f.readNotification, f.writeNotification);
        std::lock_guard<std::mutex> l(stateLock);
        stateStreams.push_back(s);
        static std::once_flag started;
        std::call_once(started, [] { std::thread(publishStates).detach(); });
        return Result::OK; }
    STREAM(prepareCurlStream, CurlData) STREAM(prepareImuStream, ControllerImuData) STREAM(prepareInputStream, ButtonData)
    STREAM(preparePrecisionPadStream, PrecisionPadData) STREAM(prepareMultiTouchStream, MultiTouchData)
    STREAM(prepareStylusStream, StylusData) STREAM(prepareStatsStream, WirelessDeviceStats) STREAM(preparePoseStream, PoseInput)
    STREAM(prepareInputADCStream, ControllerInputADCData) STREAM(prepareCollisionEventStream, ControllerCollisionEvent)
};

template <typename Base, typename Data> struct Motion : Base {
    // MotionSensorProperties: four strings (name, ?, factory calibration, online calibration), then plain data
    Return<void> getProperties(typename Base::getProperties_cb cb) override {
        Zero<0, 16, 32, 48> z; auto* str = reinterpret_cast<hidl_string*>(z.b);
        str[0] = "ICM42686"; str[2] = imuJson(); str[3] = imuJson();
        // MotionSensorProperties nominalRateHz is a float at offset 0x40.
        *reinterpret_cast<float*>(z.b + 0x40) = 800.0f;
        cb(Result::OK, z.template as<MotionSensorProperties>()); return {}; }
    Return<Result> prepareStream(const MQ<Data>&, const sp<ISensorClient>&, const FmqConfig&) override { return Result::OK; }
    Return<Result> streamControl(const sp<ISensorClient>&, StreamCommand) override { return Result::OK; }
    Return<Result> configureStream(const MotionStreamProperties&) override { return Result::OK; }
};

struct Iad : IIad {
    Return<void> getProperties(getProperties_cb cb) override { Zero<> z; cb(z.as<IadProperties>()); return {}; }
    Return<Result> prepareStream(const MQ<IadData>&, const sp<ISensorClient>&, const FmqConfig&) override { return Result::OK; }
    Return<Result> streamControl(const sp<ISensorClient>&, StreamCommand) override { return Result::OK; }
};

struct Timing : IExternalTimingProvider {
    Return<Result> prepareExternalTimestampDataStream(const MQ<ExternalTimestampData>&, const sp<ISensorClient>&, const FmqConfig&) override { return Result::OK; }
    Return<Result> externalTimestampDataStreamControl(const sp<ISensorClient>&, StreamCommand) override { return Result::OK; }
};

struct Zapper : IZapper {
    Return<Result> prepareStream(const MQ<ZapMessage>&, const sp<ISensorClient>&, ZapMessageClass) override { return Result::OK; }
    Return<Result> streamControl(const sp<ISensorClient>&, ZapMessageClass, StreamCommand) override { return Result::OK; }
    Return<Result> registerPulsaRpcClient(uint8_t, const sp<IZapperCallback>&) override { return Result::OK; }
    Return<Result> unregisterPulsaRpcClient(uint8_t) override { return Result::OK; }
    Return<Result> sendPulsaRpcMessage(const sp<IZapperCallback>&, const ControllerAddr&, uint32_t, const hidl_vec<uint8_t>&) override { return Result::OK; }
    Return<int64_t> getTimeTranslation(int64_t t, TimeDomain, TimeDomain) override { return t; }
};

void registerSensors() {
    static sp<Camera> cam = new Camera; static sp<Controllers> ctl = new Controllers;
    static sp<Motion<IImu, ImuData>> imu = new Motion<IImu, ImuData>; static sp<Motion<IMag, MagData>> mag = new Motion<IMag, MagData>;
    static sp<Iad> iad = new Iad; static sp<Timing> tim = new Timing; static sp<Zapper> zap = new Zapper;
    ALOGI("sensors: camera %d controllers %d imu %d mag %d iad %d timing %d zapper %d", cam->registerAsService(), ctl->registerAsService(),
          imu->registerAsService(), mag->registerAsService(), iad->registerAsService(), tim->registerAsService(), zap->registerAsService());
}
