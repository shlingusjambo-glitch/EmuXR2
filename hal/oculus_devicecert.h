// ABI declarations recovered from the user's devicecert HIDL interface library.
#pragma once
#include "oculus_composer.h"
namespace vendor::oculus::hardware::devicecert::V1_0 {
using ::android::hardware::Return;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
enum class Result : uint32_t { OK = 0, ERROR = 1 };
struct IDeviceCert : public ::android::hidl::base::V1_0::IBase {
    static const char *descriptor;
    using BoolCallback = std::function<void(Result, bool)>;
    using BytesCallback = std::function<void(Result, const hidl_vec<uint8_t>&)>;
    using NonceCallback = std::function<void(Result, const hidl_string&)>;
    using AttestCallback = std::function<void(Result, const hidl_vec<uint8_t>&, const hidl_vec<uint8_t>&)>;
    virtual Return<Result> generateRsaKeyPair(const hidl_string&) = 0;
    virtual Return<Result> generateRsaKeyPairForCurrentSecureState(const hidl_string&) = 0;
    virtual Return<void> verifyKey(const hidl_string&, BoolCallback) = 0;
    virtual Return<void> verifyKeyForCurrentSecureState(const hidl_string&, BoolCallback) = 0;
    virtual Return<void> getPublicKey(const hidl_string&, const hidl_string&, BytesCallback) = 0;
    virtual Return<void> sign(const hidl_string&, const hidl_string&, const hidl_vec<uint8_t>&, BytesCallback) = 0;
    virtual Return<void> loadCertificate(const hidl_string&, const hidl_string&, BytesCallback) = 0;
    virtual Return<Result> storeCertificate(const hidl_string&, const hidl_string&, const hidl_vec<uint8_t>&) = 0;
    virtual Return<void> getDeviceLockState(BoolCallback) = 0;
    virtual Return<Result> prototypeProvision(const hidl_string&, const hidl_vec<uint8_t>&, const hidl_vec<uint8_t>&, const hidl_vec<uint8_t>&) = 0;
    virtual Return<void> createNonce(const hidl_string&, uint32_t, bool, uint32_t, NonceCallback) = 0;
    virtual Return<void> verifyNonce(const hidl_string&, const hidl_string&, BoolCallback) = 0;
    virtual Return<Result> invalidateNonce(const hidl_string&) = 0;
    virtual Return<void> ovrtzAttestPayload(const hidl_string&, const hidl_vec<uint8_t>&, AttestCallback) = 0;
    virtual Return<Result> ovrtzKeyBoxProvision(const hidl_vec<uint8_t>&, bool) = 0;
    virtual Return<Result> isKeyBoxProvisioned() = 0;
    virtual Return<Result> removeKeyBox() = 0;
    MACVR_IBASE_OVERRIDES
};
}
