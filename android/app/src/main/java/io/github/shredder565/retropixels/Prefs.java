package io.github.shredder565.retropixels;

import android.content.Context;
import android.content.SharedPreferences;

import io.github.shredder565.retropixels.engine.Json;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** The app's settings, as the native app's settings.json keeps them (volume, controls, machine settings...). */
final class Prefs {
    private final SharedPreferences p;

    Prefs(Context c) {
        p = c.getSharedPreferences("retropixels", Context.MODE_PRIVATE);
    }

    int volume() {
        return Math.max(0, Math.min(100, p.getInt("volume", 80))); // 80%: host.js's starting volume
    }

    void setVolume(int v) {
        p.edit().putInt("volume", v).apply();
    }

    boolean muted() {
        return p.getBoolean("muted", false);
    }

    void setMuted(boolean m) {
        p.edit().putBoolean("muted", m).apply();
    }

    boolean smooth() {
        return p.getBoolean("smooth", false);
    }

    void setSmooth(boolean s) {
        p.edit().putBoolean("smooth", s).apply();
    }

    /** Touch controls: "auto" (hidden while a controller is used), "always" or "never". */
    String touchMode() {
        return p.getString("touch", "auto");
    }

    void setTouchMode(String m) {
        p.edit().putString("touch", m).apply();
    }

    /** How see-through the touch controls are: 10..90 (% opaque). */
    int touchOpacity() {
        return Math.max(10, Math.min(90, p.getInt("touchOpacity", 45)));
    }

    void setTouchOpacity(int v) {
        p.edit().putInt("touchOpacity", v).apply();
    }

    /** Touch control size: 80, 100 or 125 (%). */
    int touchSize() {
        return p.getInt("touchSize", 100);
    }

    void setTouchSize(int v) {
        p.edit().putInt("touchSize", v).apply();
    }

    boolean touchVibrate() {
        return p.getBoolean("touchVibrate", true);
    }

    void setTouchVibrate(boolean v) {
        p.edit().putBoolean("touchVibrate", v).apply();
    }

    boolean rumble() {
        return p.getBoolean("rumble", true);
    }

    void setRumble(boolean v) {
        p.edit().putBoolean("rumble", v).apply();
    }

    /** "auto", "landscape" or "portrait". */
    String orientation() {
        return p.getString("orientation", "auto");
    }

    void setOrientation(String o) {
        p.edit().putString("orientation", o).apply();
    }

    /** The system for .bin files ("" = guess from the file), as the native app's File > Open .bin files as. */
    String binSystem() {
        return p.getString("bin", "");
    }

    void setBinSystem(String s) {
        p.edit().putString("bin", s).apply();
    }

    // ---- per system and per game (JSON) ----

    private Map<String, Object> obj(String key) {
        try {
            return Json.parseObject(p.getString(key, "{}"));
        } catch (RuntimeException e) {
            return new LinkedHashMap<String, Object>();
        }
    }

    /** The player's changes to a system's controls (only the buttons that differ from the defaults). */
    Map<String, Object> controls(String system) {
        return Json.obj(obj("controls"), system);
    }

    void setControls(String system, Map<String, Object> changed) {
        Map<String, Object> all = obj("controls");
        if (changed == null || changed.isEmpty()) {
            all.remove(system);
        } else {
            all.put(system, changed);
        }
        p.edit().putString("controls", Json.write(all)).apply();
    }

    /** Arcade machine settings (DIP switches) the player changed, by game. */
    Map<String, Object> dips(String game) {
        return Json.obj(obj("dips"), game);
    }

    void setDip(String game, String id, int value) {
        Map<String, Object> all = obj("dips");
        Map<String, Object> g = Json.obj(all, game);
        g.put(id, value);
        all.put(game, g);
        p.edit().putString("dips", Json.write(all)).apply();
    }

    /** The player picked in arcade games for 3 or 4 players, by game. */
    int seat(String game) {
        return Json.integer(obj("seats"), game, 0);
    }

    void setSeat(String game, int seat) {
        Map<String, Object> all = obj("seats");
        all.put(game, seat);
        p.edit().putString("seats", Json.write(all)).apply();
    }

    // ---- recent games ----

    static final class Recent {
        final String path, name, label;

        Recent(String path, String name, String label) {
            this.path = path;
            this.name = name;
            this.label = label;
        }
    }

    static final int MAX_RECENT = 10;

    List<Recent> recent() {
        List<Recent> out = new ArrayList<Recent>();
        try {
            Object v = Json.parse(p.getString("recent", "[]"));
            if (v instanceof List) {
                for (Object o : (List<?>) v) {
                    if (o instanceof Map) {
                        @SuppressWarnings("unchecked")
                        Map<String, Object> m = (Map<String, Object>) o;
                        out.add(new Recent(Json.str(m, "path", ""), Json.str(m, "name", ""), Json.str(m, "label", "")));
                    }
                }
            }
        } catch (RuntimeException ignored) {
            // a damaged list starts again
        }
        return out;
    }

    void setRecent(List<Recent> list) {
        List<Object> out = new ArrayList<Object>();
        for (Recent r : list) {
            out.add(Json.obj("path", r.path, "name", r.name, "label", r.label));
        }
        p.edit().putString("recent", Json.write(out)).apply();
    }
}
