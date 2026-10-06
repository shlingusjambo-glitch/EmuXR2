// Answer key-existence queries truthfully; report unsupported secure-hardware operations.
// This adapter never manufactures keys, signatures, device certificates or attestation.
#include "oculus_devicecert.h"
#include <log/log.h>
using namespace vendor::oculus::hardware::devicecert::V1_0;
struct MacVRDeviceCert : IDeviceCert {
    Return<Result> generateRsaKeyPair(const hidl_string&) override { return Result::UNSUPPORTED_COMMAND; }
    Return<Result> generateRsaKeyPairForCurrentSecureState(const hidl_string&) override { return Result::UNSUPPORTED_COMMAND; }
    Return<void> verifyKey(const hidl_string&, BoolCallback cb) override { cb(Result::OK, false); return {}; }
    Return<void> verifyKeyForCurrentSecureState(const hidl_string&, BoolCallback cb) override { cb(Result::OK, false); return {}; }
    Return<void> getPublicKey(const hidl_string&, const hidl_string&, BytesCallback cb) override { cb(Result::UNSUPPORTED_COMMAND, {}); return {}; }
    Return<void> sign(const hidl_string&, const hidl_string&, const hidl_vec<uint8_t>&, BytesCallback cb) override { cb(Result::UNSUPPORTED_COMMAND, {}); return {}; }
    Return<void> loadCertificate(const hidl_string&, const hidl_string&, BytesCallback cb) override { cb(Result::UNSUPPORTED_COMMAND, {}); return {}; }
    Return<Result> storeCertificate(const hidl_string&, const hidl_string&, const hidl_vec<uint8_t>&) override { return Result::UNSUPPORTED_COMMAND; }
    Return<void> getDeviceLockState(BoolCallback cb) override { cb(Result::UNSUPPORTED_COMMAND, false); return {}; }
    Return<Result> prototypeProvision(const hidl_string&, const hidl_vec<uint8_t>&, const hidl_vec<uint8_t>&, const hidl_vec<uint8_t>&) override { return Result::UNSUPPORTED_COMMAND; }
    Return<void> createNonce(const hidl_string&, uint32_t, bool, uint32_t, NonceCallback cb) override { cb(Result::UNSUPPORTED_COMMAND, ""); return {}; }
    Return<void> verifyNonce(const hidl_string&, const hidl_string&, BoolCallback cb) override { cb(Result::UNSUPPORTED_COMMAND, false); return {}; }
    Return<Result> invalidateNonce(const hidl_string&) override { return Result::UNSUPPORTED_COMMAND; }
    Return<void> ovrtzAttestPayload(const hidl_string&, const hidl_vec<uint8_t>&, AttestCallback cb) override { cb(Result::UNSUPPORTED_COMMAND, {}, {}); return {}; }
    Return<Result> ovrtzKeyBoxProvision(const hidl_vec<uint8_t>&, bool) override { return Result::UNSUPPORTED_COMMAND; }
    Return<Result> isKeyBoxProvisioned() override { return Result::UNSUPPORTED_COMMAND; }
    Return<Result> removeKeyBox() override { return Result::UNSUPPORTED_COMMAND; }
};
void registerDeviceCert() {
    static ::android::sp<MacVRDeviceCert> service = new MacVRDeviceCert;
    ALOGI("devicecert (no secure hardware): %d", service->registerAsService());
}
