import android.app.PendingIntent;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.os.Looper;
import android.os.SystemProperties;
import android.view.Surface;
import android.view.SurfaceControl;
import java.lang.reflect.Method;

/**
 * Streams Horizon's undistorted stereo view out of the guest: asks the runtime for its casting capture (the
 * compositor's own undistorted render, both eyes side by side: the path Quest casting uses) into a SurfaceFlinger
 * layer that covers the emulator's display, so the host reads it from the emulator's framebuffer. The request goes
 * out as the trusted system package Meta's receiver expects; root may create a PendingIntent for any package.
 * Usage (root): app_process / Capture <capture width> <capture height> <display width> <display height>
 */
public class Capture {
    static final String ACTION = "com.oculus.systemactivities.BEGIN_VIDEO_CAPTURE_WITH_SURFACE";
    static final String STOP = "com.oculus.systemactivities.STOP_VIDEO_CAPTURE";
    static final int BOTH_EYES = 2;
    static SurfaceControl layer;   // held for the process lifetime: a collected layer or Surface ends the capture
    static Surface surface;

    static Method find(Class<?> c, String name) {
        for (Method m : c.getMethods()) if (m.getName().equals(name)) return m;
        throw new IllegalStateException("no " + name);
    }

    // IActivityManager's parameter lists differ between releases: fill them by type
    static PendingIntent trustedToken(Object am) throws Exception {
        Method m = find(am.getClass(), "getIntentSenderWithFeature");
        Class<?>[] t = m.getParameterTypes();
        Object[] a = new Object[t.length];
        int[] ints = {1 /* broadcast */, 0 /* request code */, PendingIntent.FLAG_IMMUTABLE, 0 /* user */};
        int n = 0; boolean pkg = false;
        for (int i = 0; i < t.length; i++) {
            if (t[i] == int.class) a[i] = n < ints.length ? ints[n++] : 0;
            else if (t[i] == String.class && !pkg) { a[i] = "com.oculus.systemux"; pkg = true; }
            else if (t[i] == Intent[].class) a[i] = new Intent[]{new Intent("emuxr2.capture.token")};
        }
        return PendingIntent.class.getDeclaredConstructor(Class.forName("android.content.IIntentSender")).newInstance(m.invoke(am, a));
    }

    static void broadcast(Object am, Intent intent) throws Exception {
        Method m = find(am.getClass(), "broadcastIntentWithFeature");
        Class<?>[] t = m.getParameterTypes();
        Object[] a = new Object[t.length];
        int last = -1;
        for (int i = 0; i < t.length; i++) if (t[i] == int.class) last = i;
        for (int i = 0; i < t.length; i++) {
            if (t[i] == int.class) a[i] = i == last ? 0 /* user */ : -1;   // result code, app op: none
            else if (t[i] == boolean.class) a[i] = false;
            else if (t[i] == Intent.class) a[i] = intent;
        }
        m.invoke(am, a);
    }

    public static void main(String[] argv) throws Exception {
        int w = Integer.parseInt(argv[0]), h = Integer.parseInt(argv[1]);
        int dw = Integer.parseInt(argv[2]), dh = Integer.parseInt(argv[3]);
        Looper.prepareMainLooper();
        layer = new SurfaceControl.Builder().setName("EmuXR2 capture").setBufferSize(w, h)
                .setFormat(PixelFormat.RGBA_8888).setOpaque(true).build();
        SurfaceControl.Transaction t = new SurfaceControl.Transaction();
        find(t.getClass(), "setLayerStack").invoke(t, layer, 0);   // the emulator's display
        // landscape capture onto the portrait panel, turned the way the panel's own content is
        boolean turn = (w > h) != (dw > dh);
        t.setGeometry(layer, new Rect(0, 0, w, h), new Rect(0, 0, dw, dh), turn ? Surface.ROTATION_90 : Surface.ROTATION_0);
        t.setLayer(layer, Integer.MAX_VALUE).setVisibility(layer, true).apply();
        surface = new Surface(layer);
        // tells the EGL shim in the runtime which window surface is this capture (it stamps pose numbers on it)
        SystemProperties.set("debug.emuxr2.capture", w + "x" + h);

        Object am = Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        // a capture the runtime stopped itself (a swap error) or never released ignores the next start: stop it first
        Intent stop = new Intent(STOP).setPackage("com.oculus.systemdriver");
        stop.putExtra("_ci_", trustedToken(am));
        broadcast(am, stop);
        Thread.sleep(300);
        Intent i = new Intent(ACTION).setPackage("com.oculus.systemdriver");
        i.putExtra("_ci_", trustedToken(am));
        i.putExtra("surface", surface);
        i.putExtra("show_capture_indicator", false);
        i.putExtra("lift_inhibit", true);
        i.putExtra("video_capture_eye_selection", BOTH_EYES);
        i.putExtra("video_capture_frame_rate_divisor", 1);
        broadcast(am, i);
        System.out.println("capture " + w + "x" + h + " on the " + dw + "x" + dh + " display");
        Looper.loop();
    }
}
