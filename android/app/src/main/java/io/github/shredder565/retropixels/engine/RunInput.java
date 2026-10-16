package io.github.shredder565.retropixels.engine;

import java.nio.charset.Charset;
import java.util.LinkedHashSet;
import java.util.Set;

/**
 * One run request: how many frames, and the controls held for them. The
 * engine turns these into each core's buttons with the system's control map,
 * as RetroPixels Native does. Reused from frame to frame.
 */
public final class RunInput {
    public static final int MAX_PADS = 4;
    private static final Charset UTF8 = Charset.forName("UTF-8");

    public int frames;
    public boolean fast;
    public float volume = 1;
    /** Player 1's buttons held on the touch controls: bit i = the game's button i. */
    public int touchButtons;
    /** Player 1's left stick from the touch controls (N64 stick, Atari paddle). */
    public boolean touchX, touchY;
    public float axisX, axisY;
    /** Controllers in player order: buttons in the standard gamepad numbering, and 4 axes. */
    public int pads;
    public final int[] padButtons = new int[MAX_PADS];
    public final float[][] padAxes = new float[MAX_PADS][4];
    /** Keyboard keys held (or pressed since the last run), as browser key names ("x", "enter"). */
    public final Set<String> keys = new LinkedHashSet<String>();

    private byte[] out = new byte[256];
    private int n;

    public byte[] bytes() {
        return out;
    }

    /** Encodes the request; returns its length in bytes(). */
    public int encode() {
        n = 0;
        u8(Math.max(0, Math.min(16, frames)));
        u8(fast ? 1 : 0);
        f32(volume);
        u32(touchButtons);
        u8((touchX ? 1 : 0) | (touchY ? 2 : 0));
        f32(axisX);
        f32(axisY);
        int np = Math.max(0, Math.min(MAX_PADS, pads));
        u8(np);
        for (int p = 0; p < np; p++) {
            u8(padButtons[p] & 0xff);
            u8((padButtons[p] >>> 8) & 0xff);
            for (int a = 0; a < 4; a++) {
                f32(padAxes[p][a]);
            }
        }
        int nk = Math.min(255, keys.size());
        u8(nk);
        int k = 0;
        for (String key : keys) {
            if (k++ >= nk) {
                break;
            }
            byte[] b = key.getBytes(UTF8);
            int l = Math.min(255, b.length);
            u8(l);
            grow(l);
            System.arraycopy(b, 0, out, n, l);
            n += l;
        }
        return n;
    }

    private void grow(int more) {
        if (n + more > out.length) {
            byte[] b = new byte[(n + more) * 2];
            System.arraycopy(out, 0, b, 0, n);
            out = b;
        }
    }

    private void u8(int v) {
        grow(1);
        out[n++] = (byte) v;
    }

    private void u32(int v) {
        grow(4);
        out[n++] = (byte) v;
        out[n++] = (byte) (v >>> 8);
        out[n++] = (byte) (v >>> 16);
        out[n++] = (byte) (v >>> 24);
    }

    private void f32(float v) {
        if (Float.isNaN(v) || Float.isInfinite(v)) {
            v = 0;
        }
        u32(Float.floatToIntBits(v));
    }
}
