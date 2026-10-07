package android.os;

/** Compile-time stand-in for the framework's hidden class; the real one is on the boot classpath. */
public abstract class HwBinder {
    public abstract void onTransact(int code, HwParcel request, HwParcel reply, int flags) throws RemoteException;
    public final void registerService(String serviceName) throws RemoteException {}
    public static void configureRpcThreadpool(long maxThreads, boolean callerWillJoin) {}
    public static void joinRpcThreadpool() {}
}
