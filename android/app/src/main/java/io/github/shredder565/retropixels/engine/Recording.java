package io.github.shredder565.retropixels.engine;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.regex.Pattern;

/**
 * The plain parts of recording a game (Settings > Record games
 * automatically), as RetroPixels Native's recorder.go: the file's name, the
 * video's size and the recording's clock. No Android classes, so the tests
 * check them on the computer; GameRecorder does the encoding.
 */
public final class Recording {
    /** A recording shorter than this (seconds of game) isn't kept: the game was closed straight away (minRecording). */
    public static final double MIN_SECONDS = 5.0;

    private static final Pattern UNSAFE = Pattern.compile("[<>:\"/\\\\|?*\\x00-\\x1f]+");

    private Recording() {
    }

    /** A game's name made usable as a file name (games.SafeName). */
    public static String safeName(String s) {
        if (s == null) {
            s = "";
        }
        s = UNSAFE.matcher(s).replaceAll("");
        int end = s.length();
        while (end > 0 && (s.charAt(end - 1) == '.' || s.charAt(end - 1) == ' ')) {
            end--;
        }
        s = s.substring(0, end).trim();
        String first = s.split("\\.", 2)[0].toUpperCase(Locale.ROOT);
        switch (first) {
            case "CON": case "PRN": case "AUX": case "NUL": case "COM1": case "COM2": case "COM3": case "COM4":
            case "LPT1": case "LPT2": case "LPT3":
                s = "_" + s;
                break;
            default:
                break;
        }
        if (s.isEmpty()) {
            return "game";
        }
        if (s.codePointCount(0, s.length()) > 120) {
            s = s.substring(0, s.offsetByCodePoints(0, 120));
        }
        return s;
    }

    /** "<game> <date time>", without ".mp4": "Super Mario World 2026-10-23 18-04-05", as on Windows. */
    public static String baseName(String game, Date when) {
        return safeName(game) + " " + new SimpleDateFormat("yyyy-MM-dd HH-mm-ss", Locale.ROOT).format(when);
    }

    /** Says whether a file name is taken. */
    public interface Taken {
        boolean taken(String fileName);
    }

    /** base + ".mp4", or "base (2).mp4", "base (3).mp4"... when that's taken (uniqueOut). */
    public static String uniqueFile(String base, Taken t) {
        String name = base + ".mp4";
        for (int n = 2; t != null && t.taken(name) && n < 10000; n++) {
            name = base + " (" + n + ").mp4";
        }
        return name;
    }

    // ---------------- the video's size ----------------

    /**
     * The video's size (recordSize in RetroPixels Native, recordingCanvas in
     * the web player): the picture scaled up by a whole number to about 720
     * lines, at the game's shape, in even sizes (odd sizes are rounded down,
     * so a 255 x 223 picture is fine: it is scaled into the video).
     */
    public static int[] videoSize(int h, double aspect) {
        if (h <= 0) {
            h = 240;
        }
        if (!(aspect > 0) || Double.isInfinite(aspect)) {
            aspect = 4.0 / 3;
        }
        int scale = (int) Math.max(1, Math.floor(720.0 / h + 0.5)); // Go's math.Round for positives
        int rh = (h * scale) & ~1;
        int rw = ((int) Math.floor(rh * aspect + 0.5)) & ~1;
        return new int[] {Math.max(2, rw), Math.max(2, rh)};
    }

    /**
     * Fits a size to what the phone's encoder takes: no bigger than maxW x
     * maxH (keeping the shape), then rounded down to the encoder's
     * alignments (at least 2: H.264 needs even sizes) and no smaller than
     * the smallest it takes.
     */
    public static int[] fit(int w, int h, int alignW, int alignH, int minW, int minH, int maxW, int maxH) {
        alignW = Math.max(2, alignW);
        alignH = Math.max(2, alignH);
        if (maxW > 0 && w > maxW) {
            h = (int) ((long) h * maxW / w);
            w = maxW;
        }
        if (maxH > 0 && h > maxH) {
            w = (int) ((long) w * maxH / h);
            h = maxH;
        }
        w = Math.max(w - w % alignW, alignW);
        h = Math.max(h - h % alignH, alignH);
        if (w < minW) {
            w = minW + (alignW - minW % alignW) % alignW;
        }
        if (h < minH) {
            h = minH + (alignH - minH % alignH) % alignH;
        }
        return new int[] {w, h};
    }

    /** The video's bit rate: enough for sharp pixels, 2 to 16 Mbit/s. */
    public static int bitRate(int w, int h, double fps) {
        double r = w * (double) h * Math.max(24, Math.min(60, fps)) * 0.12;
        return (int) Math.max(2000000, Math.min(16000000, r));
    }

    // ---------------- the clock ----------------

    /**
     * The recording's clock, the game's own (not the phone's), so the video
     * plays at the game's speed whatever the phone does, and pausing (the
     * menu) leaves no gap. A run of n frames moves it on n frames; while
     * fast-forwarding the engine only sends the first frame's sound, so the
     * clock moves on one frame and the video shows the game sped up, with the
     * sound in step. The sound's own time comes from the samples written; if
     * it drifts more than SLACK from the picture's, silence is added or
     * samples are left out so the two stay together.
     */
    public static final class Clock {
        public static final double SLACK = 0.1; // seconds

        private final double fps;
        private final int rate;
        private long frames; // game frames on the recording's clock
        private long samples; // stereo sample pairs written
        private long lastVideoUs = -1;

        public Clock(double fps, int rate) {
            this.fps = fps > 0 && fps < 1000 ? fps : 60;
            this.rate = rate > 0 ? rate : 48000;
        }

        /**
         * A run of n frames: returns the presentation time of its picture, in
         * microseconds, always later than the last one's.
         */
        public long video(int n, boolean fast) {
            frames += fast ? 1 : Math.max(1, n);
            long us = (long) Math.floor(frames * 1e6 / fps + 0.5);
            if (us <= lastVideoUs) {
                us = lastVideoUs + 1;
            }
            lastVideoUs = us;
            return us;
        }

        /** The run's frames, for a run without a picture. */
        public void skip(int n, boolean fast) {
            frames += fast ? 1 : Math.max(0, n);
        }

        /** Seconds of game on the clock. */
        public double seconds() {
            return frames / fps;
        }

        /** Where the next sample pair goes, in sample pairs from the start. */
        public long audioPairs() {
            return samples;
        }

        /** Where the next sample pair goes, in microseconds. */
        public long audioUs() {
            return samples * 1000000L / rate;
        }

        /**
         * pairs sample pairs are about to be written after a run (the clock
         * already moved on by it): returns how many of them to write (fewer
         * when the sound is ahead), and silencePairs() then says how many
         * silent pairs go first (when it is behind).
         */
        public int audio(int pairs) {
            silence = 0;
            long want = (long) Math.floor(frames * rate / fps + 0.5); // where the sound should be after this run
            long slack = (long) (SLACK * rate);
            long after = samples + pairs;
            if (after < want - slack) {
                silence = (int) Math.min(want - after, rate); // at most a second at a time
            } else if (after > want + slack) {
                pairs = (int) Math.max(0, pairs - (after - want));
            }
            samples += silence + pairs;
            return pairs;
        }

        private int silence;

        public int silencePairs() {
            return silence;
        }
    }
}
