import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;

/** Drain complete queued snapshots without consuming a partial next packet. */
final class LatestPacketInput {
    static int drain(InputStream source, byte[] packet) throws IOException {
        int skipped = 0;
        // Bound work even if the producer keeps sending while we drain.
        while (skipped < 64 && source.available() >= packet.length) {
            int offset = 0;
            while (offset < packet.length) {
                int count = source.read(packet, offset, packet.length - offset);
                if (count < 0) throw new EOFException();
                offset += count;
            }
            skipped++;
        }
        return skipped;
    }
}
