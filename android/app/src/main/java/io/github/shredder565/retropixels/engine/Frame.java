package io.github.shredder565.retropixels.engine;

import java.nio.charset.Charset;

/**
 * What a run brought back: the picture (RGBA bytes, ready for an Android
 * ARGB_8888 bitmap's copyPixelsFromBuffer), the sound (s16 stereo) and the
 * rumble. The arrays are reused from frame to frame.
 */
public final class Frame {
    private static final Charset UTF8 = Charset.forName("UTF-8");

    /** The emulator stopped; message says why. */
    public boolean failed;
    public int width, height;
    public float rumble;
    /** Something to show the player, or "". */
    public String message = "";
    public boolean hasPicture;
    public byte[] pixels = new byte[0]; // width * height * 4 bytes when hasPicture
    public short[] samples = new short[0];
    public int sampleCount; // s16 values in samples (stereo pairs * 2)

    void decode(byte[] b, int off, int len) throws java.io.IOException {
        int end = off + len;
        int p = off;
        if (len < 14) {
            throw new java.io.IOException("short frame from the engine");
        }
        failed = b[p++] == 1;
        width = u16(b, p);
        height = u16(b, p + 2);
        p += 4;
        rumble = Float.intBitsToFloat(u32(b, p));
        p += 4;
        int flags = b[p++] & 0xff;
        int ml = u16(b, p);
        p += 2;
        message = ml == 0 ? "" : new String(b, p, ml, UTF8);
        p += ml;
        sampleCount = u32(b, p);
        p += 4;
        if (sampleCount < 0 || p + sampleCount * 2 > end) {
            throw new java.io.IOException("bad sound length from the engine");
        }
        if (samples.length < sampleCount) {
            samples = new short[sampleCount + sampleCount / 2];
        }
        for (int i = 0; i < sampleCount; i++, p += 2) {
            samples[i] = (short) ((b[p] & 0xff) | (b[p + 1] << 8));
        }
        hasPicture = (flags & 1) != 0;
        if (hasPicture) {
            int n = width * height * 4;
            if (p + n > end) {
                throw new java.io.IOException("short picture from the engine");
            }
            if (pixels.length < n) {
                pixels = new byte[n];
            }
            System.arraycopy(b, p, pixels, 0, n);
        }
    }

    private static int u16(byte[] b, int p) {
        return (b[p] & 0xff) | (b[p + 1] & 0xff) << 8;
    }

    private static int u32(byte[] b, int p) {
        return (b[p] & 0xff) | (b[p + 1] & 0xff) << 8 | (b[p + 2] & 0xff) << 16 | (b[p + 3] & 0xff) << 24;
    }
}
