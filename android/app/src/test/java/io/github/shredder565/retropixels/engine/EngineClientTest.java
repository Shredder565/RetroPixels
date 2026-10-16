package io.github.shredder565.retropixels.engine;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import org.junit.Test;

/**
 * Drives the engine built for this computer (the RP_ENGINE environment
 * variable or the rp.engine property names it; the tests are skipped
 * without one) through EngineClient, as the app does, and checks the frames
 * and sound that come back are exactly what the core makes when run on its
 * own (the engine's -reference mode). RP_TEST_ROMS names a folder of free
 * test ROMs (homebrew and test programs) to check each system with.
 */
public class EngineClientTest {
    static String engine() {
        String p = System.getProperty("rp.engine");
        if (p == null || p.isEmpty()) {
            p = System.getenv("RP_ENGINE");
        }
        assumeTrue("no engine (set RP_ENGINE)", p != null && new File(p).canExecute());
        return p;
    }

    static EngineClient start(File dir) throws IOException {
        ProcessBuilder pb = new ProcessBuilder(engine(), "-dir", dir.getPath());
        pb.redirectError(ProcessBuilder.Redirect.INHERIT);
        return EngineClient.start(pb.start());
    }

    static File tempDir() throws IOException {
        File d = File.createTempFile("rptest", "");
        d.delete();
        d.mkdirs();
        d.deleteOnExit();
        return d;
    }

    /** A Master System program that shows a coloured backdrop, beeps and loops. */
    static byte[] smsLoop() {
        byte[] rom = new byte[0x8000];
        int[] prog = {0xf3,
            0x3e, 0x04, 0xd3, 0xbf, 0x3e, 0x80, 0xd3, 0xbf,
            0x3e, 0x60, 0xd3, 0xbf, 0x3e, 0x81, 0xd3, 0xbf,
            0x3e, 0x00, 0xd3, 0xbf, 0x3e, 0xc0, 0xd3, 0xbf,
            0x3e, 0x2a, 0xd3, 0xbe,
            0x3e, 0x30, 0xd3, 0x7f,
            0x18, 0xfe};
        for (int i = 0; i < prog.length; i++) {
            rom[i] = (byte) prog[i];
        }
        byte[] tmr = "TMR SEGA".getBytes();
        System.arraycopy(tmr, 0, rom, 0x7ff0, tmr.length);
        return rom;
    }

    static String sha1(byte[] b, int n) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-1");
        md.update(b, 0, n);
        StringBuilder s = new StringBuilder();
        for (byte x : md.digest()) {
            s.append(String.format("%02x", x & 0xff));
        }
        return s.toString();
    }

    /** What the core makes on its own: the engine's -reference mode. */
    static Map<String, Object> reference(String rom, int frames, int rate) throws Exception {
        Process p = new ProcessBuilder(engine(), "-reference", rom, "-frames", "" + frames, "-rate", "" + rate)
                .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        InputStream in = p.getInputStream();
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] b = new byte[4096];
        int n;
        while ((n = in.read(b)) > 0) {
            out.write(b, 0, n);
        }
        assertEquals(0, p.waitFor());
        return Json.parseObject(out.toString("UTF-8").trim());
    }

    /** Runs frames one at a time through the client; returns [pixels sha, audio sha, samples, w, h]. */
    static Object[] play(EngineClient c, int frames) throws Exception {
        RunInput in = new RunInput();
        in.frames = 1;
        in.volume = 1;
        Frame f = null;
        MessageDigest audio = MessageDigest.getInstance("SHA-1");
        int samples = 0;
        byte[] le = new byte[0];
        for (int i = 0; i < frames; i++) {
            f = c.run(in, f);
            assertFalse(f.message, f.failed);
            assertTrue(f.hasPicture);
            if (le.length < f.sampleCount * 2) {
                le = new byte[f.sampleCount * 2];
            }
            for (int k = 0; k < f.sampleCount; k++) {
                le[k * 2] = (byte) f.samples[k];
                le[k * 2 + 1] = (byte) (f.samples[k] >> 8);
            }
            audio.update(le, 0, f.sampleCount * 2);
            samples += f.sampleCount;
        }
        StringBuilder s = new StringBuilder();
        for (byte x : audio.digest()) {
            s.append(String.format("%02x", x & 0xff));
        }
        return new Object[] {sha1(f.pixels, f.width * f.height * 4), s.toString(), samples, f.width, f.height};
    }

    static File write(File dir, String name, byte[] data) throws IOException {
        File f = new File(dir, name);
        FileOutputStream o = new FileOutputStream(f);
        o.write(data);
        o.close();
        return f;
    }

    @Test
    public void framesAndSoundMatchTheCore() throws Exception {
        File dir = tempDir();
        File rom = write(dir, "Loop.sms", smsLoop());
        EngineClient c = start(dir);
        try {
            Map<String, Object> g = c.must(Json.obj("cmd", "open", "path", rom.getPath(), "sampleRate", 44100));
            assertEquals("sms", Json.str(g, "system", ""));
            assertEquals("Master System", Json.str(g, "label", ""));
            assertEquals(4.0 / 3, Json.num(g, "aspect", 0), 1e-9);
            Object[] got = play(c, 120);
            Map<String, Object> ref = reference(rom.getPath(), 120, 44100);
            assertEquals("picture", ref.get("pixels"), got[0]);
            assertEquals("sound", ref.get("audio"), got[1]);
            assertEquals(Json.integer(ref, "samples", -1), got[2]);
            assertTrue((Integer) got[2] > 120 * 44100 / 60 * 2 * 9 / 10);
            assertEquals(256, got[3]);
            assertEquals(192, got[4]);

            // a save state, a battery save's absence, an unknown command
            assertTrue(Json.bool(c.call(Json.obj("cmd", "save", "slot", 0)), "ok"));
            assertTrue(new File(dir, "saves/sms/Loop.state").exists());
            assertTrue(Json.bool(c.call(Json.obj("cmd", "load", "slot", 0)), "ok"));
            assertFalse(Json.bool(c.call(Json.obj("cmd", "exportSRAM", "path", new File(dir, "x").getPath())), "ok"));
            assertFalse(Json.bool(c.call(Json.obj("cmd", "nope")), "ok"));

            // the controls: player 1's keys and touch buttons, a pad; fast-forward
            RunInput in = new RunInput();
            in.frames = 4;
            in.fast = true;
            in.volume = 0.5f;
            in.touchButtons = 1 << 4;
            in.keys.add("arrowup");
            in.pads = 2;
            in.padButtons[1] = 1;
            in.padAxes[1][0] = 0.9f;
            Frame f = c.run(in, null);
            assertFalse(f.failed);
            assertTrue(f.sampleCount > 0 && f.sampleCount <= 44100 / 60 * 2 + 8);
            c.must(Json.obj("cmd", "close"));
            c.must(Json.obj("cmd", "quit"));
        } finally {
            c.close();
        }
    }

    @Test
    public void chipProgramsAreAskedFor() throws Exception {
        File dir = tempDir();
        byte[] r = new byte[0x8000];
        r[0] = (byte) 0x80;
        r[1] = (byte) 0xfe;
        int h = 0x7fc0;
        byte[] title = "SD RP GX TEST        ".getBytes();
        System.arraycopy(title, 0, r, h, title.length);
        r[h + 0x15] = 0x20;
        r[h + 0x16] = 0x03;
        r[h + 0x17] = 0x05;
        r[h + 0x19] = 1;
        r[0x7ffd] = (byte) 0x80;
        r[h + 0x1c] = (byte) 0xff;
        r[h + 0x1d] = (byte) 0xff;
        File rom = write(dir, "chip.sfc", r);
        EngineClient c = start(dir);
        try {
            Map<String, Object> rep = c.call(Json.obj("cmd", "open", "path", rom.getPath()));
            assertFalse(Json.bool(rep, "ok"));
            assertEquals("firmware", rep.get("need"));
            assertEquals("DSP-3", rep.get("chip"));
            assertEquals("dsp3.rom", rep.get("file"));
            try {
                c.must(Json.obj("cmd", "open", "path", rom.getPath()));
                assertTrue("must should throw", false);
            } catch (EngineClient.EngineError e) {
                assertEquals("firmware", e.reply.get("need"));
            }
        } finally {
            c.close();
        }
    }

    @Test
    public void theEngineGoingAwayIsAnError() throws Exception {
        File dir = tempDir();
        EngineClient c = start(dir);
        c.must(Json.obj("cmd", "quit"));
        try {
            c.call(Json.obj("cmd", "systems"));
            assertTrue("no error", false);
        } catch (IOException e) {
            assertFalse(e instanceof EngineClient.EngineError);
        }
        c.close();
    }

    /** Every system's free test ROM, when RP_TEST_ROMS has them. */
    @Test
    public void testROMs() throws Exception {
        String root = System.getenv("RP_TEST_ROMS");
        assumeTrue("RP_TEST_ROMS not set", root != null);
        String[] roms = {
            "nes/nes-test-roms/other/nestest.nes",
            "chips/SNES/CHIP/GSU/GSUTest/CMP/GSUCMP.sfc",
            "gbgba/retrio_gb-test-roms/cpu_instrs/cpu_instrs.gb",
            "gbgba/alyosha-tas_gba-tests/arm/arm.gba",
            "segaatari/rb-md-games/shatteringjaws.md",
            "segaatari/rb-sms-games/shootingstars.sms",
            "segaatari/rb-atari2600-games/hauntedbakery.bin",
            "../rel/bas_adventurer.a78",
            "../rel/n64niccc.z64",
        };
        File dir = tempDir();
        EngineClient c = start(dir);
        List<String> done = new ArrayList<String>();
        try {
            for (String r : roms) {
                File f = new File(root, r);
                if (!f.exists()) {
                    continue;
                }
                Map<String, Object> g = c.must(Json.obj("cmd", "open", "path", f.getPath(), "sampleRate", 48000));
                long t0 = System.nanoTime();
                Object[] got = play(c, 240);
                double ms = (System.nanoTime() - t0) / 1e6 / 240;
                Map<String, Object> ref = reference(f.getPath(), 240, 48000);
                assertEquals(r + " picture", ref.get("pixels"), got[0]);
                assertEquals(r + " sound", ref.get("audio"), got[1]);
                done.add(String.format("%-10s %-22s %dx%d  %.2f ms per frame through the client", g.get("system"), f.getName(), got[3], got[4], ms));
            }
        } finally {
            c.close();
        }
        for (String s : done) {
            System.out.println(s);
        }
        assertFalse(done.isEmpty());
    }
}
