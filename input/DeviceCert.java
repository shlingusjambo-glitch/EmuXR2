import android.os.HwBinder;
import android.os.HwBlob;
import android.os.HwParcel;

import java.util.ArrayList;
import java.util.Arrays;

/**
 * vendor.oculus.hardware.devicecert@1.0::IDeviceCert for firmware that no longer ships the interface's native library
 * (v64): Meta's companion service still asks for it and crash-loops without it. Like hal/devicecert.cpp (which serves
 * it where the library exists), there is no secure hardware here: no key exists and every key operation is
 * unsupported; nothing is manufactured. Spoken directly in HIDL's parcel format (transaction n = the interface's
 * n-th method, as Meta's generated client numbers them), so it needs none of Meta's code.
 * Run by init (patch-extra.sh): app_process /system/bin DeviceCert
 */
public class DeviceCert extends HwBinder {
    static final String NAME = "vendor.oculus.hardware.devicecert@1.0::IDeviceCert";
    static final String BASE = "android.hidl.base@1.0::IBase";
    static final int OK = 0, UNSUPPORTED = 0x66;
    // what each method returns after its Result: 'b' a bool, 'v' a byte vector, 's' a string, 'w' two byte vectors.
    // The layout as of v64 (read from DeviceAuthServer's client): v54's, with one method added before createNonce.
    static final String METHODS = "-" +   // codes start at 1
            "-" +  // 1 generateRsaKeyPair
            "-" +  // 2 generateRsaKeyPairForCurrentSecureState
            "b" +  // 3 verifyKey
            "b" +  // 4 verifyKeyForCurrentSecureState
            "v" +  // 5 getPublicKey
            "v" +  // 6 sign
            "v" +  // 7 loadCertificate
            "-" +  // 8 storeCertificate
            "b" +  // 9 getDeviceLockState
            "-" +  // 10 prototypeProvision
            "-" +  // 11 (added in v64; no client reads it)
            "s" +  // 12 createNonce
            "b" +  // 13 verifyNonce
            "-" +  // 14 invalidateNonce
            "w" +  // 15 ovrtzAttestPayload
            "-" +  // 16 ovrtzKeyBoxProvision
            "-" +  // 17 isKeyBoxProvisioned
            "-";   // 18 removeKeyBox

    @Override
    public void onTransact(int code, HwParcel request, HwParcel reply, int flags) {
        switch (code) {
            case 0x0f43484e:   // interfaceChain
                request.enforceInterface(BASE);
                reply.writeStatus(0);
                reply.writeStringVector(new ArrayList<>(Arrays.asList(NAME, BASE)));
                reply.send();
                return;
            case 0x0f445343:   // interfaceDescriptor
                request.enforceInterface(BASE);
                reply.writeStatus(0);
                reply.writeString(NAME);
                reply.send();
                return;
            case 0x0f485348: { // getHashChain: unhashed (all zero), like an interface under development
                request.enforceInterface(BASE);
                reply.writeStatus(0);
                HwBlob vec = new HwBlob(16), hashes = new HwBlob(2 * 32);
                hashes.putInt8Array(0, new byte[64]);
                vec.putBlob(0, hashes);
                vec.putInt32(8, 2);
                vec.putBool(12, false);
                reply.writeBuffer(vec);
                reply.send();
                return;
            }
            case 0x0f504e47:   // ping
            case 0x0f444247:   // debug
                reply.writeStatus(0);
                reply.send();
                return;
            case 0x0f445049: { // getDebugInfo: pid, no pointer, 64-bit
                request.enforceInterface(BASE);
                reply.writeStatus(0);
                HwBlob info = new HwBlob(24);
                info.putInt32(0, android.os.Process.myPid());
                info.putInt64(8, 0);
                info.putInt32(16, 2);
                reply.writeBuffer(info);
                reply.send();
                return;
            }
            case 0x0f535953:   // notifySyspropsChanged (oneway)
            case 0x0f494e54:   // setHALInstrumentation (oneway)
                return;
        }
        if (code < 1 || code >= METHODS.length()) {
            reply.writeStatus(-74);   // UNKNOWN_TRANSACTION
            reply.send();
            return;
        }
        request.enforceInterface(NAME);
        char out = METHODS.charAt(code);
        reply.writeStatus(0);
        reply.writeInt32(code == 3 || code == 4 ? OK : UNSUPPORTED);   // verifyKey*: answered, no such key
        if (out == 'b') reply.writeBool(false);
        if (out == 'v' || out == 'w') reply.writeInt8Vector(new ArrayList<>());
        if (out == 'w') reply.writeInt8Vector(new ArrayList<>());
        if (out == 's') reply.writeString("");
        reply.send();
    }

    public static void main(String[] args) throws Exception {
        HwBinder.configureRpcThreadpool(2, true);
        new DeviceCert().registerService("default");
        System.err.println("DeviceCert: registered " + NAME + "/default (no secure hardware)");
        HwBinder.joinRpcThreadpool();
    }
}
