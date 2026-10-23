package io.github.shredder565.retropixels;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/** Settings: sound, pixels, the touch controls, rumble, recording, the screen's way round, .bin files and the controls. */
public final class SettingsActivity extends Activity {
    static final String[] SYSTEM_IDS = {"nes", "snes", "gb", "gba", "n64", "segaMD", "sms", "atari2600", "atari7800", "arcade"};
    static final String[] SYSTEM_LABELS = {"NES", "Super Nintendo", "Game Boy / Color", "Game Boy Advance", "Nintendo 64", "Sega Genesis",
        "Master System", "Atari 2600", "Atari 7800", "Arcade"};

    private Prefs prefs;
    private ListView list;
    private final List<Runnable> acts = new ArrayList<Runnable>();

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = new Prefs(this);
        LinearLayout col = Ui.column(this);
        col.addView(Ui.heading(this, "Settings"));
        list = new ListView(this);
        list.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            public void onItemClick(AdapterView<?> p, View v, int pos, long id) {
                acts.get(pos).run();
            }
        });
        col.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(col);
        refresh();
    }

    private static String pick(String[] ids, String[] labels, String id) {
        for (int i = 0; i < ids.length; i++) {
            if (ids[i].equals(id)) {
                return labels[i];
            }
        }
        return labels[0];
    }

    private void refresh() {
        acts.clear();
        List<String> items = new ArrayList<String>();
        items.add("Volume: " + (prefs.muted() ? "muted" : prefs.volume() + "%"));
        acts.add(new Runnable() {
            public void run() {
                slider("Volume", 0, 100, prefs.volume(), new Set() {
                    public void set(int v) {
                        prefs.setVolume(v);
                    }
                });
            }
        });
        items.add(prefs.muted() ? "Sound: muted (tap to unmute)" : "Sound: on (tap to mute)");
        acts.add(new Runnable() {
            public void run() {
                prefs.setMuted(!prefs.muted());
                refresh();
            }
        });
        items.add("Pixels: " + (prefs.smooth() ? "smooth" : "sharp"));
        acts.add(new Runnable() {
            public void run() {
                prefs.setSmooth(!prefs.smooth());
                refresh();
            }
        });
        final String[] touchIds = {"auto", "always", "never"};
        final String[] touchLabels = {"shown, hidden while a controller is used", "always shown", "never shown"};
        items.add("Touch controls: " + pick(touchIds, touchLabels, prefs.touchMode()));
        acts.add(new Runnable() {
            public void run() {
                choose("Touch controls", touchIds, touchLabels, prefs.touchMode(), new Choose() {
                    public void chose(String id) {
                        prefs.setTouchMode(id);
                    }
                });
            }
        });
        items.add("Touch control opacity: " + prefs.touchOpacity() + "%");
        acts.add(new Runnable() {
            public void run() {
                slider("Touch control opacity", 10, 90, prefs.touchOpacity(), new Set() {
                    public void set(int v) {
                        prefs.setTouchOpacity(v);
                    }
                });
            }
        });
        final String[] sizeIds = {"80", "100", "125"};
        final String[] sizeLabels = {"small", "normal", "large"};
        items.add("Touch control size: " + pick(sizeIds, sizeLabels, String.valueOf(prefs.touchSize())));
        acts.add(new Runnable() {
            public void run() {
                choose("Touch control size", sizeIds, sizeLabels, String.valueOf(prefs.touchSize()), new Choose() {
                    public void chose(String id) {
                        prefs.setTouchSize(Integer.parseInt(id));
                    }
                });
            }
        });
        items.add("Touch controls vibrate when pressed: " + (prefs.touchVibrate() ? "yes" : "no"));
        acts.add(new Runnable() {
            public void run() {
                prefs.setTouchVibrate(!prefs.touchVibrate());
                refresh();
            }
        });
        items.add("Rumble (cartridges with a motor): " + (prefs.rumble() ? "on" : "off"));
        acts.add(new Runnable() {
            public void run() {
                prefs.setRumble(!prefs.rumble());
                refresh();
            }
        });
        items.add("Record games automatically: " + (prefs.autoRecord() ? "on (to Movies/RetroPixels)" : "off"));
        acts.add(new Runnable() {
            public void run() {
                prefs.setAutoRecord(!prefs.autoRecord());
                refresh();
            }
        });
        final String[] oriIds = {"auto", "landscape", "portrait"};
        final String[] oriLabels = {"turns with the phone", "landscape", "portrait"};
        items.add("Screen: " + pick(oriIds, oriLabels, prefs.orientation()));
        acts.add(new Runnable() {
            public void run() {
                choose("Screen", oriIds, oriLabels, prefs.orientation(), new Choose() {
                    public void chose(String id) {
                        prefs.setOrientation(id);
                    }
                });
            }
        });
        final String[] binIds = {"", "segaMD", "sms", "atari2600", "atari7800"};
        final String[] binLabels = {"guess from the file", "Sega Genesis", "Master System", "Atari 2600", "Atari 7800"};
        items.add("Open .bin files as: " + pick(binIds, binLabels, prefs.binSystem()));
        acts.add(new Runnable() {
            public void run() {
                choose("Open .bin files as", binIds, binLabels, prefs.binSystem(), new Choose() {
                    public void chose(String id) {
                        prefs.setBinSystem(id);
                    }
                });
            }
        });
        items.add("Controls (controllers and keyboards)…");
        acts.add(new Runnable() {
            public void run() {
                new AlertDialog.Builder(SettingsActivity.this).setTitle("Controls for")
                        .setItems(SYSTEM_LABELS, new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface d, int w) {
                                Intent i = new Intent(SettingsActivity.this, RemapActivity.class);
                                i.putExtra(RemapActivity.EXTRA_SYSTEM, SYSTEM_IDS[w]);
                                startActivity(i);
                            }
                        }).show();
            }
        });
        list.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, items));
    }

    interface Set {
        void set(int v);
    }

    interface Choose {
        void chose(String id);
    }

    private void slider(String title, final int min, int max, int value, final Set s) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        int p = Ui.dp(this, 20);
        l.setPadding(p, p, p, p);
        final TextView t = Ui.text(this, value + "%", 16, Ui.FG);
        SeekBar b = new SeekBar(this);
        b.setMax(max - min);
        b.setProgress(value - min);
        b.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar sb, int v, boolean user) {
                t.setText((v + min) + "%");
                s.set(v + min);
            }

            public void onStartTrackingTouch(SeekBar sb) {
            }

            public void onStopTrackingTouch(SeekBar sb) {
            }
        });
        l.addView(t);
        l.addView(b);
        new AlertDialog.Builder(this).setTitle(title).setView(l).setPositiveButton("Done", null)
                .setOnDismissListener(new DialogInterface.OnDismissListener() {
                    public void onDismiss(DialogInterface d) {
                        refresh();
                    }
                }).show();
    }

    private void choose(String title, final String[] ids, String[] labels, String cur, final Choose c) {
        int checked = 0;
        for (int i = 0; i < ids.length; i++) {
            if (ids[i].equals(cur)) {
                checked = i;
            }
        }
        new AlertDialog.Builder(this).setTitle(title).setSingleChoiceItems(labels, checked, new DialogInterface.OnClickListener() {
            public void onClick(DialogInterface d, int w) {
                c.chose(ids[w]);
                d.dismiss();
                refresh();
            }
        }).show();
    }
}
