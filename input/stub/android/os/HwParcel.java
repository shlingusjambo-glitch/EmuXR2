package android.os;

import java.util.ArrayList;

/** Compile-time stand-in for the framework's hidden class; the real one is on the boot classpath. */
public class HwParcel {
    public final void enforceInterface(String descriptor) {}
    public final void writeStatus(int status) {}
    public final void writeInt32(int v) {}
    public final void writeBool(boolean v) {}
    public final void writeString(String v) {}
    public final void writeStringVector(ArrayList<String> v) {}
    public final void writeInt8Vector(ArrayList<Byte> v) {}
    public final void writeBuffer(HwBlob blob) {}
    public final void send() {}
}
