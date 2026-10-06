import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.Looper;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * The guest's audio for the headset: records Android's whole output mix through the remote submix (while a recorder
 * holds it, playback goes there instead of the emulator's speaker) and serves it on 127.0.0.1:7793 as raw PCM s16le,
 * 48000 Hz stereo, in 10 ms chunks of 1920 bytes. The host forwards each chunk to the Quest as a VR4_AUDIO packet.
 * Usage (root): app_process / Audio
 */
public class Audio {
    static final int PORT = 7793, RATE = 48000, CHUNK = 1920;

    public static void main(String[] args) throws Exception {
        Looper.prepareMainLooper();
        int min = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        ServerSocket server = new ServerSocket(PORT, 1, InetAddress.getByName("127.0.0.1"));
        System.err.println("Audio: listening on " + PORT);
        byte[] buf = new byte[CHUNK];
        while (true) {
            try (Socket client = server.accept(); OutputStream out = client.getOutputStream()) {
                client.setTcpNoDelay(true);
                // recording only while the host listens: otherwise the guest's sound stays on the emulator's speaker
                AudioRecord rec = new AudioRecord(MediaRecorder.AudioSource.REMOTE_SUBMIX, RATE,
                        AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT, Math.max(min, CHUNK * 4));
                if (rec.getState() != AudioRecord.STATE_INITIALIZED) { System.err.println("Audio: recorder failed"); rec.release(); continue; }
                rec.startRecording();
                System.err.println("Audio: host connected, recording");
                try {
                    while (true) {
                        int have = 0;
                        while (have < CHUNK) {
                            int n = rec.read(buf, have, CHUNK - have);
                            if (n < 0) throw new IllegalStateException("read " + n);
                            have += n;
                        }
                        out.write(buf);
                    }
                } finally {
                    rec.stop();
                    rec.release();
                }
            } catch (Exception e) {
                System.err.println("Audio: host gone (" + e + ")");
            }
        }
    }
}
