package io.github.shredder565.retropixels;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.Vibrator;
import android.text.format.DateFormat;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import io.github.shredder565.retropixels.engine.EngineClient;
import io.github.shredder565.retropixels.engine.Fit;
import io.github.shredder565.retropixels.engine.Frame;
import io.github.shredder565.retropixels.engine.Json;
import io.github.shredder565.retropixels.engine.Pacer;
import io.github.shredder565.retropixels.engine.RunInput;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.locks.LockSupport;

/**
 * A game playing: the engine runs it, this screen shows the picture, plays
 * the sound, reads the touch controls, controllers and keyboard, and has the
 * game menu (Back): pause, reset, fast-forward, save states, volume, machine
 * settings, battery saves, controls and quit.
 */
public final class GameActivity extends Activity implements TouchControls.Listener {
    static final String EXTRA_PATH = "path";

    private static final int REQ_FIRMWARE = 1, REQ_APBASE = 2, REQ_IMPORT_SRAM = 3, REQ_EXPORT_SRAM = 4, REQ_IMPORT_STATE = 5,
            REQ_CONTROLS = 6, REQ_STORAGE = 7;

    private Prefs prefs;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService cmds = Executors.newSingleThreadExecutor();
    private GameView view;
    private TouchControls touch;
    private TextView status;
    private final Pads pads = new Pads();

    private File game;
    private volatile EngineProcess engine;
    private volatile Map<String, Object> info; // the engine's description of the game
    private String system = "", gameName = "";
    private final Map<String, Object> firmwareChosen = new java.util.LinkedHashMap<String, Object>();
    private String apBaseChosen;
    private String pendingChip;
    private int slot;
    private boolean finishing;

    // the game's thread and what it is told
    private Thread loopThread;
    private volatile boolean running, paused, menuOpen, background, failed, fast, holdFastKey, clearAudio;
    private volatile int volume;
    private volatile boolean muted;
    private AudioOut audio;
    private Vibrator phoneVibrator;
    private InputManager.InputDeviceListener deviceListener;

    // recording (Settings > Record games automatically)
    private final Object recLock = new Object();
    private GameRecorder rec;
    private volatile boolean recordWanted, recFailed;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = new Prefs(this);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        applyOrientation();
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        view = new GameView(this);
        view.setSmooth(prefs.smooth());
        root.addView(view, new FrameLayout.LayoutParams(-1, -1));
        touch = new TouchControls(this, this);
        root.addView(touch, new FrameLayout.LayoutParams(-1, -1));
        status = Ui.text(this, "Starting…", 16, Ui.FG);
        status.setGravity(Gravity.CENTER);
        root.addView(status, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        root.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                placePicture();
            }
        });
        Ui.onBack(this, new Runnable() {
            public void run() {
                openMenu();
            }
        });
        phoneVibrator = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
        InputManager im = (InputManager) getSystemService(Context.INPUT_SERVICE);
        deviceListener = new InputManager.InputDeviceListener() {
            public void onInputDeviceAdded(int id) {
            }

            public void onInputDeviceRemoved(int id) {
                pads.removed(id);
            }

            public void onInputDeviceChanged(int id) {
            }
        };
        im.registerInputDeviceListener(deviceListener, ui);
        volume = prefs.volume();
        muted = prefs.muted();

        String path = getIntent().getStringExtra(EXTRA_PATH);
        if (path == null) {
            finish();
            return;
        }
        game = new File(path);
        gameName = Library.gameName(game.getName());
        cmds.execute(new Runnable() {
            public void run() {
                startEngine();
            }
        });
    }

    private void applyOrientation() {
        String o = prefs.orientation();
        setRequestedOrientation(o.equals("landscape") ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                : o.equals("portrait") ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT : ActivityInfo.SCREEN_ORIENTATION_FULL_USER);
    }

    @SuppressWarnings("deprecation")
    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) {
            immersive();
        } else {
            pads.clear();
        }
    }

    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        placePicture();
    }

    /** Portrait with the touch controls: the picture at the top, the controls below. */
    private void placePicture() {
        if (view == null) {
            return;
        }
        boolean portrait = view.getHeight() > view.getWidth();
        boolean top = portrait && touchWanted();
        view.setTop(top);
        Fit f = view.picture();
        touch.setPictureBottom(top ? f.y + f.h : 0);
    }

    private boolean touchWanted() {
        return !prefs.touchMode().equals("never");
    }

    // ---------------- starting ----------------

    private void startEngine() {
        try {
            engine = EngineProcess.start(this);
        } catch (IOException e) {
            fatal("The emulator engine couldn't start: " + e.getMessage());
            return;
        }
        openGame();
    }

    /** Opens the game in the engine (on the command thread); asks for what's missing. */
    private void openGame() {
        EngineProcess e = engine;
        if (e == null) {
            return;
        }
        final Map<String, Object> req = Json.obj("cmd", "open", "path", game.getPath(), "dips", prefs.dips(gameName),
                "bin", prefs.binSystem(), "seat", prefs.seat(gameName));
        if (audio == null) {
            audio = new AudioOut();
        }
        req.put("sampleRate", audio.rate);
        if (!firmwareChosen.isEmpty()) {
            req.put("firmware", firmwareChosen);
        }
        if (apBaseChosen != null) {
            req.put("apBase", apBaseChosen);
        }
        Map<String, Object> r;
        try {
            r = e.client.call(req);
        } catch (IOException ex) {
            engineDied(ex);
            return;
        }
        if (!Json.bool(r, "ok")) {
            final String need = Json.str(r, "need", "");
            final String msg = Json.str(r, "error", "That game couldn't be opened.");
            if (need.equals("firmware")) {
                pendingChip = Json.str(r, "chip", "");
                final String file = Json.str(r, "file", "");
                ui.post(new Runnable() {
                    public void run() {
                        ask(msg + (file.isEmpty() ? "" : "\n\n(usually named " + file + ")"), "Choose the file", REQ_FIRMWARE);
                    }
                });
            } else if (need.equals("apbase")) {
                apBaseChosen = null;
                ui.post(new Runnable() {
                    public void run() {
                        ask(msg, "Choose the ROM", REQ_APBASE);
                    }
                });
            } else {
                firmwareChosen.clear();
                // it won't open next time either: off the list (and its copy deleted)
                Library.removeRecent(this, game.getPath());
                fatal(msg);
            }
            return;
        }
        firmwareChosen.clear();
        apBaseChosen = null;
        final Map<String, Object> rr = r;
        system = Json.str(r, "system", "");
        Map<String, Object> ctl = prefs.controls(system);
        if (!ctl.isEmpty()) {
            try {
                e.client.call(Json.obj("cmd", "setControls", "saved", ctl));
            } catch (IOException ex) {
                engineDied(ex);
                return;
            }
        }
        Library.addRecent(this, game, Json.str(r, "label", ""));
        ui.post(new Runnable() {
            public void run() {
                started(rr);
            }
        });
    }

    private void ask(String msg, String button, final int req) {
        if (isFinishing()) {
            return;
        }
        new AlertDialog.Builder(this).setMessage(msg).setCancelable(false)
                .setPositiveButton(button, new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) {
                        pick(req, null);
                    }
                })
                .setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) {
                        quit();
                    }
                }).show();
    }

    private void pick(int req, String createName) {
        Intent i = new Intent(createName == null ? Intent.ACTION_OPEN_DOCUMENT : Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(createName == null ? "*/*" : "application/octet-stream");
        if (createName != null) {
            i.putExtra(Intent.EXTRA_TITLE, createName);
        }
        try {
            startActivityForResult(i, req);
        } catch (RuntimeException e) {
            toast("There's no app on this phone to choose files with.");
        }
    }

    /** The game is open: start playing it. */
    private void started(Map<String, Object> r) {
        info = r;
        status.setVisibility(View.GONE);
        List<String> buttons = Json.strings(r, "buttons");
        view.setAspect(Json.num(r, "aspect", 4.0 / 3));
        touch.setGame(system, buttons);
        touch.setLook(prefs.touchOpacity(), prefs.touchSize(), prefs.touchVibrate());
        touch.setVisibility(touchWanted() ? View.VISIBLE : View.GONE);
        placePicture();
        recordWanted = prefs.autoRecord();
        askStorage();
        if (loopThread == null) {
            running = true;
            loopThread = new Thread(new Runnable() {
                public void run() {
                    loop();
                }
            }, "game");
            loopThread.start();
        }
    }

    // ---------------- the game's thread ----------------

    private void loop() {
        try {
            play();
        } finally {
            stopRecording(false);
        }
    }

    private void play() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY);
        double fps = Json.num(info, "fps", 60);
        Pacer pacer = new Pacer(fps, audio.bufferSeconds());
        RunInput in = new RunInput();
        Frame f = null;
        long last = System.nanoTime();
        boolean audioPaused = false;
        boolean rumbling = false;
        long lastBuzz = 0;
        while (running) {
            EngineProcess e = engine;
            long now = System.nanoTime();
            double dt = (now - last) / 1e9;
            last = now;
            if (clearAudio) {
                clearAudio = false;
                audio.clear();
            }
            if (paused || menuOpen || background || failed || e == null) {
                pacer.reset();
                if (!audioPaused) {
                    audio.pause();
                    audioPaused = true;
                }
                if (rumbling) {
                    stopRumble();
                    rumbling = false;
                }
                LockSupport.parkNanos(16000000L);
                continue;
            }
            if (audioPaused) {
                audio.resume();
                audioPaused = false;
            }
            boolean fastNow = fast || holdFastKey || touch.fastHeld();
            int n = pacer.step(dt, audio.queued(), fastNow);
            if (n > 0) {
                in.frames = n;
                in.fast = fastNow;
                in.volume = muted ? 0 : volume / 100f;
                pads.fill(in);
                if (touch.getVisibility() == View.VISIBLE && touch.shown()) {
                    touch.fill(in);
                } else {
                    in.touchButtons = 0;
                    in.touchX = in.touchY = false;
                }
                try {
                    f = e.client.run(in, f);
                } catch (IOException ex) {
                    engineDied(ex);
                    return;
                }
                if (f.failed) {
                    failed = true;
                    final String m = f.message;
                    ui.post(new Runnable() {
                        public void run() {
                            stopped(m);
                        }
                    });
                    continue;
                }
                audio.write(f.samples, f.sampleCount);
                record(f, n, fastNow);
                if (f.hasPicture) {
                    view.setFrame(f.pixels, f.width, f.height);
                }
                float lvl = prefs.rumble() && !fastNow ? f.rumble : 0;
                if (lvl < 0.05f) {
                    if (rumbling) {
                        stopRumble();
                        rumbling = false;
                    }
                } else {
                    long t = SystemClock.uptimeMillis();
                    if (!rumbling || t - lastBuzz > 70) {
                        buzz(90);
                        lastBuzz = t;
                        rumbling = true;
                    }
                }
                if (!f.message.isEmpty()) {
                    toast(f.message);
                }
            }
            double wait = fastNow ? 1 / fps : Math.max(0.001, Math.min(0.004, pacer.untilNext()));
            LockSupport.parkNanos((long) (wait * 1e9));
        }
    }

    // ---------------- recording ----------------

    /** Hands a run's picture and sound to the recording, starting one when there is none (the game's thread). */
    private void record(Frame f, int frames, boolean fastNow) {
        GameRecorder r;
        synchronized (recLock) {
            r = rec;
            if (r != null && r.dead()) {
                rec = r = null;
            }
            if (r == null && recordWanted && !recFailed && !background && !finishing && f.hasPicture) {
                rec = r = GameRecorder.start(this, Json.str(info, "name", gameName), Json.num(info, "aspect", 4.0 / 3),
                        Json.num(info, "fps", 60), audio.rate, recListener);
            }
        }
        if (r != null) {
            r.frame(f.hasPicture ? f.pixels : null, f.width, f.height, frames, fastNow, f.samples, f.sampleCount);
        }
    }

    /** Ends the recording (the game closed or went to the background): the file is finished and saved. */
    private void stopRecording(boolean announce) {
        GameRecorder r;
        synchronized (recLock) {
            r = rec;
            rec = null;
        }
        if (r != null) {
            r.stop(announce);
        }
    }

    private final GameRecorder.Listener recListener = new GameRecorder.Listener() {
        public void failed(final String why) {
            if (recFailed) {
                return;
            }
            recFailed = true; // not tried again for this game: it plays on without
            appToast("Recording stopped: " + why + ". The game plays on without it.");
        }

        public void saved(String where, boolean announce) {
            if (announce) {
                appToast("Recording saved in " + where);
            }
        }
    };

    /** A toast that still shows after this screen has closed. */
    private void appToast(final String msg) {
        final Context app = getApplicationContext();
        ui.post(new Runnable() {
            public void run() {
                Toast.makeText(app, msg, Toast.LENGTH_SHORT).show();
            }
        });
    }

    /** Android 7-9: the public Movies folder needs storage access, asked for once (without it, recordings go to the app's own Movies folder). */
    private void askStorage() {
        if (Build.VERSION.SDK_INT >= 29 || !recordWanted || prefs.askedStorage()
                || checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        prefs.setAskedStorage();
        requestPermissions(new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_STORAGE);
    }

    /** The controller that rumbles: the first controller's motor, else the phone's (with the touch controls). */
    private Vibrator vibrator() {
        int id = pads.firstDevice();
        if (id != -1) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null) {
                Vibrator v = d.getVibrator();
                if (v != null && v.hasVibrator()) {
                    return v;
                }
            }
            return null;
        }
        return phoneVibrator != null && phoneVibrator.hasVibrator() ? phoneVibrator : null;
    }

    @SuppressWarnings("deprecation")
    private void buzz(long ms) {
        Vibrator v = vibrator();
        if (v != null) {
            try {
                v.vibrate(ms);
            } catch (RuntimeException ignored) {
                // no permission or no motor
            }
        }
    }

    private void stopRumble() {
        Vibrator v = vibrator();
        if (v != null) {
            try {
                v.cancel();
            } catch (RuntimeException ignored) {
                // nothing to stop
            }
        }
    }

    // ---------------- when things go wrong ----------------

    private void engineDied(IOException e) {
        if (finishing) {
            return;
        }
        EngineProcess p = engine;
        String tail = p != null ? p.tailNote() : "";
        fatal("The emulator engine stopped: " + e.getMessage() + tail);
    }

    private void fatal(final String msg) {
        running = false;
        ui.post(new Runnable() {
            public void run() {
                if (isFinishing()) {
                    return;
                }
                status.setVisibility(View.GONE);
                new AlertDialog.Builder(GameActivity.this).setTitle("RetroPixels").setMessage(msg).setCancelable(false)
                        .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface d, int w) {
                                quit();
                            }
                        }).show();
            }
        });
    }

    /** The emulator stopped (the core failed): load a state, reset or quit. */
    private void stopped(String msg) {
        new AlertDialog.Builder(this).setTitle("The game stopped").setMessage(msg + "\n\nLoad a save state, reset the game or quit.")
                .setPositiveButton("Menu", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) {
                        openMenu();
                    }
                })
                .setNegativeButton("Quit", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) {
                        quit();
                    }
                }).show();
    }

    private void toast(final String msg) {
        ui.post(new Runnable() {
            public void run() {
                Toast.makeText(GameActivity.this, msg, Toast.LENGTH_SHORT).show();
            }
        });
    }

    // ---------------- commands ----------------

    interface Done {
        void done(Map<String, Object> reply);
    }

    /** Runs an engine command on the command thread; done (on the UI thread) gets the reply when it's ok. */
    private void command(final Map<String, Object> req, final String okMessage, final Done done) {
        cmds.execute(new Runnable() {
            public void run() {
                EngineProcess e = engine;
                if (e == null) {
                    return;
                }
                final Map<String, Object> r;
                try {
                    r = e.client.call(req);
                } catch (IOException ex) {
                    engineDied(ex);
                    return;
                }
                if (!Json.bool(r, "ok")) {
                    toast(Json.str(r, "error", "That didn't work"));
                    return;
                }
                if (okMessage != null) {
                    toast(okMessage);
                }
                if (done != null) {
                    ui.post(new Runnable() {
                        public void run() {
                            done.done(r);
                        }
                    });
                }
            }
        });
    }

    private void saveState(final int s) {
        slot = s;
        command(Json.obj("cmd", "save", "slot", s), "State saved to slot " + (s + 1), null);
    }

    private void loadState(final int s) {
        slot = s;
        command(Json.obj("cmd", "load", "slot", s), "Slot " + (s + 1) + " loaded", new Done() {
            public void done(Map<String, Object> r) {
                failed = false;
                clearAudio = true;
            }
        });
    }

    private void reset() {
        command(Json.obj("cmd", "reset"), null, new Done() {
            public void done(Map<String, Object> r) {
                failed = false;
                clearAudio = true;
            }
        });
    }

    // ---------------- the menu ----------------

    @Override
    public void onMenu() {
        openMenu();
    }

    @Override
    public Fit paddleArea() {
        return view.picture();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        openMenu();
    }

    private AlertDialog menu;

    private void openMenu() {
        if (info == null) {
            quit();
            return;
        }
        if (menu != null && menu.isShowing()) {
            return;
        }
        menuOpen = true;
        pads.clear();
        final List<String> items = new ArrayList<String>();
        final List<Runnable> acts = new ArrayList<Runnable>();
        add(items, acts, "Resume", new Runnable() {
            public void run() {
            }
        });
        add(items, acts, paused ? "Unpause" : "Pause", new Runnable() {
            public void run() {
                paused = !paused;
            }
        });
        add(items, acts, fast ? "Fast-forward: on (tap to stop)" : "Fast-forward", new Runnable() {
            public void run() {
                fast = !fast;
            }
        });
        add(items, acts, "Save state…", new Runnable() {
            public void run() {
                slots(true);
            }
        });
        add(items, acts, "Load state…", new Runnable() {
            public void run() {
                slots(false);
            }
        });
        add(items, acts, "Undo last load", new Runnable() {
            public void run() {
                command(Json.obj("cmd", "undo"), "Back to before the load", new Done() {
                    public void done(Map<String, Object> r) {
                        clearAudio = true;
                    }
                });
            }
        });
        add(items, acts, "Reset", new Runnable() {
            public void run() {
                reset();
            }
        });
        add(items, acts, "Volume (" + (muted ? "muted" : volume + "%") + ")…", new Runnable() {
            public void run() {
                volumeDialog();
            }
        });
        if (!Json.list(info, "dips").isEmpty()) {
            add(items, acts, "Machine settings (DIP switches)…", new Runnable() {
                public void run() {
                    dipsDialog();
                }
            });
        }
        if (Json.integer(info, "seats", 0) > 0) {
            add(items, acts, "Player…", new Runnable() {
                public void run() {
                    seatDialog();
                }
            });
        }
        if (Json.bool(info, "hasSRAM")) {
            add(items, acts, "Battery save…", new Runnable() {
                public void run() {
                    batteryDialog();
                }
            });
        }
        add(items, acts, "Import a save state file…", new Runnable() {
            public void run() {
                pick(REQ_IMPORT_STATE, null);
            }
        });
        add(items, acts, "Controls…", new Runnable() {
            public void run() {
                Intent i = new Intent(GameActivity.this, RemapActivity.class);
                i.putExtra(RemapActivity.EXTRA_SYSTEM, system);
                startActivityForResult(i, REQ_CONTROLS);
            }
        });
        if (touchWanted()) {
            add(items, acts, touch.shown() ? "Hide the touch controls" : "Show the touch controls", new Runnable() {
                public void run() {
                    touch.setShown(!touch.shown());
                }
            });
        }
        add(items, acts, prefs.smooth() ? "Sharp pixels" : "Smooth pixels", new Runnable() {
            public void run() {
                prefs.setSmooth(!prefs.smooth());
                view.setSmooth(prefs.smooth());
            }
        });
        add(items, acts, "Quit to the start screen", new Runnable() {
            public void run() {
                quit();
            }
        });
        String title = Json.str(info, "name", gameName) + " — " + Json.str(info, "label", "");
        menu = new AlertDialog.Builder(this).setTitle(title).setItems(items.toArray(new String[0]), new DialogInterface.OnClickListener() {
            public void onClick(DialogInterface d, int w) {
                acts.get(w).run();
            }
        }).create();
        menu.setOnDismissListener(new DialogInterface.OnDismissListener() {
            public void onDismiss(DialogInterface d) {
                menuDone();
            }
        });
        menu.show();
    }

    private void menuDone() {
        if (sub == null || !sub.isShowing()) {
            menuOpen = false;
            pads.clear();
            immersive();
        }
    }

    private AlertDialog sub;

    private void showSub(AlertDialog.Builder b) {
        menuOpen = true;
        sub = b.create();
        sub.setOnDismissListener(new DialogInterface.OnDismissListener() {
            public void onDismiss(DialogInterface d) {
                sub = null;
                menuDone();
            }
        });
        sub.show();
    }

    private static void add(List<String> items, List<Runnable> acts, String label, Runnable r) {
        items.add(label);
        acts.add(r);
    }

    private void slots(final boolean save) {
        command(Json.obj("cmd", "slots"), null, new Done() {
            public void done(Map<String, Object> r) {
                List<Object> list = Json.list(r, "slots");
                String[] labels = new String[list.size()];
                for (int i = 0; i < labels.length; i++) {
                    @SuppressWarnings("unchecked")
                    Map<String, Object> s = (Map<String, Object>) list.get(i);
                    labels[i] = "Slot " + (i + 1) + (Json.bool(s, "used") ? " — " + when((long) Json.num(s, "time", 0)) : " (empty)")
                            + (i == slot ? "  •" : "");
                }
                showSub(new AlertDialog.Builder(GameActivity.this).setTitle(save ? "Save state to" : "Load state from")
                        .setItems(labels, new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface d, int w) {
                                if (save) {
                                    saveState(w);
                                } else {
                                    loadState(w);
                                }
                            }
                        }));
            }
        });
    }

    private String when(long ms) {
        Date d = new Date(ms);
        boolean today = DateFormat.format("yyyyMMdd", d).equals(DateFormat.format("yyyyMMdd", new Date()));
        return (today ? "" : DateFormat.getMediumDateFormat(this).format(d) + " ") + DateFormat.getTimeFormat(this).format(d);
    }

    private void volumeDialog() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        int p = Ui.dp(this, 20);
        l.setPadding(p, p, p, p);
        final TextView t = Ui.text(this, volume + "%", 16, Ui.FG);
        SeekBar s = new SeekBar(this);
        s.setMax(100);
        s.setProgress(volume);
        s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b, int v, boolean user) {
                volume = v;
                t.setText(v + "%");
                prefs.setVolume(v);
            }

            public void onStartTrackingTouch(SeekBar b) {
            }

            public void onStopTrackingTouch(SeekBar b) {
            }
        });
        l.addView(t);
        l.addView(s);
        showSub(new AlertDialog.Builder(this).setTitle("Volume").setView(l)
                .setPositiveButton(muted ? "Unmute" : "Mute", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface d, int w) {
                        muted = !muted;
                        prefs.setMuted(muted);
                    }
                }).setNegativeButton("Done", null));
    }

    @SuppressWarnings("unchecked")
    private void dipsDialog() {
        command(Json.obj("cmd", "dips"), null, new Done() {
            public void done(Map<String, Object> r) {
                final List<Object> dips = Json.list(r, "dips");
                String[] labels = new String[dips.size()];
                for (int i = 0; i < labels.length; i++) {
                    Map<String, Object> d = (Map<String, Object>) dips.get(i);
                    String cur = "";
                    for (Object o : Json.list(d, "options")) {
                        Map<String, Object> op = (Map<String, Object>) o;
                        if (Json.integer(op, "value", -1) == Json.integer(d, "value", -2)) {
                            cur = Json.str(op, "label", "");
                        }
                    }
                    labels[i] = Json.str(d, "label", "") + ": " + cur;
                }
                showSub(new AlertDialog.Builder(GameActivity.this).setTitle("Machine settings (Reset after a change)")
                        .setItems(labels, new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface di, int w) {
                                dipOptions((Map<String, Object>) dips.get(w));
                            }
                        }));
            }
        });
    }

    @SuppressWarnings("unchecked")
    private void dipOptions(final Map<String, Object> d) {
        final List<Object> opts = Json.list(d, "options");
        String[] labels = new String[opts.size()];
        int checked = -1;
        for (int i = 0; i < labels.length; i++) {
            Map<String, Object> op = (Map<String, Object>) opts.get(i);
            labels[i] = Json.str(op, "label", "");
            if (Json.integer(op, "value", -1) == Json.integer(d, "value", -2)) {
                checked = i;
            }
        }
        showSub(new AlertDialog.Builder(this).setTitle(Json.str(d, "label", ""))
                .setSingleChoiceItems(labels, checked, new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface di, int w) {
                        Map<String, Object> op = (Map<String, Object>) opts.get(w);
                        final String id = Json.str(d, "id", "");
                        final int v = Json.integer(op, "value", 0);
                        final String label = Json.str(d, "label", "") + ": " + Json.str(op, "label", "");
                        command(Json.obj("cmd", "dip", "id", id, "value", v), label + " (Reset if it doesn't take effect)", new Done() {
                            public void done(Map<String, Object> r) {
                                prefs.setDip(gameName, id, v);
                                info.put("dips", r.get("dips"));
                            }
                        });
                        di.dismiss();
                    }
                }));
    }

    private void seatDialog() {
        List<String> names = Json.strings(info, "seatNames");
        int cur = Json.integer(info, "seat", 0);
        showSub(new AlertDialog.Builder(this).setTitle("You play as")
                .setSingleChoiceItems(names.toArray(new String[0]), cur, new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface di, int w) {
                        final int n = w;
                        command(Json.obj("cmd", "seat", "value", n), null, new Done() {
                            public void done(Map<String, Object> r) {
                                int s = Json.integer(r, "seat", 0);
                                prefs.setSeat(gameName, s);
                                info.put("seat", s);
                                List<String> names = Json.strings(info, "seatNames");
                                if (s < names.size()) {
                                    Toast.makeText(GameActivity.this, "You are p" + names.get(s).substring(1), Toast.LENGTH_SHORT).show();
                                }
                            }
                        });
                        di.dismiss();
                    }
                }));
    }

    private void batteryDialog() {
        showSub(new AlertDialog.Builder(this).setTitle("Battery save")
                .setItems(new String[] {"Export (.sav)…", "Import a .sav / .srm… (the game restarts; the old save is kept as .bak)"},
                        new DialogInterface.OnClickListener() {
                            public void onClick(DialogInterface d, int w) {
                                if (w == 0) {
                                    command(Json.obj("cmd", "flush"), null, null);
                                    pick(REQ_EXPORT_SRAM, gameName + ".sav");
                                } else {
                                    pick(REQ_IMPORT_SRAM, null);
                                }
                            }
                        }));
    }

    // ---------------- files chosen ----------------

    @Override
    protected void onActivityResult(final int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req == REQ_CONTROLS) {
            command(Json.obj("cmd", "setControls", "saved", prefs.controls(system)), null, null);
            return;
        }
        final Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            if (req == REQ_FIRMWARE || req == REQ_APBASE) {
                quit();
            }
            return;
        }
        cmds.execute(new Runnable() {
            public void run() {
                chosen(req, uri);
            }
        });
    }

    /** A file the player chose (on the command thread). */
    private void chosen(int req, Uri uri) {
        File cache = getCacheDir();
        try {
            switch (req) {
                case REQ_FIRMWARE: {
                    File f = new File(cache, "firmware-" + Library.safeFileName(pendingChip));
                    Library.copy(this, uri, f, 16 << 20);
                    firmwareChosen.put(pendingChip, f.getPath());
                    openGame();
                    break;
                }
                case REQ_APBASE: {
                    File f = new File(cache, "archipelago-rom");
                    Library.copy(this, uri, f, 64 << 20);
                    apBaseChosen = f.getPath();
                    openGame();
                    break;
                }
                case REQ_IMPORT_STATE: {
                    File f = new File(cache, "import.state");
                    Library.copy(this, uri, f, 64 << 20);
                    command(Json.obj("cmd", "importState", "path", f.getPath()), "Save state loaded (save it to a slot to keep it)", new Done() {
                        public void done(Map<String, Object> r) {
                            failed = false;
                            clearAudio = true;
                        }
                    });
                    break;
                }
                case REQ_IMPORT_SRAM: {
                    File f = new File(cache, "import.sav");
                    Library.copy(this, uri, f, 8 << 20);
                    command(Json.obj("cmd", "importSRAM", "path", f.getPath()), "Battery save imported; the game has restarted", new Done() {
                        public void done(Map<String, Object> r) {
                            info = r;
                            failed = false;
                            clearAudio = true;
                        }
                    });
                    break;
                }
                case REQ_EXPORT_SRAM: {
                    final File f = new File(cache, "export.sav");
                    EngineProcess e = engine;
                    if (e == null) {
                        return;
                    }
                    Map<String, Object> r = e.client.call(Json.obj("cmd", "exportSRAM", "path", f.getPath()));
                    if (!Json.bool(r, "ok")) {
                        toast(Json.str(r, "error", "Couldn't export the battery save"));
                        return;
                    }
                    OutputStream out = getContentResolver().openOutputStream(uri);
                    if (out == null) {
                        throw new IOException("couldn't write there");
                    }
                    InputStream in = new FileInputStream(f);
                    try {
                        byte[] b = new byte[1 << 16];
                        int n;
                        while ((n = in.read(b)) > 0) {
                            out.write(b, 0, n);
                        }
                    } finally {
                        in.close();
                        out.close();
                    }
                    toast("Battery save exported");
                    break;
                }
                default:
                    break;
            }
        } catch (EngineClient.EngineError e) {
            toast(e.getMessage());
        } catch (IOException e) {
            if (req == REQ_FIRMWARE || req == REQ_APBASE) {
                fatal("Couldn't read that file: " + e.getMessage());
            } else {
                toast("Couldn't do that: " + e.getMessage());
            }
        }
    }

    // ---------------- controllers and keys ----------------

    private void padUsed() {
        if (prefs.touchMode().equals("auto") && touch.shown()) {
            touch.setShown(false);
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode();
        switch (k) {
            case KeyEvent.KEYCODE_BACK:
            case KeyEvent.KEYCODE_BUTTON_MODE:
            case KeyEvent.KEYCODE_ESCAPE:
            case KeyEvent.KEYCODE_MENU:
                if (e.getAction() == KeyEvent.ACTION_UP && !e.isCanceled()) {
                    openMenu();
                }
                return true;
            case KeyEvent.KEYCODE_VOLUME_UP:
            case KeyEvent.KEYCODE_VOLUME_DOWN:
            case KeyEvent.KEYCODE_VOLUME_MUTE:
                return super.dispatchKeyEvent(e);
            default:
                break;
        }
        if (menuOpen || info == null) {
            return super.dispatchKeyEvent(e);
        }
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0;
        if (KeyNames.fromPad(e)) {
            if (pads.padKey(e)) {
                padUsed();
                // click the right stick to save a state, the left stick to load it (as in the native app)
                if (down && k == KeyEvent.KEYCODE_BUTTON_THUMBR) {
                    saveState(slot);
                } else if (down && k == KeyEvent.KEYCODE_BUTTON_THUMBL) {
                    loadState(slot);
                }
                return true;
            }
            return super.dispatchKeyEvent(e);
        }
        // the keyboard: the native app's shortcuts, then the game's keys
        if (k == KeyEvent.KEYCODE_TAB) {
            holdFastKey = e.getAction() == KeyEvent.ACTION_DOWN;
            return true;
        }
        if (down && k == KeyEvent.KEYCODE_F2) {
            saveState(slot);
            return true;
        }
        if (down && k == KeyEvent.KEYCODE_F4) {
            loadState(slot);
            return true;
        }
        if (down && k == KeyEvent.KEYCODE_F3) {
            slot = (slot + (e.isShiftPressed() ? 3 : 1)) % 4;
            toast("Save state slot " + (slot + 1));
            return true;
        }
        if (pads.keyboardKey(e)) {
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (!menuOpen && pads.motion(e)) {
            if (pads.padInUse()) {
                padUsed();
            }
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    // ---------------- coming and going ----------------

    @Override
    protected void onResume() {
        super.onResume();
        background = false;
        recordWanted = prefs.autoRecord();
        immersive();
        if (info != null) {
            touch.setLook(prefs.touchOpacity(), prefs.touchSize(), prefs.touchVibrate());
            view.setSmooth(prefs.smooth());
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        background = true;
        pads.clear();
        stopRecording(isFinishing()); // a playable file every time; a new one starts when the game comes back
        if (info != null) {
            command(Json.obj("cmd", "flush"), null, null); // the battery save, now
        }
    }

    private void quit() {
        if (finishing) {
            return;
        }
        finishing = true;
        finish();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        finishing = true;
        running = false;
        stopRecording(false);
        if (deviceListener != null) {
            ((InputManager) getSystemService(Context.INPUT_SERVICE)).unregisterInputDeviceListener(deviceListener);
        }
        final Thread t = loopThread;
        final AudioOut a = audio;
        cmds.execute(new Runnable() {
            public void run() {
                if (t != null) {
                    try {
                        t.join(2000);
                    } catch (InterruptedException ignored) {
                        Thread.currentThread().interrupt();
                    }
                }
                EngineProcess e = engine;
                engine = null;
                if (e != null) {
                    try {
                        e.client.call(Json.obj("cmd", "close"));
                    } catch (IOException ignored) {
                        // gone already
                    }
                    e.stop();
                }
                if (a != null) {
                    a.release();
                }
            }
        });
        cmds.shutdown();
    }
}
