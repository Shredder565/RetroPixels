package io.github.shredder565.retropixels;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import io.github.shredder565.retropixels.engine.Json;

import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Changes a system's controls, as the native app's Controls window: each of
 * the system's buttons with its controller button and keyboard key. Tap one
 * and press the controller button or key to use. Only the changes from the
 * defaults are kept, so new defaults in later versions still arrive.
 */
public final class RemapActivity extends Activity {
    static final String EXTRA_SYSTEM = "system";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private Prefs prefs;
    private String system;
    private List<String> buttons = new ArrayList<String>();
    private Map<String, Object> defaults = new LinkedHashMap<String, Object>();
    private Map<String, Object> map = new LinkedHashMap<String, Object>();
    private ListView list;
    private TextView note;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = new Prefs(this);
        system = getIntent().getStringExtra(EXTRA_SYSTEM);
        if (system == null) {
            system = "nes";
        }
        String label = system;
        for (int i = 0; i < SettingsActivity.SYSTEM_IDS.length; i++) {
            if (SettingsActivity.SYSTEM_IDS[i].equals(system)) {
                label = SettingsActivity.SYSTEM_LABELS[i];
            }
        }
        LinearLayout col = Ui.column(this);
        col.addView(Ui.heading(this, "Controls: " + label));
        note = Ui.text(this, "Loading…", 14, Ui.DIM);
        col.addView(note);
        list = new ListView(this);
        list.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            public void onItemClick(AdapterView<?> p, View v, int pos, long id) {
                if (pos < buttons.size()) {
                    change(buttons.get(pos));
                }
            }
        });
        col.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
        col.addView(Ui.button(this, "Put them all back as they were at first", new View.OnClickListener() {
            public void onClick(View v) {
                map = copy(defaults);
                save();
            }
        }));
        setContentView(col);
        new Thread(new Runnable() {
            public void run() {
                load();
            }
        }, "controls").start();
    }

    /** Asks the engine for the system's buttons and default controls. */
    private void load() {
        String err = null;
        Map<String, Object> r = null;
        try {
            EngineProcess e = EngineProcess.start(this);
            try {
                r = e.client.call(Json.obj("cmd", "controls", "system", system, "saved", prefs.controls(system)));
            } finally {
                e.stop();
            }
            if (!Json.bool(r, "ok")) {
                err = Json.str(r, "error", "?");
            }
        } catch (IOException ex) {
            err = ex.getMessage();
        }
        final String error = err;
        final Map<String, Object> reply = r;
        ui.post(new Runnable() {
            public void run() {
                if (error != null) {
                    note.setText("The emulator engine couldn't say what the controls are: " + error);
                    return;
                }
                buttons = Json.strings(reply, "buttons");
                defaults = Json.obj(reply, "defaults");
                map = Json.obj(reply, "map");
                String n = Json.str(reply, "note", "");
                note.setText((n.isEmpty() ? "" : n + "\n\n") + "Tap a button, then press the controller button or key to use for it. "
                        + "Controllers: the first plays as player 1, the second as player 2 and so on; the keyboard plays as player 1.");
                show();
            }
        });
    }

    private static Map<String, Object> copy(Map<String, Object> m) {
        return Json.parseObject(Json.write(m));
    }

    private String describe(String b) {
        Map<String, Object> bd = Json.obj(map, b);
        List<String> pad = new ArrayList<String>();
        for (Object o : Json.list(bd, "pad")) {
            int i = (int) Math.round(((Number) o).doubleValue());
            if (i >= 0 && i < KeyNames.PAD_NAMES.length) {
                pad.add(KeyNames.PAD_NAMES[i]);
            }
        }
        List<String> keys = new ArrayList<String>();
        for (String k : Json.strings(bd, "keys")) {
            keys.add(KeyNames.keyLabel(k));
        }
        String p = pad.isEmpty() ? "—" : join(pad);
        String k = keys.isEmpty() ? "—" : join(keys);
        return b + (Json.bool(bd, "toggle") ? " (switch)" : "") + "\n    controller: " + p + "    keyboard: " + k;
    }

    private static String join(List<String> l) {
        StringBuilder b = new StringBuilder();
        for (String s : l) {
            if (b.length() > 0) {
                b.append(", ");
            }
            b.append(s);
        }
        return b.toString();
    }

    private void show() {
        List<String> rows = new ArrayList<String>();
        for (String b : buttons) {
            rows.add(describe(b));
        }
        list.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, rows));
    }

    private void change(final String button) {
        final AlertDialog d = new AlertDialog.Builder(this).setTitle(button)
                .setMessage("Press the controller button or keyboard key for " + button + ".\n\n" + describe(button))
                .setPositiveButton("Clear", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface di, int w) {
                        set(button, new ArrayList<Object>(), new ArrayList<Object>());
                    }
                })
                .setNeutralButton("Default", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface di, int w) {
                        map.put(button, Json.obj(copy(defaults), button));
                        save();
                    }
                })
                .setNegativeButton("Cancel", null).create();
        d.setOnKeyListener(new DialogInterface.OnKeyListener() {
            public boolean onKey(DialogInterface di, int code, KeyEvent e) {
                if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_ESCAPE) {
                    return false;
                }
                if (e.getAction() != KeyEvent.ACTION_DOWN || e.getRepeatCount() != 0) {
                    return true;
                }
                Map<String, Object> bd = Json.obj(map, button);
                if (KeyNames.fromPad(e)) {
                    int b = KeyNames.padButton(code);
                    if (b < 0) {
                        return false;
                    }
                    List<Object> pad = new ArrayList<Object>();
                    pad.add((double) b);
                    set(button, pad, Json.list(bd, "keys"));
                } else {
                    String k = KeyNames.keyName(e);
                    if (k == null) {
                        return false;
                    }
                    List<Object> keys = new ArrayList<Object>();
                    keys.add(k);
                    set(button, Json.list(bd, "pad"), keys);
                }
                di.dismiss();
                return true;
            }
        });
        d.show();
    }

    private void set(String button, List<Object> pad, List<Object> keys) {
        Map<String, Object> bd = Json.obj(map, button);
        bd.put("pad", pad);
        bd.put("keys", keys);
        map.put(button, bd);
        save();
    }

    /** Keeps only the buttons that differ from the defaults. */
    private void save() {
        Map<String, Object> changed = new LinkedHashMap<String, Object>();
        for (String b : buttons) {
            Map<String, Object> cur = Json.obj(map, b), def = Json.obj(defaults, b);
            String a = Json.write(Json.list(cur, "pad")) + Json.write(Json.list(cur, "keys"));
            String z = Json.write(Json.list(def, "pad")) + Json.write(Json.list(def, "keys"));
            if (!a.equals(z)) {
                changed.put(b, Json.obj("pad", Json.list(cur, "pad"), "keys", Json.list(cur, "keys")));
            }
        }
        prefs.setControls(system, changed);
        show();
    }
}
