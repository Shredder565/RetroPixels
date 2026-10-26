package io.github.shredder565.retropixels;

import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Bundle;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

import io.github.shredder565.retropixels.engine.StarWars;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Star Wars Trilogy Arcade, played by RetroPixels' Supermodel (Sega Model 3)
 * in libsupermodel.so, an SDL program (OSD/Android in supermodel/Src). It
 * runs in a process of its own (":supermodel"), which ends with the game,
 * so each game starts the program afresh as on a PC.
 *
 * Before SDL starts the program this puts Supermodel's own files
 * (Config/Games.xml, Config/Music.xml, Assets/, from the APK's
 * assets/supermodel) in the app's storage, writes Supermodel.ini (while it
 * is still RetroPixels' own) and sets SUPERMODEL_HOME and RP_FLIGHTSTICK. The
 * game's start-up runs unseen at full speed (-rp-fast-boot), as on Windows.
 *
 * Controls: touch controls drawn over the sides of the 4:3 picture (a stick
 * that appears where the left thumb lands, Fire and Event on the right,
 * Coin and Start at the top), which the program sees as a controller of
 * their own, and any Xbox-style Bluetooth or USB controller (stick aims, A
 * fires, B is the event button, Back inserts a coin, Start starts). With
 * Settings > Touch controls on "auto" they hide while a controller is used.
 *
 * Recording (Settings > Record games automatically): the program hands each
 * frame's picture and sound to recordFrame(), which feeds the app's
 * GameRecorder, the same H.264 / AAC .mp4 recorder the other games use.
 */
public final class SupermodelActivity extends SDLActivity {
    static final String TAG = "RetroPixels";
    static final String EXTRA_ZIP = "zip";
    static final String EXTRA_FORCE_FEEDBACK = "forceFeedback";
    static final String EXTRA_INVERT_Y = "invertY";
    static final String EXTRA_SMOOTHNESS = "smoothness";

    /** Written when a recording is saved, for the Star Wars screen to tell the player (this process ends with the game). */
    static final String SAVED_NOTE = "recording-saved.txt";

    private static native boolean nativePrepare(String dataDir);

    private static native void nativeTouch(float x, float y, int buttons);

    private static native void nativeSetRecording(boolean on);

    private static final Object recLock = new Object();
    private static SupermodelActivity current;

    private Prefs prefs;
    private boolean ready;
    private GameRecorder rec;
    private boolean recFailed, recWanted;
    private TouchPad touch;

    // ---------------- files ----------------

    static File root(Context c) {
        return new File(c.getFilesDir(), "supermodel");
    }

    /** Supermodel's own files, from the APK, again whenever the app changes. */
    private static File dataDir(Context c) throws IOException {
        File data = new File(root(c), "data");
        File stamp = new File(data, ".from");
        String want;
        try {
            want = String.valueOf(c.getPackageManager().getPackageInfo(c.getPackageName(), 0).lastUpdateTime);
        } catch (Exception e) {
            want = "?";
        }
        if (stamp.isFile() && want.equals(read(stamp))) {
            return data;
        }
        deleteTree(data);
        copyAssets(c.getAssets(), "supermodel", data);
        if (!new File(data, "Config/Games.xml").isFile()) {
            throw new IOException("Supermodel's Games.xml isn't in this build of the app");
        }
        write(stamp, want);
        return data;
    }

    private static void copyAssets(AssetManager am, String from, File to) throws IOException {
        String[] list = am.list(from);
        if (list == null || list.length == 0) { // a file
            to.getParentFile().mkdirs();
            InputStream in = am.open(from);
            try {
                OutputStream out = new FileOutputStream(to);
                try {
                    byte[] b = new byte[1 << 16];
                    int n;
                    while ((n = in.read(b)) > 0) {
                        out.write(b, 0, n);
                    }
                } finally {
                    out.close();
                }
            } finally {
                in.close();
            }
            return;
        }
        to.mkdirs();
        for (String name : list) {
            copyAssets(am, from + "/" + name, new File(to, name));
        }
    }

    private static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) {
            for (File g : list) {
                deleteTree(g);
            }
        }
        f.delete();
    }

    static String read(File f) {
        try {
            InputStream in = new java.io.FileInputStream(f);
            try {
                byte[] b = new byte[(int) Math.min(f.length(), 1 << 20)];
                int n = 0, r;
                while (n < b.length && (r = in.read(b, n, b.length - n)) > 0) {
                    n += r;
                }
                return new String(b, 0, n, "UTF-8");
            } finally {
                in.close();
            }
        } catch (IOException e) {
            return null;
        }
    }

    static void write(File f, String s) throws IOException {
        f.getParentFile().mkdirs();
        File tmp = new File(f.getPath() + ".tmp");
        OutputStream out = new FileOutputStream(tmp);
        try {
            out.write(s.getBytes("UTF-8"));
        } finally {
            out.close();
        }
        if (!tmp.renameTo(f)) {
            throw new IOException("couldn't write " + f);
        }
    }

    /** Supermodel.ini, while its first line says it is still RetroPixels' own. */
    private static void writeIni(File home, boolean invertY, boolean flightStick) throws IOException {
        File ini = new File(home, "Config/Supermodel.ini");
        String now = ini.isFile() ? read(ini) : null;
        if (now != null && !now.startsWith(StarWars.INI_MARKER)) {
            return; // the player's own
        }
        write(ini, StarWars.ini(invertY, flightStick));
    }

    // ---------------- SDL ----------------

    private String dataPath;
    private String[] args = new String[0];

    @Override
    protected void onCreate(Bundle saved) {
        prefs = new Prefs(this);
        Intent in = getIntent();
        String zip = in.getStringExtra(EXTRA_ZIP);
        String problem = null;
        try {
            if (zip == null || !new File(zip).isFile()) {
                throw new IOException("the game's file isn't there");
            }
            dataPath = dataDir(this).getPath();
            File home = new File(root(this), "home");
            home.mkdirs();
            writeIni(home, in.getBooleanExtra(EXTRA_INVERT_Y, false), true);
            Os.setenv("SUPERMODEL_HOME", home.getPath(), true);
            Os.setenv("RP_FLIGHTSTICK", StarWars.flightStick(true, in.getStringExtra(EXTRA_SMOOTHNESS)), true);
            List<String> a = StarWars.args(zip, in.getBooleanExtra(EXTRA_FORCE_FEEDBACK, false));
            args = a.toArray(new String[0]);
        } catch (IOException | ErrnoException | RuntimeException e) {
            problem = e.getMessage();
        }
        super.onCreate(saved);
        if (mBrokenLibraries) {
            return; // SDL says so
        }
        if (problem == null && !nativePrepare(dataPath)) {
            problem = "Supermodel's files couldn't be opened";
        }
        if (problem != null) {
            Log.e(TAG, "Star Wars Trilogy Arcade: " + problem);
            android.widget.Toast.makeText(this, "Star Wars Trilogy Arcade can't start: " + problem, android.widget.Toast.LENGTH_LONG).show();
            args = null;
            finish();
            return;
        }
        ready = true;
        synchronized (recLock) {
            current = this;
        }
        touch = new TouchPad(this);
        addContentView(touch, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        touch.setVisibility("never".equals(prefs.touchMode()) ? View.GONE : View.VISIBLE);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "supermodel"};
    }

    @Override
    protected String[] getArguments() {
        return args != null ? args : new String[0];
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (ready) {
            synchronized (recLock) {
                recWanted = prefs.autoRecord() && !recFailed;
            }
            nativeSetRecording(recWanted);
        }
    }

    @Override
    protected void onPause() {
        if (ready) {
            nativeSetRecording(false);
            stopRecording(isFinishing());
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        boolean wasReady = ready;
        if (ready) {
            nativeSetRecording(false);
            stopRecording(true);
        }
        super.onDestroy(); // waits for the program to end
        GameRecorder r;
        synchronized (recLock) {
            r = lastRec;
            if (current == this) {
                current = null;
            }
        }
        if (r != null) {
            r.awaitFinished(15000); // the .mp4 is finished before the process goes
        }
        if (wasReady && isFinishing()) {
            // The program's state (and SDL's) lives in this process: the next
            // game starts in a new one.
            Process.killProcess(Process.myPid());
        }
    }

    // a controller used: the touch controls step aside (Settings > Touch controls: auto)
    private void controllerUsed(InputDevice d, int source) {
        if (touch == null || !"auto".equals(prefs.touchMode())) {
            return;
        }
        boolean pad = (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        if (pad && d != null && !d.isVirtual() && touch.getVisibility() == View.VISIBLE) {
            touch.release();
            touch.setVisibility(View.GONE);
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        controllerUsed(e.getDevice(), e.getSource());
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        controllerUsed(e.getDevice(), e.getSource());
        return super.dispatchGenericMotionEvent(e);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        // touching the screen again brings the touch controls back
        if (touch != null && touch.getVisibility() == View.GONE && !"never".equals(prefs.touchMode())
                && e.getActionMasked() == MotionEvent.ACTION_DOWN) {
            touch.setVisibility(View.VISIBLE);
        }
        return super.dispatchTouchEvent(e);
    }

    // ---------------- recording ----------------

    private GameRecorder lastRec;

    /** From the program's thread: one frame's picture (RGBA, top row first) and its sound (s16 stereo). */
    @SuppressWarnings("unused")
    static void recordFrame(byte[] rgba, int w, int h, short[] pcm, int samples) {
        GameRecorder r;
        synchronized (recLock) {
            SupermodelActivity a = current;
            if (a == null) {
                return;
            }
            r = a.rec;
            if (r != null && r.dead()) {
                a.rec = r = null;
            }
            if (r == null && a.recWanted && !a.recFailed && !a.isFinishing()) {
                a.rec = r = GameRecorder.start(a, StarWars.TITLE, 4.0 / 3, 60, 44100, a.recListener);
                a.lastRec = r;
            }
        }
        if (r != null) {
            r.frame(rgba, w, h, 1, false, pcm, samples);
        }
    }

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
        public void failed(String why) {
            synchronized (recLock) {
                recFailed = true; // not tried again for this game
                recWanted = false;
            }
            nativeSetRecording(false);
            Log.w(TAG, "Star Wars Trilogy Arcade recording stopped: " + why);
        }

        public void saved(String where, boolean announce) {
            try {
                write(new File(root(SupermodelActivity.this), SAVED_NOTE), where);
            } catch (IOException ignored) {
                // only the message is lost
            }
        }
    };

    // ---------------- touch controls ----------------

    /** Bits of nativeTouch's buttons. */
    static final int FIRE = 1, EVENT = 2, COIN = 4, START = 8;

    /**
     * The touch controls: a stick that appears where the left thumb lands
     * (anywhere on the left half), Fire and Event buttons at the bottom
     * right, Coin and Start at the top. Each finger is followed on its own.
     */
    final class TouchPad extends View {
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
                label = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF fire = new RectF(), event = new RectF(), coin = new RectF(), start = new RectF();
        private final int alpha;
        private final float scale;
        private final boolean vibrate;

        private int stickId = -1;
        private float baseX, baseY, knobX, knobY;
        private final List<int[]> held = new ArrayList<int[]>(); // pointer id, button bit
        private int lastButtons = -1;
        private float lastX = 2, lastY = 2;

        TouchPad(Context c) {
            super(c);
            alpha = prefs.touchOpacity() * 255 / 100;
            scale = prefs.touchSize() / 100f;
            vibrate = prefs.touchVibrate();
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(Ui.dp(c, 2));
            label.setTextAlign(Paint.Align.CENTER);
            label.setTextSize(Ui.dp(c, 14 * scale));
            setHapticFeedbackEnabled(true);
        }

        private float r() {
            return Ui.dp(getContext(), 56 * scale);
        }

        @Override
        protected void onSizeChanged(int w, int h, int ow, int oh) {
            float b = Ui.dp(getContext(), 40 * scale), m = Ui.dp(getContext(), 20);
            float cx = w - m - b * 1.2f, cy = h - m - b * 1.2f;
            fire.set(cx - b, cy - b, cx + b, cy + b);
            float ex = cx - b * 2.6f, ey = cy + b * 0.25f, eb = b * 0.75f;
            event.set(ex - eb, ey - eb, ex + eb, ey + eb);
            float sw = Ui.dp(getContext(), 64 * scale), sh = Ui.dp(getContext(), 32 * scale);
            coin.set(m, m, m + sw, m + sh);
            start.set(w - m - sw, m, w - m, m + sh);
        }

        private int buttonAt(float x, float y) {
            float slop = Ui.dp(getContext(), 8);
            if (hit(fire, x, y, slop)) {
                return FIRE;
            }
            if (hit(event, x, y, slop)) {
                return EVENT;
            }
            if (hit(coin, x, y, slop)) {
                return COIN;
            }
            if (hit(start, x, y, slop)) {
                return START;
            }
            return 0;
        }

        private boolean hit(RectF r, float x, float y, float slop) {
            return x >= r.left - slop && x <= r.right + slop && y >= r.top - slop && y <= r.bottom + slop;
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            int a = e.getActionMasked();
            int i = e.getActionIndex();
            switch (a) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN: {
                    int id = e.getPointerId(i);
                    float x = e.getX(i), y = e.getY(i);
                    int b = buttonAt(x, y);
                    if (b != 0) {
                        held.add(new int[] {id, b});
                        if (vibrate) {
                            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                        }
                    } else if (stickId == -1 && x < getWidth() / 2f) {
                        stickId = id;
                        baseX = knobX = x;
                        baseY = knobY = y;
                    }
                    break;
                }
                case MotionEvent.ACTION_MOVE:
                    for (int p = 0; p < e.getPointerCount(); p++) {
                        if (e.getPointerId(p) == stickId) {
                            knobX = e.getX(p);
                            knobY = e.getY(p);
                        }
                    }
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP:
                    lift(e.getPointerId(i));
                    break;
                case MotionEvent.ACTION_CANCEL:
                    release();
                    return true;
                default:
                    break;
            }
            send();
            return true;
        }

        private void lift(int id) {
            if (id == stickId) {
                stickId = -1;
            }
            for (int k = held.size() - 1; k >= 0; k--) {
                if (held.get(k)[0] == id) {
                    held.remove(k);
                }
            }
        }

        /** Lets go of everything (the controls hidden, or the touch cancelled). */
        void release() {
            stickId = -1;
            held.clear();
            send();
        }

        private void send() {
            float x = 0, y = 0;
            if (stickId != -1) {
                float r = r();
                x = Math.max(-1f, Math.min(1f, (knobX - baseX) / r));
                y = Math.max(-1f, Math.min(1f, (knobY - baseY) / r));
            }
            int buttons = 0;
            for (int[] h : held) {
                buttons |= h[1];
            }
            if (buttons != lastButtons || x != lastX || y != lastY) {
                lastButtons = buttons;
                lastX = x;
                lastY = y;
                nativeTouch(x, y, buttons);
                invalidate();
            }
        }

        @Override
        protected void onDraw(Canvas c) {
            int buttons = Math.max(0, lastButtons);
            if (stickId != -1) {
                float r = r();
                paint(Color.WHITE, alpha / 3, false);
                c.drawCircle(baseX, baseY, r, fill);
                paint(Color.WHITE, alpha, true);
                c.drawCircle(baseX, baseY, r, line);
                float dx = knobX - baseX, dy = knobY - baseY, d = (float) Math.hypot(dx, dy);
                if (d > r) {
                    dx = dx * r / d;
                    dy = dy * r / d;
                }
                paint(Color.WHITE, alpha, false);
                c.drawCircle(baseX + dx, baseY + dy, r * 0.45f, fill);
            } else {
                label.setColor(Color.argb(alpha, 255, 255, 255));
                c.drawText("Drag here to aim", getWidth() * 0.18f, getHeight() * 0.62f, label);
            }
            button(c, fire, "FIRE", 0xffe53935, (buttons & FIRE) != 0, true);
            button(c, event, "EVENT", 0xff1e88e5, (buttons & EVENT) != 0, true);
            button(c, coin, "COIN", Color.WHITE, (buttons & COIN) != 0, false);
            button(c, start, "START", Color.WHITE, (buttons & START) != 0, false);
        }

        private void paint(int color, int a, boolean stroke) {
            Paint p = stroke ? line : fill;
            p.setColor(Color.argb(Math.min(255, a), Color.red(color), Color.green(color), Color.blue(color)));
        }

        private void button(Canvas c, RectF r, String text, int color, boolean down, boolean round) {
            paint(color, down ? Math.min(255, alpha * 2) : alpha / 2, false);
            if (round) {
                c.drawOval(r, fill);
            } else {
                float k = r.height() / 2;
                c.drawRoundRect(r, k, k, fill);
            }
            paint(Color.WHITE, alpha, true);
            if (round) {
                c.drawOval(r, line);
            } else {
                float k = r.height() / 2;
                c.drawRoundRect(r, k, k, line);
            }
            label.setColor(Color.argb(Math.min(255, alpha * 2), 255, 255, 255));
            c.drawText(text, r.centerX(), r.centerY() - (label.ascent() + label.descent()) / 2, label);
        }
    }
}
