package io.github.shredder565.retropixels.engine;

/**
 * Decides how many frames to run, as RetroPixels Native's pacer (pacing.go)
 * and the web player's Player.loop: frames follow the clock, and run a little
 * early or late to keep the sound waiting to be played in a band (35-140 ms
 * there; narrower when the phone's sound buffer is small), so the sound
 * card's clock is the one that sets the speed.
 */
public final class Pacer {
    public static final int MAX_FRAMES = 4; // most frames run in one go
    public static final int FAST_FRAMES = 4; // frames per step while fast-forwarding
    private static final double MAX_STEP = 0.1; // a longer gap (a stall) counts as this

    private final double frameTime;
    private final double low, high;
    private double acc;

    /** bufferSeconds: how much sound the output can hold (0 = plenty). */
    public Pacer(double fps, double bufferSeconds) {
        if (!(fps > 0)) {
            fps = 60;
        }
        frameTime = 1 / fps;
        double lo = 0.035, hi = 0.14;
        if (bufferSeconds > 0 && hi > bufferSeconds * 0.7) {
            hi = bufferSeconds * 0.7;
            lo = Math.min(lo, hi / 3);
        }
        low = lo;
        high = hi;
    }

    public double frameTime() {
        return frameTime;
    }

    /**
     * dt seconds have passed; queued is the sound waiting to be played, in
     * seconds (negative when there is no sound output: then only the clock
     * counts). Returns the frames to run now.
     */
    public int step(double dt, double queued, boolean fast) {
        if (dt > MAX_STEP) {
            dt = MAX_STEP;
        }
        if (dt < 0) {
            dt = 0;
        }
        if (fast) {
            acc = 0;
            return FAST_FRAMES;
        }
        acc += dt;
        if (queued >= 0) {
            if (queued < low) {
                acc += dt * 0.5;
            } else if (queued > high) {
                acc -= dt * 0.5;
            }
        }
        int n = 0;
        while (acc >= frameTime && n < MAX_FRAMES) {
            n++;
            acc -= frameTime;
        }
        if (acc > frameTime * MAX_FRAMES) {
            acc = 0;
        }
        return n;
    }

    /** Seconds until the next frame is due. */
    public double untilNext() {
        double d = frameTime - acc;
        return d < 0 ? 0 : d;
    }

    public void reset() {
        acc = 0;
    }
}
