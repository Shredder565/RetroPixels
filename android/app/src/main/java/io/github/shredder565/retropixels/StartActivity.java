package io.github.shredder565.retropixels;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The start screen: Open game, the recent games, Settings, Controls, Help and
 * the version. Game files other apps open with RetroPixels (or share with
 * it) start here too.
 */
public final class StartActivity extends Activity {
    private static final int REQ_OPEN = 1;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private ListView recentList;
    private TextView recentNone;
    private List<Prefs.Recent> recent = new ArrayList<Prefs.Recent>();

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout col = Ui.column(this);
        TextView title = Ui.heading(this, "RetroPixels " + Ui.version(this));
        col.addView(title);
        col.addView(Ui.text(this, "NES, Super Nintendo, Game Boy / Color, Game Boy Advance, Nintendo 64, Genesis, Master System, "
                + "Atari 2600, Atari 7800 and arcade games.", 14, Ui.DIM));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, Ui.dp(this, 12), 0, Ui.dp(this, 12));
        row.addView(Ui.button(this, "Open game…", new View.OnClickListener() {
            public void onClick(View v) {
                openPicker();
            }
        }));
        row.addView(Ui.button(this, "Settings", new View.OnClickListener() {
            public void onClick(View v) {
                startActivity(new Intent(StartActivity.this, SettingsActivity.class));
            }
        }));
        row.addView(Ui.button(this, "Help", new View.OnClickListener() {
            public void onClick(View v) {
                startActivity(new Intent(StartActivity.this, HelpActivity.class));
            }
        }));
        col.addView(row);

        col.addView(Ui.text(this, "Recent games", 16, Ui.ACCENT));
        recentNone = Ui.text(this, "Games you open show here. Tap one to play it again; hold one to take it off the list.", 14, Ui.DIM);
        recentNone.setPadding(0, Ui.dp(this, 8), 0, 0);
        col.addView(recentNone);
        recentList = new ListView(this);
        recentList.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            public void onItemClick(AdapterView<?> p, View v, int pos, long id) {
                if (pos < recent.size()) {
                    play(new File(recent.get(pos).path));
                }
            }
        });
        recentList.setOnItemLongClickListener(new AdapterView.OnItemLongClickListener() {
            public boolean onItemLongClick(AdapterView<?> p, View v, int pos, long id) {
                if (pos < recent.size()) {
                    final Prefs.Recent r = recent.get(pos);
                    new AlertDialog.Builder(StartActivity.this).setMessage("Take " + r.name + " off the list? Its saves are kept.")
                            .setPositiveButton("Take off", new DialogInterface.OnClickListener() {
                                public void onClick(DialogInterface d, int w) {
                                    Library.removeRecent(StartActivity.this, r.path);
                                    refresh();
                                }
                            }).setNegativeButton("Cancel", null).show();
                }
                return true;
            }
        });
        col.addView(recentList, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(col);
        handle(getIntent());
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        handle(i);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void refresh() {
        recent = new ArrayList<Prefs.Recent>();
        List<String> labels = new ArrayList<String>();
        for (Prefs.Recent r : new Prefs(this).recent()) {
            if (new File(r.path).exists()) {
                recent.add(r);
                labels.add(r.name + (r.label.isEmpty() ? "" : "   · " + r.label));
            }
        }
        recentList.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, labels));
        recentNone.setVisibility(recent.isEmpty() ? View.VISIBLE : View.GONE);
    }

    private void openPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, REQ_OPEN);
        } catch (RuntimeException e) {
            new AlertDialog.Builder(this).setMessage("There's no app on this phone to choose files with.").setPositiveButton("OK", null).show();
        }
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req == REQ_OPEN && result == RESULT_OK && data != null && data.getData() != null) {
            importAndPlay(data.getData());
        }
    }

    /** Game files other apps open with RetroPixels or share with it. */
    @SuppressWarnings("deprecation")
    private void handle(Intent i) {
        if (i == null) {
            return;
        }
        Uri u = null;
        if (Intent.ACTION_VIEW.equals(i.getAction())) {
            u = i.getData();
        } else if (Intent.ACTION_SEND.equals(i.getAction())) {
            Object o = i.getParcelableExtra(Intent.EXTRA_STREAM);
            if (o instanceof Uri) {
                u = (Uri) o;
            }
        }
        if (u != null) {
            i.setAction(Intent.ACTION_MAIN); // (not again when the screen turns)
            importAndPlay(u);
        }
    }

    @SuppressWarnings("deprecation")
    private void importAndPlay(final Uri u) {
        final ProgressDialog pd = ProgressDialog.show(this, null, "Opening…", true, false);
        new Thread(new Runnable() {
            public void run() {
                try {
                    final File f = Library.importGame(StartActivity.this, u);
                    ui.post(new Runnable() {
                        public void run() {
                            pd.dismiss();
                            play(f);
                        }
                    });
                } catch (final IOException | RuntimeException e) {
                    ui.post(new Runnable() {
                        public void run() {
                            pd.dismiss();
                            new AlertDialog.Builder(StartActivity.this).setMessage("Couldn't open that file: " + e.getMessage())
                                    .setPositiveButton("OK", null).show();
                        }
                    });
                }
            }
        }, "import").start();
    }

    private void play(File f) {
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra(GameActivity.EXTRA_PATH, f.getPath());
        startActivity(i);
    }
}
