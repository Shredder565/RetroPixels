package io.github.shredder565.retropixels;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import io.github.shredder565.retropixels.engine.RunInput;

import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Set;

/**
 * Controllers (up to four, one per player, in the order they are first used)
 * and the keyboard. Controller n plays as player n, as in the native app;
 * the keyboard plays as player 1. Called on the UI thread; the game's thread
 * reads it with fill().
 */
final class Pads {
    static final int MAX = RunInput.MAX_PADS;

    private final int[] device = {-1, -1, -1, -1};
    private final int[] keys = new int[MAX]; // buttons held as keys
    private final int[] hat = new int[MAX]; // D-pad from the hat axes
    private final int[] triggers = new int[MAX]; // LT / RT from the trigger axes
    private final float[][] axes = new float[MAX][4];
    private final Set<String> keyboard = new LinkedHashSet<String>();
    private final Set<String> tapped = new HashSet<String>();
    private long lastPadUse;

    /** The player slot for a controller (taking a free one), or -1 when all four are taken. */
    private int slot(int deviceId) {
        for (int i = 0; i < MAX; i++) {
            if (device[i] == deviceId) {
                return i;
            }
        }
        for (int i = 0; i < MAX; i++) {
            if (device[i] == -1) {
                device[i] = deviceId;
                return i;
            }
        }
        return -1;
    }

    /** A controller was unplugged: the ones after it move up, as a browser's gamepad list does. */
    synchronized void removed(int deviceId) {
        for (int i = 0; i < MAX; i++) {
            if (device[i] == deviceId) {
                for (int j = i; j < MAX - 1; j++) {
                    device[j] = device[j + 1];
                    keys[j] = keys[j + 1];
                    hat[j] = hat[j + 1];
                    triggers[j] = triggers[j + 1];
                    axes[j] = axes[j + 1];
                }
                device[MAX - 1] = -1;
                keys[MAX - 1] = hat[MAX - 1] = triggers[MAX - 1] = 0;
                axes[MAX - 1] = new float[4];
                return;
            }
        }
    }

    /** True when a controller was used in the last moment (the touch controls hide). */
    synchronized boolean padInUse() {
        return lastPadUse != 0 && SystemClock.uptimeMillis() - lastPadUse < 15000;
    }

    synchronized void forgetPadUse() {
        lastPadUse = 0;
    }

    /** The first connected controller's InputDevice id, or -1. */
    synchronized int firstDevice() {
        return device[0];
    }

    /** A controller button; returns false for keys it doesn't use. */
    synchronized boolean padKey(KeyEvent e) {
        int b = KeyNames.padButton(e.getKeyCode());
        if (b < 0) {
            return false;
        }
        int s = slot(e.getDeviceId());
        if (s < 0) {
            return true;
        }
        lastPadUse = SystemClock.uptimeMillis();
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            keys[s] |= 1 << b;
        } else if (e.getAction() == KeyEvent.ACTION_UP) {
            keys[s] &= ~(1 << b);
        }
        return true;
    }

    /** A keyboard key; returns false for keys the controls don't know. */
    synchronized boolean keyboardKey(KeyEvent e) {
        String n = KeyNames.keyName(e);
        if (n == null) {
            return false;
        }
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            keyboard.add(n);
            tapped.add(n);
        } else if (e.getAction() == KeyEvent.ACTION_UP) {
            keyboard.remove(n);
        }
        return true;
    }

    private static float axis(MotionEvent e, InputDevice d, int a) {
        float v = e.getAxisValue(a);
        InputDevice.MotionRange r = d == null ? null : d.getMotionRange(a, e.getSource());
        float flat = r != null ? Math.max(r.getFlat(), 0.08f) : 0.12f;
        return Math.abs(v) < flat ? 0 : v;
    }

    /** Sticks, hats and triggers. */
    synchronized boolean motion(MotionEvent e) {
        if (!KeyNames.fromPad(e) || e.getAction() != MotionEvent.ACTION_MOVE) {
            return false;
        }
        int s = slot(e.getDeviceId());
        if (s < 0) {
            return true;
        }
        InputDevice d = e.getDevice();
        float[] a = axes[s];
        a[0] = axis(e, d, MotionEvent.AXIS_X);
        a[1] = axis(e, d, MotionEvent.AXIS_Y);
        // the right stick: Z / RZ on most controllers, RX / RY on some
        boolean rz = d == null || d.getMotionRange(MotionEvent.AXIS_RZ, e.getSource()) != null;
        a[2] = axis(e, d, rz ? MotionEvent.AXIS_Z : MotionEvent.AXIS_RX);
        a[3] = axis(e, d, rz ? MotionEvent.AXIS_RZ : MotionEvent.AXIS_RY);
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        int h = 0;
        if (hy < -0.5f) h |= 1 << 12;
        if (hy > 0.5f) h |= 1 << 13;
        if (hx < -0.5f) h |= 1 << 14;
        if (hx > 0.5f) h |= 1 << 15;
        hat[s] = h;
        float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
        float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
        triggers[s] = (lt > 0.5f ? 1 << 6 : 0) | (rt > 0.5f ? 1 << 7 : 0);
        if (h != 0 || triggers[s] != 0 || Math.abs(a[0]) > 0.3f || Math.abs(a[1]) > 0.3f || Math.abs(a[2]) > 0.3f || Math.abs(a[3]) > 0.3f) {
            lastPadUse = SystemClock.uptimeMillis();
        }
        return true;
    }

    /** Controller buttons of player p (for menus and shortcuts). */
    synchronized int buttons(int p) {
        return p < MAX ? keys[p] | hat[p] | triggers[p] : 0;
    }

    synchronized float axisOf(int p, int a) {
        return axes[p][a];
    }

    /** Lets go of everything (the window lost focus, a menu opened). */
    synchronized void clear() {
        for (int i = 0; i < MAX; i++) {
            keys[i] = hat[i] = triggers[i] = 0;
            axes[i] = new float[4];
        }
        keyboard.clear();
        tapped.clear();
    }

    /** Copies the controls into a run request. */
    synchronized void fill(RunInput in) {
        int n = 0;
        for (int i = 0; i < MAX; i++) {
            if (device[i] != -1) {
                n = i + 1;
            }
        }
        in.pads = n;
        for (int i = 0; i < n; i++) {
            in.padButtons[i] = keys[i] | hat[i] | triggers[i];
            System.arraycopy(axes[i], 0, in.padAxes[i], 0, 4);
        }
        in.keys.clear();
        in.keys.addAll(keyboard);
        in.keys.addAll(tapped);
        tapped.clear();
    }
}
