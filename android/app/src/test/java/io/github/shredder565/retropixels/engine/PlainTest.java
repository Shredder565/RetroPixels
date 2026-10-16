package io.github.shredder565.retropixels.engine;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import org.junit.Test;

/** The parts that need no engine: JSON, the pacer, the picture's place, the touch controls. */
public class PlainTest {
    @Test
    public void json() {
        Map<String, Object> m = Json.parseObject("{\"a\":1,\"b\":[true,false,null,\"x\\n\\u00e9\"],\"c\":{\"d\":-2.5e1}}");
        assertEquals(1.0, m.get("a"));
        assertEquals(Arrays.asList(true, false, null, "x\né"), m.get("b"));
        assertEquals(-25.0, Json.num(Json.obj(m, "c"), "d", 0), 0);
        assertEquals("{\"a\":1,\"b\":[true,false,null,\"x\\né\"],\"c\":{\"d\":-25}}", Json.write(m));
        Map<String, Object> o = Json.obj("cmd", "open", "n", 3, "f", 0.5, "l", new int[] {1, 2});
        assertEquals("{\"cmd\":\"open\",\"n\":3,\"f\":0.5,\"l\":[1,2]}", Json.write(o));
        assertEquals(Json.write(o), Json.write(Json.parse(Json.write(o))));
        try {
            Json.parse("{\"a\":}");
            assertTrue(false);
        } catch (IllegalArgumentException expected) {
            // ok
        }
    }

    @Test
    public void pacerFollowsTheClockAndTheSound() {
        Pacer p = new Pacer(60, 0);
        int frames = 0;
        for (int i = 0; i < 1000; i++) { // 4 s in 4 ms steps, the sound queue in the band
            frames += p.step(0.004, 0.08, false);
        }
        assertEquals(240, frames, 1);
        frames = 0;
        for (int i = 0; i < 1000; i++) { // the sound running low: hurry (1.5x)
            frames += p.step(0.004, 0.01, false);
        }
        assertEquals(360, frames, 2);
        frames = 0;
        for (int i = 0; i < 1000; i++) { // too much queued: slow down (0.5x)
            frames += p.step(0.004, 0.3, false);
        }
        assertEquals(120, frames, 2);
        assertEquals(Pacer.FAST_FRAMES, p.step(0.016, 0.08, true));
        assertTrue(p.step(5, 0.08, false) <= Pacer.MAX_FRAMES); // a stall doesn't make a burst
        // a small sound buffer narrows the band so it can be kept
        Pacer small = new Pacer(60, 0.1);
        frames = 0;
        for (int i = 0; i < 1000; i++) {
            frames += small.step(0.004, 0.09, false);
        }
        assertEquals(120, frames, 2);
    }

    @Test
    public void fit() {
        Fit f = Fit.fit(1920, 1080, 4.0 / 3, false);
        assertEquals(1440, f.w);
        assertEquals(1080, f.h);
        assertEquals(240, f.x);
        assertEquals(0, f.y);
        f = Fit.fit(1080, 2400, 10.0 / 9, true);
        assertEquals(1080, f.w);
        assertEquals(972, f.h);
        assertEquals(0, f.y);
        f = Fit.fit(1080, 2400, 10.0 / 9, false);
        assertEquals((2400 - 972) / 2, f.y);
    }

    static final Map<String, List<String>> SYSTEMS = new LinkedHashMap<String, List<String>>();

    static {
        SYSTEMS.put("nes", Arrays.asList("A", "B", "Select", "Start", "Up", "Down", "Left", "Right"));
        SYSTEMS.put("snes", Arrays.asList("R", "L", "X", "A", "Right", "Left", "Down", "Up", "Start", "Select", "Y", "B"));
        SYSTEMS.put("gb", Arrays.asList("A", "B", "Select", "Start", "Up", "Down", "Left", "Right", "Tilt Up", "Tilt Down", "Tilt Left", "Tilt Right"));
        SYSTEMS.put("gba", Arrays.asList("A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"));
        SYSTEMS.put("n64", Arrays.asList("A", "B", "Z", "Start", "Up", "Down", "Left", "Right", "L", "R",
                "C-Up", "C-Down", "C-Left", "C-Right", "Stick Up", "Stick Down", "Stick Left", "Stick Right"));
        SYSTEMS.put("segaMD", Arrays.asList("A", "B", "C", "Start", "X", "Y", "Z", "Mode", "Up", "Down", "Left", "Right"));
        SYSTEMS.put("sms", Arrays.asList("Up", "Down", "Left", "Right", "1", "2", "Pause", "Reset"));
        SYSTEMS.put("atari2600", Arrays.asList("Up", "Down", "Left", "Right", "Fire", "Select", "Reset", "Left difficulty", "Right difficulty", "Color / B&W"));
        SYSTEMS.put("atari7800", Arrays.asList("Up", "Down", "Left", "Right", "Button 1", "Button 2", "Pause", "Select", "Reset", "Left difficulty", "Right difficulty"));
        SYSTEMS.put("arcade", Arrays.asList("Up", "Down", "Left", "Right", "Jump", "Coin", "Start (1P)", "Start (2P)"));
        SYSTEMS.put("arcade4", Arrays.asList("Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Coin", "Start (1P)", "Service"));
    }

    @Test
    public void touchControlsForEverySystem() {
        int[][] screens = {{2400, 1080}, {1080, 2400}, {1280, 800}, {800, 1280}, {1920, 1200}};
        for (Map.Entry<String, List<String>> e : SYSTEMS.entrySet()) {
            String sys = e.getKey().startsWith("arcade") ? "arcade" : e.getKey();
            for (int[] s : screens) {
                int w = s[0], h = s[1];
                float dp = Math.min(w, h) / 400f;
                int picBottom = h > w ? (int) (w / (4.0 / 3)) : 0;
                TouchLayout t = TouchLayout.make(sys, e.getValue(), w, h, dp, 1, picBottom);
                String where = e.getKey() + " " + w + "x" + h;
                for (String b : e.getValue()) {
                    assertTrue(where + ": no control for " + b, t.placed.contains(b) || b.startsWith("Stick "));
                }
                for (TouchLayout.Control c : t.controls) {
                    float hw = c.kind == TouchLayout.PILL ? c.hw : c.r;
                    assertTrue(where + ": " + c.name + " off the screen", c.cx - hw >= 0 && c.cx + hw <= w && c.cy - c.r >= 0 && c.cy + c.r <= h);
                    if (c.kind == TouchLayout.BUTTON || c.kind == TouchLayout.PILL) {
                        assertTrue(where + ": " + c.name + " has no bit", c.bit >= 0);
                        assertEquals(c.name, e.getValue().get(c.bit));
                    }
                    // a finger on the middle of a control works that control
                    if (c.kind != TouchLayout.DPAD || c.r > 0) {
                        assertSame(where + ": " + c.name, c, t.at(c.cx, c.cy));
                    }
                    for (TouchLayout.Control d : t.controls) {
                        if (c == d) {
                            continue;
                        }
                        float dx = c.cx - d.cx, dy = c.cy - d.cy;
                        float ra = c.kind == TouchLayout.PILL ? Math.min(c.hw, c.r) : c.r;
                        float rb = d.kind == TouchLayout.PILL ? Math.min(d.hw, d.r) : d.r;
                        assertTrue(where + ": " + c.name + " on top of " + d.name, Math.sqrt(dx * dx + dy * dy) > (ra + rb) * 0.9);
                    }
                }
            }
        }
    }

    @Test
    public void dpadAndStick() {
        List<String> b = SYSTEMS.get("nes");
        TouchLayout t = TouchLayout.make("nes", b, 2400, 1080, 2.7f, 1, 0);
        TouchLayout.Control d = null;
        for (TouchLayout.Control c : t.controls) {
            if (c.kind == TouchLayout.DPAD) {
                d = c;
            }
        }
        int up = 1 << b.indexOf("Up"), down = 1 << b.indexOf("Down"), left = 1 << b.indexOf("Left"), right = 1 << b.indexOf("Right");
        float r = d.r * 0.8f;
        assertEquals(0, TouchLayout.dpadBits(d, d.cx, d.cy));
        assertEquals(right, TouchLayout.dpadBits(d, d.cx + r, d.cy));
        assertEquals(left, TouchLayout.dpadBits(d, d.cx - r, d.cy));
        assertEquals(up, TouchLayout.dpadBits(d, d.cx, d.cy - r));
        assertEquals(down, TouchLayout.dpadBits(d, d.cx, d.cy + r));
        assertEquals(up | right, TouchLayout.dpadBits(d, d.cx + r * 0.7f, d.cy - r * 0.7f));
        assertEquals(down | left, TouchLayout.dpadBits(d, d.cx - r * 0.7f, d.cy + r * 0.7f));
        assertNull(t.at(1200, 540)); // the middle of the screen is the picture

        TouchLayout n = TouchLayout.make("n64", SYSTEMS.get("n64"), 2400, 1080, 2.7f, 1, 0);
        TouchLayout.Control s = null;
        for (TouchLayout.Control c : n.controls) {
            if (c.kind == TouchLayout.STICK) {
                s = c;
            }
        }
        float[] ax = TouchLayout.stickAxes(s, s.cx + s.r * 2, s.cy);
        assertEquals(1, ax[0], 1e-6);
        assertEquals(0, ax[1], 1e-6);
        ax = TouchLayout.stickAxes(s, s.cx, s.cy - s.r / 2);
        assertEquals(-0.5, ax[1], 1e-6);
    }
}
