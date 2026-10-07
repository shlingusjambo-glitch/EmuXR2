package android.os;

/** Compile-time stand-in for the framework's hidden class; the real one is on the boot classpath. */
public class HwBlob {
    public HwBlob(int size) {}
    public final void putInt32(long offset, int v) {}
    public final void putInt64(long offset, long v) {}
    public final void putBool(long offset, boolean v) {}
    public final void putInt8Array(long offset, byte[] v) {}
    public final void putBlob(long offset, HwBlob blob) {}
}
