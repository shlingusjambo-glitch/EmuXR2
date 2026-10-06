public final class HomeButtonTest {
    public static void main(String[] args) {
        HomeButton menu = new HomeButton();
        assert !menu.update(true, false, 0L);
        assert !menu.update(true, true, 100L);
        assert !menu.update(true, true, 100_000_000L);
        assert menu.update(true, false, 200_000_000L);
        assert !menu.update(true, false, 300_000_000L); // Single release, no repeated Home.
        assert !menu.update(true, true, 1_000_000_000L);
        assert !menu.update(true, false, 2_000_000_000L); // Hold remains native recenter.
        assert !menu.update(true, true, 3_000_000_000L);
        assert !menu.update(false, false, 3_100_000_000L); // Lost controller cannot fire Home.
        assert !menu.update(true, false, 3_200_000_000L);
        assert !menu.update(true, true, 4_000_000_000L);
        menu.reset(); // Connection timeout/startup release does not navigate.
        assert !menu.update(true, false, 4_100_000_000L);
        System.out.println("PASS: short press, long hold, release deduplication, controller loss and timeout");
    }
}
