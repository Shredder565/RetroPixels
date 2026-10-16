package io.github.shredder565.retropixels;

import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.graphics.Color;
import android.os.Build;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/** Small helpers for screens built in code (the app has no layout files or libraries). */
final class Ui {
    static final int BG = 0xff101418;
    static final int FG = 0xffe8eaed;
    static final int DIM = 0xff9aa0a6;
    static final int ACCENT = 0xff8ab4f8;

    private Ui() {
    }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
    }

    static String version(Context c) {
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return pi.versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    static LinearLayout column(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        int p = dp(c, 16);
        l.setPadding(p, p, p, p);
        l.setBackgroundColor(BG);
        l.setFitsSystemWindows(true);
        return l;
    }

    static TextView text(Context c, String s, float sp, int color) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    static Button button(Context c, String label, View.OnClickListener l) {
        Button b = new Button(c);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }

    static TextView heading(Context c, String s) {
        TextView t = text(c, s, 22, FG);
        t.setPadding(0, 0, 0, dp(c, 8));
        t.setGravity(Gravity.START);
        return t;
    }

    static int alpha(int color, int percent) {
        return Color.argb(percent * 255 / 100, Color.red(color), Color.green(color), Color.blue(color));
    }

    /**
     * Calls back when the player presses Back (or swipes back). Apps that
     * target Android 13 and later get Back through OnBackInvokedCallback; the
     * app is built against an older Android, so it makes one at run time.
     * Older Androids call onBackPressed, which the activity passes here too.
     */
    static void onBack(Activity a, final Runnable r) {
        if (Build.VERSION.SDK_INT < 33) {
            return;
        }
        try {
            Class<?> cb = Class.forName("android.window.OnBackInvokedCallback");
            Object callback = Proxy.newProxyInstance(cb.getClassLoader(), new Class<?>[] {cb}, new InvocationHandler() {
                public Object invoke(Object proxy, Method m, Object[] args) {
                    String n = m.getName();
                    if (n.equals("onBackInvoked")) {
                        r.run();
                        return null;
                    }
                    if (n.equals("hashCode")) {
                        return System.identityHashCode(proxy);
                    }
                    if (n.equals("equals")) {
                        return proxy == args[0];
                    }
                    if (n.equals("toString")) {
                        return "RetroPixels back";
                    }
                    return null;
                }
            });
            Object dispatcher = Activity.class.getMethod("getOnBackInvokedDispatcher").invoke(a);
            Class<?> d = Class.forName("android.window.OnBackInvokedDispatcher");
            d.getMethod("registerOnBackInvokedCallback", int.class, cb).invoke(dispatcher, 0, callback);
        } catch (Exception ignored) {
            // the system's own Back behaviour stays
        }
    }
}
