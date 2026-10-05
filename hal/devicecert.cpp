// Expose absence of secure hardware as an error, rather than a missing binder service.
// This adapter never manufactures keys, signatures, device certificates or attestation.
#include "oculus_devicecert.h"
#include <log/log.h>
using namespace vendor::oculus::hardware::devicecert::V1_0;
struct MacVRDeviceCert : IDeviceCert {
    Return<Result> generateRsaKeyPair(const hidl_string&) override { return Result::ERROR; }
    Return<Result> generateRsaKeyPairForCurrentSecureState(const hidl_string&) override { return Result::ERROR; }
    Return<void> verifyKey(const hidl_string&, BoolCallback cb) override { cb(Result::ERROR, false); return {}; }
    Return<void> verifyKeyForCurrentSecureState(const hidl_string&, BoolCallback cb) override { cb(Result::ERROR, false); return {}; }
    Return<void> getPublicKey(const hidl_string&, const hidl_string&, BytesCallback cb) override { cb(Result::ERROR, {}); return {}; }
    Return<void> sign(const hidl_string&, const hidl_string&, const hidl_vec<uint8_t>&, BytesCallback cb) override { cb(Result::ERROR, {}); return {}; }
    Return<void> loadCertificate(const hidl_string&, const hidl_string&, BytesCallback cb) override { cb(Result::ERROR, {}); return {}; }
    Return<Result> storeCertificate(const hidl_string&, const hidl_string&, const hidl_vec<uint8_t>&) override { return Result::ERROR; }
    Return<void> getDeviceLockState(BoolCallback cb) override { cb(Result::ERROR, false); return {}; }
    Return<Result> prototypeProvision(const hidl_string&, const hidl_vec<uint8_t>&, const hidl_vec<uint8_t>&, const hidl_vec<uint8_t>&) override { return Result::ERROR; }
    Return<void> createNonce(const hidl_string&, uint32_t, bool, uint32_t, NonceCallback cb) override { cb(Result::ERROR, ""); return {}; }
    Return<void> verifyNonce(const hidl_string&, const hidl_string&, BoolCallback cb) override { cb(Result::ERROR, false); return {}; }
    Return<Result> invalidateNonce(const hidl_string&) override { return Result::ERROR; }
    Return<void> ovrtzAttestPayload(const hidl_string&, const hidl_vec<uint8_t>&, AttestCallback cb) override { cb(Result::ERROR, {}, {}); return {}; }
    Return<Result> ovrtzKeyBoxProvision(const hidl_vec<uint8_t>&, bool) override { return Result::ERROR; }
    Return<Result> isKeyBoxProvisioned() override { return Result::ERROR; }
    Return<Result> removeKeyBox() override { return Result::ERROR; }
};
void registerDeviceCert() {
    static ::android::sp<MacVRDeviceCert> service = new MacVRDeviceCert;
    ALOGI("devicecert (no secure hardware): %d", service->registerAsService());
}
