package io.github.shredder565.retropixels.engine;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.util.Arrays;
import java.util.Calendar;
import java.util.HashSet;
import java.util.Set;

import org.junit.Test;

/** Recording's plain parts: the file's name, the video's size and the clock. */
public class RecordingTest {
    @Test
    public void names() {
        assertEquals("Super Mario World", Recording.safeName("Super Mario World"));
        assertEquals("Zelda Link's Awakening DX", Recording.safeName("Zelda: Link's <Awakening> DX?"));
        assertEquals("ab", Recording.safeName("a/b|\u0001"));
        assertEquals("Game", Recording.safeName("Game. . "));
        assertEquals("_CON", Recording.safeName("CON"));
        assertEquals("_nul.x", Recording.safeName("nul.x"));
        assertEquals("game", Recording.safeName(":::"));
        assertEquals("game", Recording.safeName(null));
        char[] longName = new char[200];
        Arrays.fill(longName, 'x');
        assertEquals(120, Recording.safeName(new String(longName)).length());

        Calendar c = Calendar.getInstance();
        c.clear();
        c.set(2026, Calendar.OCTOBER, 23, 9, 4, 5);
        assertEquals("Pac-Man 2026-10-23 09-04-05", Recording.baseName("Pac-Man", c.getTime()));
        c.set(2026, Calendar.JANUARY, 2, 18, 30, 0);
        assertEquals("TMNT 2026-01-02 18-30-00", Recording.baseName("TMNT", c.getTime()));

        final Set<String> taken = new HashSet<String>();
        Recording.Taken t = new Recording.Taken() {
            public boolean taken(String n) {
                return taken.contains(n);
            }
        };
        assertEquals("A 1.mp4", Recording.uniqueFile("A 1", t));
        taken.add("A 1.mp4");
        assertEquals("A 1 (2).mp4", Recording.uniqueFile("A 1", t));
        taken.add("A 1 (2).mp4");
        assertEquals("A 1 (3).mp4", Recording.uniqueFile("A 1", t));
        assertEquals("A 1.mp4", Recording.uniqueFile("A 1", null));
    }

    @Test
    public void sizes() {
        // as RetroPixels Native's recordSize: ~720 lines, a whole multiple, even
        assertArrayEquals(new int[] {960, 720}, Recording.videoSize(240, 4.0 / 3)); // NES, N64
        assertArrayEquals(new int[] {896, 672}, Recording.videoSize(224, 4.0 / 3)); // SNES, Genesis
        assertArrayEquals(new int[] {800, 720}, Recording.videoSize(144, 10.0 / 9)); // Game Boy
        assertArrayEquals(new int[] {1200, 800}, Recording.videoSize(160, 1.5)); // GBA: 4.5 rounds up, as Go's math.Round
        // odd sizes come out even
        int[] odd = Recording.videoSize(223, 1.37);
        assertEquals(0, odd[0] % 2);
        assertEquals(0, odd[1] % 2);
        assertArrayEquals(new int[] {914, 668}, odd); // 223 x 3 = 669 -> 668 lines, 915 -> 914 wide
        assertArrayEquals(new int[] {1000, 750}, Recording.videoSize(375, 4.0 / 3)); // 2x: 750 lines
        assertArrayEquals(new int[] {1280, 960}, Recording.videoSize(480, 4.0 / 3)); // 1.5 rounds up to 2x
        assertArrayEquals(new int[] {800, 600}, Recording.videoSize(600, 4.0 / 3)); // already big: 1x
        assertArrayEquals(new int[] {960, 720}, Recording.videoSize(0, Double.NaN)); // unknown: 240 lines, 4:3
        int[] tiny = Recording.videoSize(1, 1);
        assertTrue(tiny[0] >= 2 && tiny[1] >= 2);

        // fitted to an encoder
        assertArrayEquals(new int[] {960, 720}, Recording.fit(960, 720, 2, 2, 0, 0, 1920, 1088));
        assertArrayEquals(new int[] {896, 672}, Recording.fit(900, 675, 16, 16, 0, 0, 1920, 1088)); // aligned down to 16
        assertArrayEquals(new int[] {1920, 1080}, Recording.fit(3840, 2160, 2, 2, 0, 0, 1920, 1088)); // too big: shape kept
        assertArrayEquals(new int[] {1280, 720}, Recording.fit(1920, 1080, 2, 2, 0, 0, 1280, 720));
        assertArrayEquals(new int[] {176, 144}, Recording.fit(100, 80, 2, 2, 176, 144, 1920, 1088)); // too small: the least
        assertArrayEquals(new int[] {960, 722}, Recording.fit(961, 723, 1, 1, 0, 0, 0, 0)); // never odd, even for "any size"
    }

    @Test
    public void bitRate() {
        assertEquals(4976640, Recording.bitRate(960, 720, 60));
        assertEquals(2000000, Recording.bitRate(160, 144, 60));
        assertEquals(14929920, Recording.bitRate(1920, 1080, 120)); // counted as 60 fps
        assertEquals(16000000, Recording.bitRate(4096, 2160, 60));
    }

    @Test
    public void clockFollowsTheGame() {
        Recording.Clock c = new Recording.Clock(60, 48000);
        long last = -1;
        for (int i = 0; i < 600; i++) { // ten seconds, one frame per run
            long us = c.video(1, false);
            assertTrue(us > last);
            last = us;
            assertEquals(800, c.audio(800));
            assertEquals(0, c.silencePairs());
        }
        assertEquals(10000000, last);
        assertEquals(10.0, c.seconds(), 1e-9);
        assertEquals(480000, c.audioPairs());
        assertEquals(10000000, c.audioUs());

        // several frames in one run: the clock moves on all of them
        c.video(3, false);
        assertEquals(2400, c.audio(2400));
        assertEquals(10.05, c.seconds(), 1e-9);
    }

    @Test
    public void clockOddRates() {
        // 60.0988 fps (NES) at 44100 Hz: the sound stays with the picture over an hour
        Recording.Clock c = new Recording.Clock(60.0988, 44100);
        double acc = 0;
        long last = -1;
        for (int i = 0; i < 60 * 60 * 60; i++) {
            acc += 44100 / 60.0988;
            int pairs = (int) acc; // what the engine would send
            acc -= pairs;
            long us = c.video(1, false);
            assertTrue(us > last);
            last = us;
            assertEquals(pairs, c.audio(pairs));
            assertEquals(0, c.silencePairs());
        }
        assertEquals(c.audioUs() / 1e6, last / 1e6, 0.05);
    }

    @Test
    public void clockFastForwardAndGaps() {
        Recording.Clock c = new Recording.Clock(60, 48000);
        c.video(1, false);
        c.audio(800);
        // fast-forward: four frames run, one frame of sound comes, the clock moves on one frame
        long before = c.video(1, false);
        c.audio(800);
        long fast = c.video(4, true);
        assertEquals(before + 16667, fast, 1);
        assertEquals(800, c.audio(800));
        assertEquals(0, c.silencePairs());

        // runs with no sound (or a picture without a run's sound): silence fills in once it is behind
        int silence = 0;
        for (int i = 0; i < 12; i++) {
            c.video(1, false);
            c.audio(0);
            silence += c.silencePairs();
        }
        assertTrue(silence > 0);
        assertEquals(c.seconds() * 48000, c.audioPairs(), 48000 * Recording.Clock.SLACK);

        // too much sound: the extra is left out
        Recording.Clock d = new Recording.Clock(60, 48000);
        d.video(1, false);
        int kept = d.audio(48000);
        assertTrue(kept < 48000);
        assertEquals(d.seconds() * 48000, d.audioPairs(), 48000 * Recording.Clock.SLACK);

        // a run without a picture still moves the clock
        Recording.Clock e = new Recording.Clock(50, 48000);
        e.skip(2, false);
        assertEquals(60000, e.video(1, false));
        assertEquals(0.06, e.seconds(), 1e-9);
    }
}
