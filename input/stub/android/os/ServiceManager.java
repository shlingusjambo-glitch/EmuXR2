package android.os;

/** Compile-time stub for the hidden framework class. At runtime under
 * app_process the bootclasspath's real ServiceManager shadows this. */
public class ServiceManager {
    public static IBinder getService(String name) {
        return null;
    }
}
