import java.io.ByteArrayInputStream;
import java.util.Arrays;

public final class LatestPacketInputTest {
    public static void main(String[] args) throws Exception {
        byte[] packet = new byte[144];
        byte[] queued = new byte[144 * 3 + 51];
        Arrays.fill(queued, 0, 144, (byte) 1);
        Arrays.fill(queued, 144, 288, (byte) 2);
        Arrays.fill(queued, 288, 432, (byte) 3);
        Arrays.fill(queued, 432, queued.length, (byte) 4);
        ByteArrayInputStream source = new ByteArrayInputStream(queued);
        assert LatestPacketInput.drain(source, packet) == 3;
        for (byte value : packet) assert value == 3;
        assert source.available() == 51 : "partial next packet must remain queued";
        assert LatestPacketInput.drain(source, packet) == 0;
        assert source.read() == 4;
        source = new ByteArrayInputStream(new byte[144 * 100]);
        assert LatestPacketInput.drain(source, packet) == 64;
        assert source.available() == 144 * 36 : "draining must be bounded";
        System.out.println("PASS: latest complete snapshot, partial packet preservation, bounded draining");
    }
}
