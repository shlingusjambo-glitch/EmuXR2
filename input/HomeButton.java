/** Short menu releases request desktop Home; holds retain native recenter behavior. */
final class HomeButton {
    private long pressedAt;
    private boolean pressed;

    boolean update(boolean present, boolean down, long now) {
        if (!present) { reset(); return false; }
        if (down && !pressed) { pressed = true; pressedAt = now; }
        if (!down && pressed) {
            pressed = false;
            return now >= pressedAt && now - pressedAt < 500_000_000L;
        }
        return false;
    }

    void reset() { pressed = false; }
}
