package io.github.shredder565.retropixels.engine;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.io.FileOutputStream;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

import org.junit.Test;

/** Star Wars Trilogy Arcade's ROM set checks, command line and Supermodel.ini (as internal/supermodel). */
public class StarWarsTest {
    private static Set<Long> crcs(StarWars.RomSet s, boolean optional) {
        Set<Long> out = new HashSet<Long>();
        for (StarWars.Region r : StarWars.merged(s)) {
            if (r.required || optional) {
                out.addAll(r.crcs);
            }
        }
        return out;
    }

    private static Set<Long> own(StarWars.RomSet s) {
        Set<Long> out = new HashSet<Long>();
        for (StarWars.Region r : s.regions.values()) {
            out.addAll(r.crcs);
        }
        return out;
    }

    @Test
    public void merged() {
        List<StarWars.Region> m = StarWars.merged(StarWars.SWTRILGYA);
        assertEquals(StarWars.SWTRILGY.regions.size(), m.size());
        assertEquals("epr-21382.20", m.get(0).names.get(0)); // the clone's own program
        assertEquals("mpr-21342.04", m.get(1).names.get(0)); // the parent's banked program
    }

    @Test
    public void wholeSets() {
        StarWars.Check c = StarWars.check(StarWars.identify(crcs(StarWars.SWTRILGY, true), "x.zip"), crcs(StarWars.SWTRILGY, true), null);
        assertEquals("swtrilgy", c.set);
        assertTrue(c.ok);
        assertTrue(c.driveBoard);
        assertEquals("", c.problem);

        // a merged / non-merged clone: everything in one zip, no force feedback program
        Set<Long> a = crcs(StarWars.SWTRILGYA, false);
        assertEquals("swtrilgya", StarWars.identify(a, "whatever.zip"));
        c = StarWars.check("swtrilgya", a, null);
        assertTrue(c.ok);
        assertFalse(c.driveBoard);
    }

    @Test
    public void splitClone() {
        Set<Long> a = own(StarWars.SWTRILGYA);
        StarWars.Check c = StarWars.check(StarWars.identify(a, "swtrilgya.zip"), a, null);
        assertEquals("swtrilgya", c.set);
        assertFalse(c.ok);
        assertTrue(c.needsParent);
        assertTrue(c.problem, c.problem.contains("swtrilgy.zip"));

        c = StarWars.check("swtrilgya", a, crcs(StarWars.SWTRILGY, true));
        assertTrue(c.ok);
        assertTrue(c.driveBoard);
    }

    @Test
    public void missingAndOther() {
        Set<Long> s = crcs(StarWars.SWTRILGY, false);
        s.remove(0x544D1E28L); // the sound program
        s.remove(0x735157A9L);
        StarWars.Check c = StarWars.check("swtrilgy", s, null);
        assertFalse(c.ok);
        assertFalse(c.needsParent);
        assertEquals(2, c.missingCount);
        assertEquals(Arrays.asList("epr-21383.21", "mpr-21375.18"), c.missing);

        Set<Long> none = new HashSet<Long>(Arrays.asList(1L, 2L));
        assertEquals("", StarWars.identify(none, "pacman.zip"));
        assertEquals("swtrilgy", StarWars.identify(none, "SWTRILGY.ZIP")); // named, its programs missing
        c = StarWars.check("", none, null);
        assertFalse(c.ok);
        assertTrue(c.problem.contains("isn't Star Wars"));
    }

    @Test
    public void zipFile() throws Exception {
        File f = File.createTempFile("swtrilgya", ".zip");
        try {
            ZipOutputStream z = new ZipOutputStream(new FileOutputStream(f));
            byte[] data = "not a real ROM".getBytes("UTF-8");
            z.putNextEntry(new ZipEntry("epr-21382.20"));
            z.write(data);
            z.closeEntry();
            z.close();
            CRC32 crc = new CRC32();
            crc.update(data);
            assertEquals(new HashSet<Long>(Arrays.asList(crc.getValue())), StarWars.zipCrcs(f));
        } finally {
            f.delete();
        }
    }

    @Test
    public void commandLine() {
        assertEquals(Arrays.asList("/g/swtrilgy.zip", "-fullscreen", "-desktop-res", "-no-wide-screen", "-force-feedback",
                "-input-system=sdlgamepad", "-rp-fast-boot=30"), StarWars.args("/g/swtrilgy.zip", true));
        assertFalse(StarWars.args("/g/swtrilgy.zip", false).contains("-force-feedback"));
        assertEquals("curve=60,dead=5,smooth=60", StarWars.flightStick(true, "medium"));
        assertEquals("curve=60,dead=5,smooth=0", StarWars.flightStick(true, "off"));
        assertEquals("curve=60,dead=5,smooth=100", StarWars.flightStick(true, "heavy"));
        assertEquals("off", StarWars.flightStick(false, "light"));
    }

    @Test
    public void ini() {
        String s = StarWars.ini(false, true);
        assertTrue(s.startsWith(StarWars.INI_MARKER + "\n"));
        assertTrue(s.contains("InputSystem = \"sdlgamepad\"\n"));
        assertTrue(s.contains("InputAnalogJoyX = \"JOY_XAXIS\"\n"));
        assertTrue(s.contains("InputAnalogJoyY = \"JOY_YAXIS\"\n"));
        assertTrue(s.contains("InputAnalogJoyTrigger = \"KEY_LEFTCTRL,KEY_A,JOY_BUTTON1,JOY_RZAXIS_POS\"\n"));
        assertTrue(s.contains("InputAnalogJoyEvent = \"KEY_LEFTALT,KEY_S,JOY_BUTTON2,JOY_BUTTON6\"\n"));
        assertTrue(s.contains("InputJoyXDeadZone = \"0\"\n"));
        assertTrue(s.contains("[ swtrilgya ]\nMusicVolume = \"70\"\n"));
        assertFalse(s.contains("JOY1_"));

        s = StarWars.ini(true, false);
        assertTrue(s.contains("InputAnalogJoyY = \"JOY_YAXIS_INV\"\n"));
        assertFalse(s.contains("DeadZone"));
    }
}
