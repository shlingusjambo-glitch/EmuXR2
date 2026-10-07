import android.os.IBinder;
import android.os.Parcel;
import android.os.ServiceManager;
import android.os.SystemProperties;
import java.util.concurrent.atomic.AtomicBoolean;
import java.io.InputStream;
import java.io.File;
import java.io.RandomAccessFile;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;

/**
 * The headset's head pose for the EmuXR2 guest, through Meta's TrackingDataInjection service. Poses arrive on
 * 127.0.0.1:7791 (the host reaches it with adb forward) as 144-byte little-endian packets: u32 pose number, the head
 * pose (position x y z, orientation x y z w; OpenXR stage space, metres), then for each Touch controller (left, right)
 * u32 flags (1 = present), u32 pressed and u32 touched masks (bit n = the service's button n), its pose, then trigger,
 * grip, thumbstick x and y. Each packet is injected as it arrives, and its number is published in
 * /data/local/tmp/emuxr2-pose, which the EGL shim stamps on every captured frame. Between packets the last poses are
 * re-sent at 72 Hz so tracking never goes stale.
 */
public class Injector {
    static final String SERVICE = "TrackingDataInjection";
    static final String DESCRIPTOR = "oculus.internal.virtual_input.ITrackingDataInjectionService";
    static final int PORT = 7791, HAND = 56, PACKET = 32 + 2 * HAND, IDLE_MS = 14;
    // the controllers macvr-hal reports (left, right); the service parses ids with base prefixes
    static final String[] REMOTES = {"0xc0ffee00a000", "0xc0ffee00a001"};
    static final int TRIGGER = 0, GRIP = 4, BUTTONS = 8;   // button ids: 0 trigger, 1 home, 2 back, 3 stick, 4 grip, 5 A/X, 6 B/Y, 7 thumbrest
    static final int RELEASED = 0, TOUCHED = 2, PRESSED = 3;

    static void call(IBinder svc, int code, int[] ints, float[] floats) throws Exception { call(svc, code, null, ints, floats); }

    static void call(IBinder svc, int code, String remote, int[] ints, float[] floats) throws Exception {
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            if (remote != null) data.writeString(remote);   // String16 on the wire
            for (int v : ints) data.writeInt(v);
            for (float v : floats) data.writeFloat(v);
            svc.transact(code, data, reply, 0);
            reply.readException();
        } finally {
            reply.recycle();
            data.recycle();
        }
    }

    static IBinder svc;
    static final float[] pose = {0, 0, 0, 0, 0, 0, 1};   // raw space: y 0 is the runtime's standing eye height
    static final boolean[] present = new boolean[2];
    static final float[][] hand = new float[2][7];
    static final int[][] sent = new int[2][BUTTONS];          // last state sent per button
    static final float[][] sentValue = new float[2][BUTTONS];
    static final int[][] sentStick = {{-1, -1}, {-1, -1}};

    static void inject() throws Exception {
        call(svc, 2, new int[]{1, 4}, new float[]{pose[3], pose[4], pose[5], pose[6]});   // WorldFromImuRot
        call(svc, 2, new int[]{0, 3}, new float[]{pose[0], pose[1], pose[2]});             // WorldFromImuTrans
        for (int h = 0; h < 2; h++) {
            if (!present[h]) continue;
            float[] p = hand[h];
            call(svc, 1, REMOTES[h], new int[]{1, 4}, new float[]{p[3], p[4], p[5], p[6]});
            call(svc, 1, REMOTES[h], new int[]{0, 3}, new float[]{p[0], p[1], p[2]});
        }
    }

    // buttons and thumbstick: only what changed, as ButtonState{button, state, value} parcelables
    static void input(int h, int pressed, int touched, float trigger, float grip, float sx, float sy) throws Exception {
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            int n = 0;
            int[] state = new int[BUTTONS];
            float[] value = new float[BUTTONS];
            for (int b = 0; b < BUTTONS; b++) {
                state[b] = (pressed >> b & 1) != 0 ? PRESSED : (touched >> b & 1) != 0 ? TOUCHED : RELEASED;
                value[b] = b == TRIGGER ? trigger : b == GRIP ? grip : state[b] == PRESSED ? 1 : 0;
                if (state[b] != sent[h][b] || Math.abs(value[b] - sentValue[h][b]) > 0.01f) n++;
            }
            if (n > 0) {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeString(REMOTES[h]);
                data.writeInt(n);
                for (int b = 0; b < BUTTONS; b++) {
                    if (state[b] == sent[h][b] && Math.abs(value[b] - sentValue[h][b]) <= 0.01f) continue;
                    data.writeInt(1);    // non-null
                    data.writeInt(16);   // parcelable size
                    data.writeInt(b);
                    data.writeInt(state[b]);
                    data.writeFloat(value[b]);
                    sent[h][b] = state[b];
                    sentValue[h][b] = value[b];
                }
                svc.transact(3, data, reply, 0);
                reply.readException();
            }
        } finally {
            reply.recycle();
            data.recycle();
        }
        // raw thumbstick ADC, 0..65535 with 32768 at rest
        int x = Math.round(32768 + 32767 * Math.max(-1, Math.min(1, sx)));
        int y = Math.round(32768 + 32767 * Math.max(-1, Math.min(1, sy)));
        if (x != sentStick[h][0] || y != sentStick[h][1]) {
            call(svc, 4, REMOTES[h], new int[]{x, y}, new float[0]);
            sentStick[h][0] = x;
            sentStick[h][1] = y;
        }
    }

    static final HomeButton menuButton = new HomeButton();
    static final AtomicBoolean homePending = new AtomicBoolean();
    static void desktopHome() {
        if (!"1".equals(SystemProperties.get("persist.emuxr2.home_fallback", "0")) ||
                !homePending.compareAndSet(false, true)) return;
        Thread worker = new Thread(() -> {
            try {
                Process command = new ProcessBuilder("input", "keyevent", "HOME").start();
                command.waitFor();
            } catch (Exception error) {
                System.err.println("Injector: desktop Home failed: " + error);
            } finally { homePending.set(false); }
        }, "Guest desktop Home");
        worker.setDaemon(true);
        worker.start();
    }

    static void releaseControllers() throws Exception {
        menuButton.reset();
        for (int h = 0; h < 2; h++) {
            input(h, 0, 0, 0, 0, 0, 0);
            present[h] = false;
        }
    }

    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 100 && svc == null; i++) {
            svc = ServiceManager.getService(SERVICE);
            if (svc == null) Thread.sleep(500);
        }
        if (svc == null) { System.err.println("Injector: service not found"); return; }
        for (int h = 0; h < 2; h++) java.util.Arrays.fill(sent[h], -1);
        releaseControllers();
        call(svc, 5, new int[]{1}, new float[0]);   // setTrackingMode: injected

        File f = new File("/data/local/tmp/emuxr2-pose");
        MappedByteBuffer published;
        try (RandomAccessFile raf = new RandomAccessFile(f, "rw")) {
            raf.setLength(4096);
            published = raf.getChannel().map(FileChannel.MapMode.READ_WRITE, 0, 4096);
        }
        published.order(ByteOrder.LITTLE_ENDIAN);
        published.putInt(0, 0);
        f.setReadable(true, false);

        ServerSocket server = new ServerSocket(PORT, 1, InetAddress.getByName("127.0.0.1"));
        System.err.println("Injector: listening on " + PORT);
        byte[] buf = new byte[PACKET];
        ByteBuffer in = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN);
        while (true) {
            server.setSoTimeout(IDLE_MS);
            Socket client = null;
            try { client = server.accept(); } catch (SocketTimeoutException e) { inject(); continue; }
            client.setTcpNoDelay(true);
            client.setSoTimeout(IDLE_MS);
            System.err.println("Injector: host connected");
            long count = 0, t0 = System.nanoTime();
            try (Socket c = client; InputStream s = c.getInputStream()) {
                int have = 0;
                long lastPacket = System.nanoTime();
                while (true) {
                    try {
                        int n = s.read(buf, have, PACKET - have);
                        if (n < 0) throw new java.io.EOFException();
                        have += n;
                    } catch (SocketTimeoutException e) {
                        if (System.nanoTime() - lastPacket > 250000000L) releaseControllers();
                        inject();   // keep-alive with the last pose; a partly received packet stays in buf
                        continue;
                    }
                    if (have < PACKET) continue;
                    have = 0;
                    LatestPacketInput.drain(s, buf);
                    lastPacket = System.nanoTime();
                    int seq = in.getInt(0);
                    for (int i = 0; i < 7; i++) pose[i] = in.getFloat(4 + 4 * i);
                    for (int h = 0; h < 2; h++) {
                        int o = 32 + h * HAND;
                        present[h] = (in.getInt(o) & 1) != 0;
                        for (int i = 0; i < 7; i++) hand[h][i] = in.getFloat(o + 12 + 4 * i);
                    }
                    inject();
                    for (int h = 0; h < 2; h++) {
                        int o = 32 + h * HAND;
                        if (present[h]) input(h, in.getInt(o + 4), in.getInt(o + 8), in.getFloat(o + 40),
                                in.getFloat(o + 44), in.getFloat(o + 48), in.getFloat(o + 52));
                        else input(h, 0, 0, 0, 0, 0, 0);
                    }
                    if (menuButton.update(present[0], (in.getInt(36) & 2) != 0, System.nanoTime())) desktopHome();
                    published.putInt(0, seq);
                    if (++count % 720 == 0) {
                        long now = System.nanoTime();
                        System.err.println(String.format("Injector: %.0f poses/s", 720e9 / (now - t0)));
                        t0 = now;
                    }
                }
            } catch (Exception e) {
                System.err.println("Injector: host gone (" + e + ")");
                releaseControllers();
            }
        }
    }
}
