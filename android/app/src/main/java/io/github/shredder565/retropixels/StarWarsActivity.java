package io.github.shredder565.retropixels;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.ClipData;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import io.github.shredder565.retropixels.engine.StarWars;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;

/**
 * Star Wars Trilogy Arcade's own screen, like the Supermodel tab of
 * RetroPixels on Windows: the player chooses their own swtrilgy.zip or
 * swtrilgya.zip (no game files come with the app), sees whether it can be
 * played, sets the stick's feel and plays it (SupermodelActivity, in the
 * APKs built with Supermodel for Android).
 */
public final class StarWarsActivity extends Activity {
    private static final int REQ_OPEN = 1;
    static final String SUPERMODEL_ACTIVITY = "io.github.shredder565.retropixels.SupermodelActivity";
    private static final String[] SMOOTHNESS = {"off", "light", "medium", "heavy"};

    private final Handler ui = new Handler(Looper.getMainLooper());
    private SharedPreferences sp;
    private TextView status;
    private View play;
    private String playZip;
    private boolean forceFeedback;

    /** Whether this build has Supermodel (its activity and libsupermodel.so). */
    static boolean available(Context c) {
        try {
            Class.forName(SUPERMODEL_ACTIVITY);
        } catch (ClassNotFoundException e) {
            return false;
        }
        return new File(c.getApplicationInfo().nativeLibraryDir, "libsupermodel.so").isFile();
    }

    static File romsDir(Context c) {
        File d = new File(new File(c.getFilesDir(), "supermodel"), "roms");
        d.mkdirs();
        return d;
    }

    /**
     * Takes a .zip into Star Wars' own folder (as swtrilgy.zip or
     * swtrilgya.zip, which Supermodel needs to find a parent set by) when it
     * holds the game; returns the set's name, or "" (the file is left alone).
     */
    static String adopt(Context c, File f) throws IOException {
        if (!f.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".zip")) {
            return "";
        }
        Set<Long> crcs;
        try {
            crcs = StarWars.zipCrcs(f);
        } catch (IOException e) {
            return "";
        }
        String set = StarWars.identify(crcs, f.getName());
        if (set.isEmpty()) {
            return "";
        }
        File to = new File(romsDir(c), set + ".zip");
        if (!f.equals(to)) {
            to.delete();
            if (!f.renameTo(to)) {
                Library.copy(c, Uri.fromFile(f), to, Library.MAX_GAME);
                f.delete();
            }
        }
        return set;
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        sp = getSharedPreferences("retropixels", MODE_PRIVATE);
        LinearLayout col = Ui.column(this);
        col.addView(Ui.heading(this, StarWars.TITLE));
        col.addView(Ui.text(this, "Sega Model 3 (1998), played by RetroPixels' Supermodel. Choose your own copy of the game: "
                + "the MAME ROM set swtrilgy.zip or swtrilgya.zip (a split swtrilgya.zip needs swtrilgy.zip too; choose both). "
                + "No game files come with RetroPixels.", 14, Ui.DIM));

        status = Ui.text(this, "", 15, Ui.FG);
        status.setPadding(0, Ui.dp(this, 12), 0, Ui.dp(this, 4));
        col.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, Ui.dp(this, 8), 0, Ui.dp(this, 8));
        play = Ui.button(this, "Play", new View.OnClickListener() {
            public void onClick(View v) {
                play();
            }
        });
        row.addView(play);
        row.addView(Ui.button(this, "Choose game file…", new View.OnClickListener() {
            public void onClick(View v) {
                openPicker();
            }
        }));
        row.addView(Ui.button(this, "Remove", new View.OnClickListener() {
            public void onClick(View v) {
                confirmRemove();
            }
        }));
        col.addView(row);

        CheckBox invert = new CheckBox(this);
        invert.setText("Flight-style up / down (push the stick up to dive)");
        invert.setTextColor(Ui.FG);
        invert.setChecked(sp.getBoolean("swInvertY", false));
        invert.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            public void onCheckedChanged(CompoundButton b, boolean on) {
                sp.edit().putBoolean("swInvertY", on).apply();
            }
        });
        col.addView(invert);

        LinearLayout smoothRow = new LinearLayout(this);
        smoothRow.setOrientation(LinearLayout.HORIZONTAL);
        smoothRow.addView(Ui.text(this, "Aim smoothing  ", 15, Ui.FG));
        Spinner smooth = new Spinner(this);
        smooth.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item,
                new String[] {"Off", "Light", "Medium", "Heavy"}));
        String now = sp.getString("swSmooth", "medium");
        for (int i = 0; i < SMOOTHNESS.length; i++) {
            if (SMOOTHNESS[i].equals(now)) {
                smooth.setSelection(i);
            }
        }
        smooth.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                sp.edit().putString("swSmooth", SMOOTHNESS[pos]).apply();
            }

            public void onNothingSelected(AdapterView<?> p) {
            }
        });
        smoothRow.addView(smooth);
        col.addView(smoothRow);

        TextView controls = Ui.text(this, "Controls\n"
                + "Touch: drag on the left half to aim, FIRE and EVENT on the right, COIN and START at the top.\n"
                + "Controller (Xbox-style, Bluetooth or USB): left stick aims, A or RT fires, B or RB is the event button "
                + "(force / lock-on), Back (View) inserts a coin, Start starts, Back + Start leaves.\n"
                + "The phone's Back leaves the game. The game's start-up checks run unseen for "
                + StarWars.FAST_BOOT_SECONDS + " seconds, so it opens ready for coins. "
                + "With Settings > Record games automatically on, games are recorded to Movies/RetroPixels.", 14, Ui.DIM);
        controls.setPadding(0, Ui.dp(this, 12), 0, 0);
        col.addView(controls);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Ui.BG);
        scroll.setFillViewport(true);
        scroll.addView(col);
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        // a recording saved by the game's own process (SupermodelActivity.SAVED_NOTE)
        File note = new File(new File(getFilesDir(), "supermodel"), "recording-saved.txt");
        if (note.isFile()) {
            String where = readNote(note);
            note.delete();
            if (where != null && !where.isEmpty()) {
                Toast.makeText(this, "Recording saved in " + where, Toast.LENGTH_LONG).show();
            }
        }
    }

    private static String readNote(File f) {
        try {
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            try {
                byte[] b = new byte[4096];
                int n = in.read(b);
                return n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
            } finally {
                in.close();
            }
        } catch (IOException e) {
            return null;
        }
    }

    /** What is in Star Wars' folder, and which version plays (the first export version first, as on Windows). */
    private void refresh() {
        playZip = null;
        forceFeedback = false;
        StringBuilder s = new StringBuilder();
        File dir = romsDir(this);
        File parent = new File(dir, "swtrilgy.zip");
        int found = 0;
        for (String set : new String[] {"swtrilgya", "swtrilgy"}) {
            File f = new File(dir, set + ".zip");
            if (!f.isFile()) {
                continue;
            }
            found++;
            StarWars.Check c;
            try {
                Set<Long> p = null;
                String par = StarWars.parentOf(set);
                if (!par.isEmpty() && parent.isFile()) {
                    p = StarWars.zipCrcs(parent);
                }
                c = StarWars.check(set, StarWars.zipCrcs(f), p);
            } catch (IOException e) {
                c = new StarWars.Check();
                c.problem = "it can't be read as a .zip file (" + e.getMessage() + ")";
            }
            s.append(StarWars.describe(set)).append(": ").append(c.ok ? "ready" + (c.driveBoard ? ", with force feedback" : "") : c.problem)
                    .append('\n');
            if (c.ok && playZip == null) {
                playZip = f.getPath();
                forceFeedback = c.driveBoard;
            }
        }
        if (found == 0) {
            s.append("No game file chosen yet.");
        }
        boolean here = available(this);
        if (!here) {
            s.append("\nThis build of RetroPixels doesn't include Supermodel for Android, so the game can't be played here.");
        }
        status.setText(s.toString().trim());
        play.setEnabled(here && playZip != null);
    }

    private void openPicker() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        try {
            startActivityForResult(i, REQ_OPEN);
        } catch (RuntimeException e) {
            new AlertDialog.Builder(this).setMessage("There's no app on this phone to choose files with.").setPositiveButton("OK", null).show();
        }
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req != REQ_OPEN || result != RESULT_OK || data == null) {
            return;
        }
        List<Uri> uris = new ArrayList<Uri>();
        ClipData clip = data.getClipData();
        if (clip != null) {
            for (int k = 0; k < clip.getItemCount(); k++) {
                if (clip.getItemAt(k).getUri() != null) {
                    uris.add(clip.getItemAt(k).getUri());
                }
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        if (!uris.isEmpty()) {
            importAll(uris);
        }
    }

    @SuppressWarnings("deprecation")
    private void importAll(final List<Uri> uris) {
        final ProgressDialog pd = ProgressDialog.show(this, null, "Copying…", true, false);
        new Thread(new Runnable() {
            public void run() {
                final StringBuilder notes = new StringBuilder();
                for (Uri u : uris) {
                    String name = Library.displayName(StarWarsActivity.this, u);
                    File tmp = new File(romsDir(StarWarsActivity.this), "incoming-" + System.nanoTime() + ".zip");
                    try {
                        Library.copy(StarWarsActivity.this, u, tmp, Library.MAX_GAME);
                        String set = adopt(StarWarsActivity.this, tmp);
                        if (set.isEmpty()) {
                            notes.append(name).append(" isn't Star Wars Trilogy Arcade (swtrilgy.zip or swtrilgya.zip).\n");
                        }
                    } catch (IOException | RuntimeException e) {
                        notes.append("Couldn't copy ").append(name).append(": ").append(e.getMessage()).append('\n');
                    } finally {
                        tmp.delete();
                    }
                }
                ui.post(new Runnable() {
                    public void run() {
                        pd.dismiss();
                        refresh();
                        if (notes.length() > 0) {
                            new AlertDialog.Builder(StarWarsActivity.this).setMessage(notes.toString().trim())
                                    .setPositiveButton("OK", null).show();
                        }
                    }
                });
            }
        }, "import").start();
    }

    private void confirmRemove() {
        new AlertDialog.Builder(this).setMessage("Remove the Star Wars Trilogy Arcade game files from RetroPixels? "
                + "Your own copies elsewhere aren't touched, and the game's settings and scores are kept.")
                .setPositiveButton("Remove", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) {
                        File[] list = romsDir(StarWarsActivity.this).listFiles();
                        if (list != null) {
                            for (File f : list) {
                                f.delete();
                            }
                        }
                        refresh();
                    }
                }).setNegativeButton("Cancel", null).show();
    }

    private void play() {
        if (playZip == null || !available(this)) {
            return;
        }
        Intent i = new Intent();
        i.setClassName(this, SUPERMODEL_ACTIVITY);
        i.putExtra("zip", playZip);
        i.putExtra("forceFeedback", forceFeedback);
        i.putExtra("invertY", sp.getBoolean("swInvertY", false));
        i.putExtra("smoothness", sp.getString("swSmooth", "medium"));
        try {
            startActivity(i);
        } catch (RuntimeException e) {
            Toast.makeText(this, "Star Wars Trilogy Arcade couldn't start: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }
}
