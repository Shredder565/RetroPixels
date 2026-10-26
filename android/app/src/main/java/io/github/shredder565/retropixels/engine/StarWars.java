package io.github.shredder565.retropixels.engine;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Star Wars Trilogy Arcade (Sega Model 3, played by RetroPixels' Supermodel),
 * the parts the Android app shares with RetroPixels on Windows
 * (internal/supermodel): which ROM set a .zip is and whether it is complete,
 * Supermodel's command line, its Supermodel.ini and the flight stick
 * setting. Plain Java, so it is tested on the PC.
 *
 * The player picks their own swtrilgy.zip or swtrilgya.zip; no game files
 * come with the app. swtrilgya (the first export version) is a clone of
 * swtrilgy that only replaces the four main program ROMs: as a split set it
 * also needs swtrilgy.zip next to it.
 */
public final class StarWars {
    public static final String TITLE = "Star Wars Trilogy Arcade";

    /** How much of the start-up runs unseen at full speed (-rp-fast-boot), as on Windows. */
    public static final int FAST_BOOT_SECONDS = 30;

    /** The first line of the Supermodel.ini the app writes; without it the file is the player's own. */
    public static final String INI_MARKER = "; RetroPixels Supermodel settings v1 (Android)";

    /** The force feedback board's program (optional). */
    static final long DRIVE_BOARD_CRC = 0x65082B14L;

    private StarWars() {
    }

    /** One ROM set: its regions (name, required, then file name / CRC-32 pairs). */
    static final class RomSet {
        final String name, parent, version;
        final Map<String, Region> regions = new LinkedHashMap<String, Region>();

        RomSet(String name, String parent, String version) {
            this.name = name;
            this.parent = parent;
            this.version = version;
        }

        RomSet region(String n, boolean required, Object... files) {
            Region r = new Region(n, required);
            for (int i = 0; i < files.length; i += 2) {
                r.names.add((String) files[i]);
                r.crcs.add(((Number) files[i + 1]).longValue() & 0xffffffffL);
            }
            regions.put(n, r);
            return this;
        }

        /** The first region: the main program, which tells the versions apart. */
        Region program() {
            return regions.values().iterator().next();
        }
    }

    static final class Region {
        final String name;
        final boolean required;
        final List<String> names = new ArrayList<String>();
        final List<Long> crcs = new ArrayList<Long>();

        Region(String name, boolean required) {
            this.name = name;
            this.required = required;
        }
    }

    static final RomSet SWTRILGY = new RomSet("swtrilgy", "", "Export, Revision A")
            .region("crom", true, "epr-21382a.20", 0x69BAF117L, "epr-21381a.19", 0x2DD34E28L, "epr-21380a.18", 0x780FB4E7L,
                    "epr-21379a.17", 0x24DC1555L)
            .region("banked_crom", true, "mpr-21342.04", 0x339525CEL, "mpr-21341.03", 0xB2A269E4L, "mpr-21340.02", 0xAD36040EL,
                    "mpr-21339.01", 0xC0CE5037L, "mpr-21346.08", 0xC8733594L, "mpr-21345.07", 0x6C183A21L, "mpr-21344.06", 0x87453D76L,
                    "mpr-21343.05", 0x12552D07L, "mpr-21350.12", 0x486195E7L, "mpr-21349.11", 0x3D39454BL, "mpr-21348.10", 0x1F7CC5F5L,
                    "mpr-21347.09", 0xECB6B934L)
            .region("vrom", true, "mpr-21359.26", 0x34EF4122L, "mpr-21360.27", 0x2882B95EL, "mpr-21361.28", 0x9B61C3C1L,
                    "mpr-21362.29", 0x01A92169L, "mpr-21363.30", 0xE7D18FEDL, "mpr-21364.31", 0xCB6A5468L, "mpr-21365.32", 0xAD5449D8L,
                    "mpr-21366.33", 0xDEFB6B95L, "mpr-21367.34", 0xDFD51029L, "mpr-21368.35", 0xAE90FD21L, "mpr-21369.36", 0xBF17EEB4L,
                    "mpr-21370.37", 0x2321592AL, "mpr-21371.38", 0xA68782FDL, "mpr-21372.39", 0xFC3F4E8BL, "mpr-21373.40", 0xB76AD261L,
                    "mpr-21374.41", 0xAE6C4D28L)
            .region("sound_program", true, "epr-21383.21", 0x544D1E28L)
            .region("sound_samples", true, "mpr-21355.22", 0xC1B2D326L, "mpr-21357.24", 0x02703FABL)
            .region("mpeg_program", true, "epr-21384.2", 0x12FA4780L)
            .region("mpeg_music", true, "mpr-21375.18", 0x735157A9L, "mpr-21376.20", 0xE635F81EL, "mpr-21377.22", 0x720621F8L,
                    "mpr-21378.24", 0x1FCF715EL)
            .region("driveboard_program", false, "epr-21119.ic8", DRIVE_BOARD_CRC);

    static final RomSet SWTRILGYA = new RomSet("swtrilgya", "swtrilgy", "Export")
            .region("crom", true, "epr-21382.20", 0x0B9C44A0L, "epr-21381.19", 0xBB5757BFL, "epr-21380.18", 0x49B182F2L,
                    "epr-21379.17", 0x61AD51D9L);

    /** The versions the app plays, the clone first as on Windows. */
    static final RomSet[] SETS = {SWTRILGYA, SWTRILGY};

    static RomSet set(String name) {
        for (RomSet s : SETS) {
            if (s.name.equalsIgnoreCase(name)) {
                return s;
            }
        }
        return null;
    }

    /** A clone's own regions, and its parent's for the rest (as Supermodel merges them). */
    static List<Region> merged(RomSet s) {
        List<Region> out = new ArrayList<Region>();
        RomSet p = s.parent.isEmpty() ? null : set(s.parent);
        if (p == null) {
            out.addAll(s.regions.values());
            return out;
        }
        for (Region r : p.regions.values()) {
            Region own = s.regions.get(r.name);
            out.add(own != null ? own : r);
        }
        for (Region r : s.regions.values()) {
            if (!p.regions.containsKey(r.name)) {
                out.add(r);
            }
        }
        return out;
    }

    /** What is in a game file. */
    public static final class Check {
        /** swtrilgy or swtrilgya, or "" when it is neither. */
        public String set = "";
        /** Supermodel has every file it needs (with the parent's .zip, when one is needed). */
        public boolean ok;
        /** A split clone set: the parent's .zip (swtrilgy.zip) is needed next to it. */
        public boolean needsParent;
        /** The force feedback board's program is there (force feedback is then turned on). */
        public boolean driveBoard;
        /** Files missing (at most four named), and how many in all. */
        public final List<String> missing = new ArrayList<String>();
        public int missingCount;
        /** Why it can't be played, said for the player ("" when it can). */
        public String problem = "";
    }

    /** The CRC-32s in a .zip (from its directory: nothing is unpacked). */
    public static Set<Long> zipCrcs(File f) throws IOException {
        Set<Long> out = new HashSet<Long>();
        ZipFile z = new ZipFile(f);
        try {
            Enumeration<? extends ZipEntry> e = z.entries();
            while (e.hasMoreElements()) {
                ZipEntry en = e.nextElement();
                if (!en.isDirectory() && en.getCrc() != -1) {
                    out.add(en.getCrc() & 0xffffffffL);
                }
            }
        } finally {
            z.close();
        }
        return out;
    }

    private static int count(Set<Long> crcs, Region r) {
        int n = 0;
        for (Long c : r.crcs) {
            if (crcs.contains(c)) {
                n++;
            }
        }
        return n;
    }

    /**
     * Which version a .zip holds, from its CRC-32s; "" when it isn't the
     * game. fileName (without folders) names the set when its program ROMs
     * are missing.
     */
    public static String identify(Set<Long> crcs, String fileName) {
        RomSet best = null;
        int bestN = 0;
        for (RomSet s : SETS) {
            int n = count(crcs, s.program());
            if (n > bestN) {
                best = s;
                bestN = n;
            }
        }
        if (best == null && fileName != null) {
            String stem = fileName.toLowerCase(java.util.Locale.ROOT);
            if (stem.endsWith(".zip")) {
                stem = stem.substring(0, stem.length() - 4);
            }
            best = set(stem);
        }
        return best == null ? "" : best.name;
    }

    /**
     * Checks a set's files: own holds the .zip's CRC-32s, parent the parent
     * .zip's (null when there is none next to it), as Supermodel would find
     * them.
     */
    public static Check check(String setName, Set<Long> own, Set<Long> parent) {
        Check c = new Check();
        RomSet s = set(setName);
        if (s == null) {
            c.problem = "it isn't Star Wars Trilogy Arcade (swtrilgy or swtrilgya)";
            return c;
        }
        c.set = s.name;
        Set<Long> all = new HashSet<Long>(own);
        if (!s.parent.isEmpty() && parent != null) {
            all.addAll(parent);
        }
        List<String> missing = new ArrayList<String>();
        for (Region r : merged(s)) {
            if (!r.required) {
                continue;
            }
            for (int i = 0; i < r.crcs.size(); i++) {
                if (!all.contains(r.crcs.get(i))) {
                    missing.add(r.names.get(i));
                }
            }
        }
        c.driveBoard = all.contains(DRIVE_BOARD_CRC);
        c.missingCount = missing.size();
        c.missing.addAll(missing.subList(0, Math.min(4, missing.size())));
        c.ok = missing.isEmpty();
        if (!c.ok) {
            if (!s.parent.isEmpty() && parent == null && !hasParentFiles(s, own)) {
                c.needsParent = true;
                c.problem = s.name + ".zip holds only the files that differ from the main version, so " + s.parent
                        + ".zip (the main version's ROM set) is needed too: choose it as well";
            } else {
                StringBuilder list = new StringBuilder();
                for (String m : c.missing) {
                    list.append(list.length() == 0 ? "" : ", ").append(m);
                }
                if (c.missingCount > c.missing.size()) {
                    list.append(", …");
                }
                c.problem = c.missingCount + " of the game's files aren't in it (" + list
                        + "), or don't match the version Supermodel expects";
            }
        }
        return c;
    }

    private static boolean hasParentFiles(RomSet s, Set<Long> crcs) {
        RomSet p = set(s.parent);
        if (p == null) {
            return false;
        }
        for (Region r : p.regions.values()) {
            if (s.regions.containsKey(r.name) || !r.required) {
                continue;
            }
            if (count(crcs, r) > 0) {
                return true;
            }
        }
        return false;
    }

    /** The version's description for the player ("swtrilgya (Export)"). */
    public static String describe(String setName) {
        RomSet s = set(setName);
        return s == null ? setName : s.name + " (" + s.version + ")";
    }

    /** The parent set a version needs, or "". */
    public static String parentOf(String setName) {
        RomSet s = set(setName);
        return s == null ? "" : s.parent;
    }

    // ---------------- running it ----------------

    /**
     * Supermodel's command line after the program name: the game's .zip,
     * the whole screen at its own resolution, the game's 4:3 picture (the
     * touch controls go at the sides), SDL's game controllers, the fast boot
     * and, with the force feedback program, the force feedback board (the
     * game then finds its stick ready instead of waiting for it).
     */
    public static List<String> args(String zip, boolean forceFeedback) {
        List<String> a = new ArrayList<String>();
        a.add(zip);
        a.add("-fullscreen");
        a.add("-desktop-res");
        a.add("-no-wide-screen");
        if (forceFeedback) {
            a.add("-force-feedback");
        }
        a.add("-input-system=sdlgamepad");
        a.add("-rp-fast-boot=" + FAST_BOOT_SECONDS);
        return a;
    }

    /** Flight stick shaping as on Windows (FlightStickEnv): a round dead zone, a cubic curve, smoothing. */
    public static final int FLIGHT_STICK_CURVE = 60, FLIGHT_STICK_DEAD = 5;

    public static int smoothnessMs(String name) {
        if ("off".equals(name)) {
            return 0;
        }
        if ("light".equals(name)) {
            return 35;
        }
        if ("heavy".equals(name)) {
            return 100;
        }
        return 60; // medium, the default
    }

    /** The value of RP_FLIGHTSTICK. */
    public static String flightStick(boolean on, String smoothness) {
        if (!on) {
            return "off";
        }
        return "curve=" + FLIGHT_STICK_CURVE + ",dead=" + FLIGHT_STICK_DEAD + ",smooth=" + smoothnessMs(smoothness);
    }

    /**
     * Supermodel.ini for Android. As on Windows (internal/supermodel INI),
     * but every controller counts (JOY_ rather than JOY1_): the touch
     * controls are a controller of their own (OSD/Android), a Bluetooth or
     * USB controller another, and whichever moved last steers.
     *
     *   stick           flight yoke (aim)
     *   A or RT         trigger (fire)
     *   B or RB         event button (force / lock-on)
     *   X / LB          second trigger / event button
     *   Start           start, Back (View) inserts a coin
     *   Back + Start    leave the game (or the phone's Back)
     */
    public static String ini(boolean invertY, boolean flightStick) {
        StringBuilder b = new StringBuilder();
        b.append(INI_MARKER).append('\n');
        b.append("; Written by RetroPixels for Star Wars Trilogy Arcade before each game while the first\n");
        b.append("; line is there; delete the first line to keep your own changes.\n\n");
        b.append("[ Global ]\n\n");
        w(b, "InputSystem", "sdlgamepad");
        b.append("\n; Common\n");
        w(b, "InputStart1", "KEY_1,JOY_BUTTON8");
        w(b, "InputCoin1", "KEY_5,KEY_3,JOY_BUTTON7");
        w(b, "InputStart2", "KEY_2");
        w(b, "InputCoin2", "KEY_6");
        w(b, "InputServiceA", "KEY_9");
        w(b, "InputTestA", "KEY_F2");
        w(b, "InputServiceB", "NONE");
        w(b, "InputTestB", "NONE");
        b.append("\n; Star Wars Trilogy's flight yoke\n");
        w(b, "InputAnalogJoyLeft", "KEY_LEFT");
        w(b, "InputAnalogJoyRight", "KEY_RIGHT");
        w(b, "InputAnalogJoyUp", "KEY_UP");
        w(b, "InputAnalogJoyDown", "KEY_DOWN");
        w(b, "InputAnalogJoyX", "JOY_XAXIS");
        w(b, "InputAnalogJoyY", invertY ? "JOY_YAXIS_INV" : "JOY_YAXIS");
        if (flightStick) {
            b.append("; The stick's dead zone is RetroPixels' flight stick shaping's own (round).\n");
            w(b, "InputJoyXDeadZone", "0");
            w(b, "InputJoyYDeadZone", "0");
        }
        w(b, "InputAnalogJoyTrigger", "KEY_LEFTCTRL,KEY_A,JOY_BUTTON1,JOY_RZAXIS_POS");
        w(b, "InputAnalogJoyEvent", "KEY_LEFTALT,KEY_S,JOY_BUTTON2,JOY_BUTTON6");
        w(b, "InputAnalogJoyTrigger2", "KEY_D,JOY_BUTTON3");
        w(b, "InputAnalogJoyEvent2", "KEY_F,JOY_BUTTON5");
        b.append("\n; Supermodel's own keys (Back + Start on a controller leaves the game)\n");
        w(b, "InputUIExit", "KEY_ESCAPE,JOY_BUTTON7+JOY_BUTTON8");
        w(b, "InputUIReset", "KEY_ALT+KEY_R");
        w(b, "InputUIPause", "KEY_ALT+KEY_P,KEY_PAUSE");
        w(b, "InputUISaveState", "KEY_F5");
        w(b, "InputUIChangeSlot", "KEY_F6");
        w(b, "InputUILoadState", "KEY_F7");
        w(b, "InputUIClearNVRAM", "KEY_ALT+KEY_N");
        b.append("\n; Star Wars Trilogy's music board is louder than its sound effects (Supermodel's README)\n");
        for (RomSet s : SETS) {
            b.append("\n[ ").append(s.name).append(" ]\n");
            w(b, "MusicVolume", "70");
            w(b, "SoundVolume", "100");
        }
        return b.toString();
    }

    private static void w(StringBuilder b, String k, String v) {
        b.append(k).append(" = \"").append(v).append("\"\n");
    }
}
