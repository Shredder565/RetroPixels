package io.github.shredder565.retropixels.engine;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The on-screen controls for a system: a D-pad (or the N64's stick) on the
 * left, the face buttons on the right placed and named as on that system's
 * pad, shoulder buttons at the top corners, Select / Start (Coin / Start,
 * Pause / Reset...) at the bottom in the middle, and any other buttons the
 * game has as small pills. Plain Java (sizes in pixels), so it is tested on a
 * computer; TouchControls draws it and reads the fingers.
 */
public final class TouchLayout {
    public static final int BUTTON = 0; // a round button
    public static final int PILL = 1; // a small rounded rectangle button
    public static final int DPAD = 2;
    public static final int STICK = 3; // an analog stick (player 1's left stick)
    public static final int MENU = 4; // opens the game menu
    public static final int FAST = 5; // fast-forward while held

    /** One control. */
    public static final class Control {
        public final int kind;
        public final String name; // the game's button name (BUTTON, PILL)
        public final String label;
        public float cx, cy, r; // centre; radius (BUTTON, DPAD, STICK, MENU, FAST) or half height (PILL)
        public float hw; // PILL: half width
        public int bit = -1; // BUTTON, PILL: the button's bit in the game's buttons
        public int up = -1, down = -1, left = -1, right = -1; // DPAD: each direction's bit, or -1

        Control(int kind, String name, String label) {
            this.kind = kind;
            this.name = name;
            this.label = label;
        }

        /** How far (x, y) is inside the control's touch area: <= 1 is inside. */
        public float reach(float x, float y) {
            float dx = x - cx, dy = y - cy;
            switch (kind) {
                case PILL: {
                    float ax = Math.max(0, Math.abs(dx) - hw + r), ay = Math.abs(dy);
                    return (float) Math.sqrt(ax * ax + ay * ay) / (r * 1.5f);
                }
                case DPAD:
                    return (float) Math.sqrt(dx * dx + dy * dy) / (r * 1.3f);
                case STICK:
                    return (float) Math.sqrt(dx * dx + dy * dy) / (r * 1.6f);
                default:
                    return (float) Math.sqrt(dx * dx + dy * dy) / (r * 1.25f);
            }
        }
    }

    public final List<Control> controls = new ArrayList<Control>();
    /** Every button the game has that has a control. */
    public final Set<String> placed = new HashSet<String>();

    private final List<String> buttons;

    private TouchLayout(List<String> buttons) {
        this.buttons = buttons;
    }

    private int bit(String name) {
        int i = buttons.indexOf(name);
        return i >= 0 && i < 32 ? i : -1;
    }

    private boolean has(String name) {
        return bit(name) >= 0;
    }

    private Control button(String name, String label, float cx, float cy, float r) {
        Control c = new Control(BUTTON, name, label);
        c.cx = cx;
        c.cy = cy;
        c.r = r;
        c.bit = bit(name);
        controls.add(c);
        placed.add(name);
        return c;
    }

    private Control pill(String name, String label, float cx, float cy, float hw, float hh) {
        Control c = new Control(PILL, name, label);
        c.cx = cx;
        c.cy = cy;
        c.hw = hw;
        c.r = hh;
        c.bit = bit(name);
        controls.add(c);
        placed.add(name);
        return c;
    }

    /** Short labels for buttons with long names. */
    static String labelFor(String name) {
        if (name.equals("Left difficulty")) return "L diff";
        if (name.equals("Right difficulty")) return "R diff";
        if (name.equals("Color / B&W")) return "Colour";
        if (name.equals("Start (1P)")) return "Start";
        if (name.equals("Start (2P)")) return "2P Start";
        if (name.equals("Button 1")) return "1";
        if (name.equals("Button 2")) return "2";
        if (name.equals("Button 3")) return "3";
        if (name.equals("Button 4")) return "4";
        if (name.startsWith("C-")) return "C" + arrow(name.substring(2));
        if (name.startsWith("Tilt ")) return "Tilt " + arrow(name.substring(5));
        return name;
    }

    private static String arrow(String dir) {
        if (dir.equals("Up")) return "▲";
        if (dir.equals("Down")) return "▼";
        if (dir.equals("Left")) return "◀";
        if (dir.equals("Right")) return "▶";
        return dir;
    }

    /**
     * Lays out the controls for a w x h screen. dp: pixels per density point;
     * scale: the player's size choice (1 = normal). picBottom: in portrait,
     * where the picture ends (the controls go below it).
     */
    public static TouchLayout make(String system, List<String> buttons, int w, int h, float dp, float scale, int picBottom) {
        TouchLayout t = new TouchLayout(buttons);
        boolean portrait = h > w;
        float u = dp * scale;
        float m = 20 * dp; // margin
        // the left cluster (D-pad or stick) and the right cluster (face buttons)
        float pad = 68 * u, br = 30 * u;
        float need = 2 * pad + faceWidth(system, buttons, br) + 4 * m;
        if (need > w) { // a narrow screen: shrink to fit
            float k = w / need;
            pad *= k;
            br *= k;
            u *= k;
        }
        float top = portrait ? Math.max(picBottom, 0) : 0;
        float bottom = h - m;
        float midY = portrait ? bottom - 40 * u - pad : bottom - pad;
        if (portrait && midY - pad < top + 70 * u) { // little room under the picture
            midY = Math.max(top + 70 * u + pad, bottom - pad);
        }
        float leftX = m + pad, rightX = w - m - pad;
        float shoulderY = portrait ? top + m + 18 * u : m + 18 * u;
        float menuY = portrait ? top + m + 18 * u : m + 18 * u;
        float centreY = bottom - 16 * u;

        boolean n64 = system.equals("n64");
        // ---- left: the D-pad, or the N64's stick with its D-pad above ----
        if (n64) {
            Control s = new Control(STICK, "stick", "");
            s.cx = leftX;
            s.cy = midY;
            s.r = pad * 0.9f;
            t.controls.add(s);
        } else {
            t.dpad(leftX, midY, pad);
        }

        // ---- right: the face buttons, moved in from the edges as far as needed ----
        int first = t.controls.size();
        t.face(system, rightX, midY, br, u);
        float maxX = 0, maxY = 0, minY = h;
        for (int i = first; i < t.controls.size(); i++) {
            Control c = t.controls.get(i);
            maxX = Math.max(maxX, c.cx + c.r);
            maxY = Math.max(maxY, c.cy + c.r);
            minY = Math.min(minY, c.cy - c.r);
        }
        float shiftX = Math.min(0, w - m - maxX);
        float shiftY = Math.min(0, bottom - maxY);
        if (minY + shiftY < shoulderY + 30 * u) {
            shiftY = Math.min(bottom - maxY, shoulderY + 30 * u - minY);
        }
        for (int i = first; i < t.controls.size(); i++) {
            t.controls.get(i).cx += shiftX;
            t.controls.get(i).cy += shiftY;
        }

        // ---- the shoulders ----
        float sw = 40 * u, sh = 18 * u;
        if (t.has("L")) t.pill("L", "L", m + sw, shoulderY, sw, sh);
        if (n64 && t.has("Z")) t.pill("Z", "Z", m + sw, shoulderY + 2 * sh + 12 * u, sw, sh); // the trigger, below L
        if (t.has("R")) t.pill("R", "R", w - m - sw, shoulderY, sw, sh);

        // the N64's D-pad: small, above the stick, or the free place nearest that
        if (n64) {
            float dr = 36 * u;
            float px = leftX, py = midY - pad * 0.9f - 16 * u - dr;
            float bx = px, by = py, best = Float.MAX_VALUE;
            float step = 6 * u;
            for (float y = top + dr; y <= h - dr; y += step) {
                for (float x = dr + m / 2; x <= w / 2f; x += step) {
                    float d = (x - px) * (x - px) + (y - py) * (y - py);
                    if (d < best && t.free(x, y, dr * 1.1f)) {
                        best = d;
                        bx = x;
                        by = y;
                    }
                }
            }
            t.dpad(bx, by, dr);
        }

        // ---- the middle: Select / Start and their kind ----
        List<String> mid = new ArrayList<String>();
        for (String n : new String[] {"Select", "Mode", "Coin", "Pause", "Reset", "Start", "Start (1P)"}) {
            if (t.has(n) && !t.placed.contains(n)) {
                mid.add(n);
            }
        }
        float pw = 34 * u, ph = 15 * u, gap = 10 * u;
        float total = mid.size() * 2 * pw + Math.max(0, mid.size() - 1) * gap;
        float x = w / 2f - total / 2 + pw;
        for (String n : mid) {
            t.pill(n, labelFor(n), x, centreY, pw, ph);
            x += 2 * pw + gap;
        }

        // ---- the menu and fast-forward buttons ----
        Control menu = new Control(MENU, "menu", "≡");
        menu.cx = w / 2f - 26 * u;
        menu.cy = menuY;
        menu.r = 17 * u;
        t.controls.add(menu);
        Control fast = new Control(FAST, "fast", "»");
        fast.cx = w / 2f + 26 * u;
        fast.cy = menuY;
        fast.r = 17 * u;
        t.controls.add(fast);

        // ---- everything else the game has: small pills, in rows above the middle ----
        List<String> rest = new ArrayList<String>();
        for (String n : buttons) {
            if (!t.placed.contains(n) && !isStick(n) && !n.equals("Up") && !n.equals("Down") && !n.equals("Left") && !n.equals("Right")) {
                rest.add(n);
            }
        }
        float ew = 36 * u, eh = 14 * u;
        int perRow = Math.max(1, (int) ((w - 2 * (m + 2 * pad)) / (2 * ew + gap)));
        if (portrait) {
            perRow = Math.max(1, (int) ((w - 2 * m) / (2 * ew + gap)));
        }
        float rowY = centreY - 2 * ph - gap - eh;
        if (portrait) {
            rowY = menuY + 17 * u + gap + eh;
        }
        for (int i = 0; i < rest.size(); i += perRow) {
            int k = Math.min(perRow, rest.size() - i);
            float tw = k * 2 * ew + (k - 1) * gap;
            float ex = w / 2f - tw / 2 + ew;
            for (int j = 0; j < k; j++) {
                String n = rest.get(i + j);
                t.pill(n, labelFor(n), ex, rowY, ew, eh);
                ex += 2 * ew + gap;
            }
            rowY += portrait ? 2 * eh + gap : -(2 * eh + gap);
        }
        return t;
    }

    /** No control within r of (x, y). */
    private boolean free(float x, float y, float r) {
        for (Control c : controls) {
            float cr = c.kind == PILL ? Math.max(c.hw, c.r) : c.r;
            float dx = c.cx - x, dy = c.cy - y;
            if (Math.sqrt(dx * dx + dy * dy) < cr + r) {
                return false;
            }
        }
        return true;
    }

    private static boolean isStick(String n) {
        return n.startsWith("Stick ");
    }

    private void dpad(float cx, float cy, float r) {
        if (!has("Up") && !has("Down") && !has("Left") && !has("Right")) {
            return;
        }
        Control d = new Control(DPAD, "dpad", "");
        d.cx = cx;
        d.cy = cy;
        d.r = r;
        d.up = bit("Up");
        d.down = bit("Down");
        d.left = bit("Left");
        d.right = bit("Right");
        controls.add(d);
        placed.addAll(Arrays.asList("Up", "Down", "Left", "Right"));
    }

    /** The face buttons' width (for fitting a narrow screen). */
    private static float faceWidth(String system, List<String> buttons, float r) {
        if (system.equals("segaMD") || system.equals("n64")) return 6.4f * r;
        return 4.6f * r;
    }

    private void face(String system, float cx, float cy, float r, float u) {
        if (system.equals("snes")) { // the diamond: X top, A right, B bottom, Y left
            float d = 1.45f * r;
            button("X", "X", cx, cy - d, r);
            button("A", "A", cx + d, cy, r);
            button("B", "B", cx, cy + d, r);
            button("Y", "Y", cx - d, cy, r);
            return;
        }
        if (system.equals("segaMD")) { // A B C below, X Y Z above, slanting up to the right
            float d = 2.1f * r, s = 0.72f * r;
            button("A", "A", cx - d, cy + 0.9f * r, r);
            button("B", "B", cx, cy + 0.45f * r, r);
            button("C", "C", cx + d, cy, r);
            button("X", "X", cx - d, cy - 1.25f * r, s);
            button("Y", "Y", cx, cy - 1.7f * r, s);
            button("Z", "Z", cx + d, cy - 2.15f * r, s);
            return;
        }
        if (system.equals("n64")) { // A and B big at the left, the C buttons at the right
            button("A", "A", cx - 1.6f * r, cy + 1.0f * r, r * 1.1f);
            button("B", "B", cx - 2.6f * r, cy - 0.6f * r, r * 0.95f);
            float ccx = cx + 1.2f * r, ccy = cy - 0.5f * r, d = 1.15f * r, s = 0.62f * r;
            button("C-Up", labelFor("C-Up"), ccx, ccy - d, s);
            button("C-Down", labelFor("C-Down"), ccx, ccy + d, s);
            button("C-Left", labelFor("C-Left"), ccx - d, ccy, s);
            button("C-Right", labelFor("C-Right"), ccx + d, ccy, s);
            return;
        }
        if (system.equals("atari2600")) {
            button("Fire", "Fire", cx, cy, r * 1.4f);
            return;
        }
        if (system.equals("sms")) {
            pair("1", "1", "2", "2", cx, cy, r);
            return;
        }
        if (system.equals("atari7800")) {
            pair("Button 1", "1", "Button 2", "2", cx, cy, r);
            return;
        }
        if (system.equals("arcade")) {
            List<String> b = new ArrayList<String>();
            for (String n : new String[] {"Fire", "Jump", "Button 1", "Button 2", "Button 3", "Button 4"}) {
                if (has(n)) {
                    b.add(n);
                }
            }
            if (b.size() == 1) {
                button(b.get(0), labelFor(b.get(0)), cx, cy, r * 1.4f);
            } else if (b.size() == 2) {
                pair(b.get(0), labelFor(b.get(0)), b.get(1), labelFor(b.get(1)), cx, cy, r);
            } else if (b.size() >= 3) {
                // 1 2 at the bottom, 3 4 above them
                float d = 1.15f * r;
                float[][] at = {{-d, d * 0.6f}, {d, d * 0.6f}, {-d, -d * 1.4f}, {d, -d * 1.4f}, {0, -d * 3.2f}, {0, d * 2.4f}};
                for (int i = 0; i < b.size() && i < at.length; i++) {
                    button(b.get(i), labelFor(b.get(i)), cx + at[i][0], cy + at[i][1], r);
                }
            }
            return;
        }
        // NES, Game Boy, Game Boy Advance (and anything else with A and B): B low left, A high right
        if (has("A") || has("B")) {
            pair("B", "B", "A", "A", cx, cy, r);
        }
    }

    /** Two buttons side by side, the right one a little higher (the NES, Game Boy, SMS layout). */
    private void pair(String a, String la, String b, String lb, float cx, float cy, float r) {
        float d = 1.2f * r;
        if (has(a)) button(a, la, cx - d, cy + 0.45f * r, r);
        if (has(b)) button(b, lb, cx + d, cy - 0.45f * r, r);
    }

    // ---- reading the fingers ----

    /** The control a finger at (x, y) works, or null. */
    public Control at(float x, float y) {
        Control best = null;
        float bestReach = 1;
        for (Control c : controls) {
            float k = c.reach(x, y);
            if (k <= bestReach) {
                best = c;
                bestReach = k;
            }
        }
        return best;
    }

    /** The D-pad bits for a finger at (x, y) on it (8 ways; the middle is nothing). */
    public static int dpadBits(Control d, float x, float y) {
        float dx = x - d.cx, dy = y - d.cy;
        double dist = Math.sqrt(dx * dx + dy * dy);
        if (dist < d.r * 0.22) {
            return 0;
        }
        double ang = Math.toDegrees(Math.atan2(dy, dx)); // 0 = right, 90 = down
        int bits = 0;
        // each direction covers 112.5 degrees, so the diagonals (22.5 degrees either side) press two
        if (Math.abs(angleDiff(ang, 0)) < 67.5) bits |= bitOf(d.right);
        if (Math.abs(angleDiff(ang, 180)) < 67.5) bits |= bitOf(d.left);
        if (Math.abs(angleDiff(ang, 90)) < 67.5) bits |= bitOf(d.down);
        if (Math.abs(angleDiff(ang, -90)) < 67.5) bits |= bitOf(d.up);
        return bits;
    }

    private static int bitOf(int b) {
        return b >= 0 ? 1 << b : 0;
    }

    private static double angleDiff(double a, double b) {
        double d = (a - b) % 360;
        if (d > 180) d -= 360;
        if (d < -180) d += 360;
        return d;
    }

    /** The stick's position for a finger at (x, y): x and y from -1 to 1 (down is +). */
    public static float[] stickAxes(Control s, float x, float y) {
        float dx = (x - s.cx) / s.r, dy = (y - s.cy) / s.r;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > 1) {
            dx /= len;
            dy /= len;
        }
        return new float[] {dx, dy};
    }
}
