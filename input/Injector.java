import android.os.IBinder;
import android.os.Parcel;
import android.os.ServiceManager;
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
 * 127.0.0.1:7791 (the host reaches it with adb forward) as 32-byte little-endian packets: u32 pose number, then
 * position x y z and orientation x y z w (OpenXR stage space, metres). Each one is injected as it arrives, and its
 * number is published in /data/local/tmp/emuxr2-pose, which the EGL shim stamps on every captured frame. Between
 * packets the last pose is re-sent at 72 Hz so tracking never goes stale.
 */
public class Injector {
    static final String SERVICE = "TrackingDataInjection";
    static final String DESCRIPTOR = "oculus.internal.virtual_input.ITrackingDataInjectionService";
    static final int PORT = 7791, PACKET = 32, IDLE_MS = 14;

    static void call(IBinder svc, int code, int[] ints, float[] floats) throws Exception {
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
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
    static final float[] pose = {0, 1.6f, 0, 0, 0, 0, 1};

    static void inject() throws Exception {
        call(svc, 2, new int[]{1, 4}, new float[]{pose[3], pose[4], pose[5], pose[6]});   // WorldFromImuRot
        call(svc, 2, new int[]{0, 3}, new float[]{pose[0], pose[1], pose[2]});             // WorldFromImuTrans
    }

    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 100 && svc == null; i++) {
            svc = ServiceManager.getService(SERVICE);
            if (svc == null) Thread.sleep(500);
        }
        if (svc == null) { System.err.println("Injector: service not found"); return; }
        call(svc, 5, new int[]{1}, new float[0]);   // setTrackingMode: injected

        File f = new File("/data/local/tmp/emuxr2-pose");
        MappedByteBuffer published;
        try (RandomAccessFile raf = new RandomAccessFile(f, "rw")) {
            raf.setLength(4096);
            published = raf.getChannel().map(FileChannel.MapMode.READ_WRITE, 0, 4096);
        }
        published.order(ByteOrder.LITTLE_ENDIAN);
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
                while (true) {
                    try {
                        int n = s.read(buf, have, PACKET - have);
                        if (n < 0) throw new java.io.EOFException();
                        have += n;
                    } catch (SocketTimeoutException e) {
                        inject();   // keep-alive with the last pose; a partly received packet stays in buf
                        continue;
                    }
                    if (have < PACKET) continue;
                    have = 0;
                    int seq = in.getInt(0);
                    for (int i = 0; i < 7; i++) pose[i] = in.getFloat(4 + 4 * i);
                    inject();
                    published.putInt(0, seq);
                    if (++count % 720 == 0) {
                        long now = System.nanoTime();
                        System.err.println(String.format("Injector: %.0f poses/s", 720e9 / (now - t0)));
                        t0 = now;
                    }
                }
            } catch (Exception e) {
                System.err.println("Injector: host gone (" + e + ")");
            }
        }
    }
}
