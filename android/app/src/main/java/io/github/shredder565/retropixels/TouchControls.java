package io.github.shredder565.retropixels;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import io.github.shredder565.retropixels.engine.Fit;
import io.github.shredder565.retropixels.engine.RunInput;
import io.github.shredder565.retropixels.engine.TouchLayout;

import java.util.List;

/**
 * The on-screen controls (TouchLayout), drawn see-through over the game, with
 * any number of fingers. A finger that lands on the D-pad or the stick keeps
 * working it wherever it slides; one on the buttons works whichever button it
 * is over. On the Atari 2600, a finger on the picture turns the paddle.
 */
final class TouchControls extends View {
    interface Listener {
        void onMenu();

        /** The picture's place, for the paddle (null: no paddle). */
        Fit paddleArea();
    }

    private final Listener listener;
    private TouchLayout layout;
    private String system = "";
    private List<String> buttons;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final SparseArray<TouchLayout.Control> held = new SparseArray<TouchLayout.Control>();
    private int opacity = 45;
    private float scale = 1;
    private boolean shown = true, vibrate = true;
    private int picBottom;

    // what the fingers hold (read by the game's thread)
    private volatile int bits;
    private volatile boolean stick, paddle, fast;
    private volatile float ax, ay, paddleX;
    private float knobX, knobY;

    TouchControls(Context c, Listener l) {
        super(c);
        listener = l;
        line.setStyle(Paint.Style.STROKE);
        label.setTextAlign(Paint.Align.CENTER);
        label.setTypeface(Typeface.DEFAULT_BOLD);
    }

    void setGame(String system, List<String> buttons) {
        this.system = system;
        this.buttons = buttons;
        relayout();
    }

    void setLook(int opacity, int sizePercent, boolean vibrate) {
        this.opacity = opacity;
        this.scale = sizePercent / 100f;
        this.vibrate = vibrate;
        relayout();
    }

    /** In portrait, the controls go below the picture. */
    void setPictureBottom(int y) {
        if (y != picBottom) {
            picBottom = y;
            relayout();
        }
    }

    /** Shown or hidden (hidden ones come back when the screen is touched). */
    void setShown(boolean s) {
        if (s != shown) {
            shown = s;
            if (!s) {
                release();
            }
            invalidate();
        }
    }

    boolean shown() {
        return shown;
    }

    private void relayout() {
        if (buttons == null || getWidth() == 0) {
            layout = null;
            invalidate();
            return;
        }
        float dp = getResources().getDisplayMetrics().density;
        layout = TouchLayout.make(system, buttons, getWidth(), getHeight(), dp, scale, picBottom);
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        relayout();
    }

    /** Copies what the fingers hold into a run request. */
    void fill(RunInput in) {
        in.touchButtons = bits;
        boolean s = stick, p = paddle;
        in.touchX = s || p;
        in.touchY = s;
        in.axisX = s ? ax : paddleX;
        in.axisY = s ? ay : 0;
    }

    boolean fastHeld() {
        return fast;
    }

    private void release() {
        held.clear();
        bits = 0;
        stick = paddle = fast = false;
        ax = ay = 0;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        if (!shown) {
            if (action == MotionEvent.ACTION_DOWN) {
                shown = true; // a touch brings them back
                invalidate();
            } else {
                return true;
            }
        }
        if (layout == null) {
            return true;
        }
        int gone = -1;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            gone = e.getPointerId(e.getActionIndex());
            held.remove(gone);
        } else if (action == MotionEvent.ACTION_CANCEL) {
            release();
            invalidate();
            return true;
        }
        boolean down = action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN;
        int downId = down ? e.getPointerId(e.getActionIndex()) : -1;
        int b = 0;
        boolean st = false, pd = false, ff = false;
        float sx = 0, sy = 0, px = 0;
        Fit pic = system.equals("atari2600") ? listener.paddleArea() : null;
        for (int i = 0; i < e.getPointerCount(); i++) {
            int id = e.getPointerId(i);
            if (id == gone) {
                continue;
            }
            float x = e.getX(i), y = e.getY(i);
            TouchLayout.Control c = held.get(id);
            if (c == null || c.kind == TouchLayout.BUTTON || c.kind == TouchLayout.PILL) {
                TouchLayout.Control at = layout.at(x, y);
                if (c == null && id != downId) {
                    at = at != null && (at.kind == TouchLayout.BUTTON || at.kind == TouchLayout.PILL) ? at : null;
                }
                c = at;
                if (c != null && (id == downId || c.kind == TouchLayout.BUTTON || c.kind == TouchLayout.PILL)) {
                    held.put(id, c);
                } else if (c == null) {
                    held.remove(id);
                }
            }
            if (c == null) {
                if (pic != null && pic.w > 0) { // the paddle: across the picture
                    pd = true;
                    px = Math.max(-1, Math.min(1, (x - pic.x) / pic.w * 2 - 1));
                }
                continue;
            }
            switch (c.kind) {
                case TouchLayout.BUTTON:
                case TouchLayout.PILL:
                    if (c.bit >= 0) {
                        b |= 1 << c.bit;
                    }
                    break;
                case TouchLayout.DPAD:
                    b |= TouchLayout.dpadBits(c, x, y);
                    break;
                case TouchLayout.STICK: {
                    float[] a = TouchLayout.stickAxes(c, x, y);
                    st = true;
                    sx = a[0];
                    sy = a[1];
                    knobX = c.cx + a[0] * c.r;
                    knobY = c.cy + a[1] * c.r;
                    break;
                }
                case TouchLayout.FAST:
                    ff = true;
                    break;
                case TouchLayout.MENU:
                    if (id == downId) {
                        held.remove(id);
                        release();
                        listener.onMenu();
                        invalidate();
                        return true;
                    }
                    break;
                default:
                    break;
            }
        }
        if (vibrate && (b & ~bits) != 0) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY, HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING);
        }
        bits = b;
        ax = sx;
        ay = sy;
        stick = st;
        paddle = pd;
        paddleX = px;
        fast = ff;
        invalidate();
        return true;
    }

    @Override
    protected void onDraw(Canvas c) {
        if (layout == null || !shown) {
            return;
        }
        float dp = getResources().getDisplayMetrics().density;
        line.setStrokeWidth(2 * dp);
        int on = bits;
        for (TouchLayout.Control k : layout.controls) {
            boolean pressed = (k.bit >= 0 && (on & (1 << k.bit)) != 0) || (k.kind == TouchLayout.FAST && fast)
                    || (k.kind == TouchLayout.STICK && stick);
            int a = pressed ? Math.min(100, opacity + 30) : opacity;
            fill.setColor(Ui.alpha(pressed ? 0xffffffff : 0xff808890, a * 2 / 3));
            line.setColor(Ui.alpha(0xffffffff, a));
            label.setColor(Ui.alpha(0xffffffff, Math.min(100, a + 20)));
            switch (k.kind) {
                case TouchLayout.PILL:
                    rect.set(k.cx - k.hw, k.cy - k.r, k.cx + k.hw, k.cy + k.r);
                    c.drawRoundRect(rect, k.r, k.r, fill);
                    c.drawRoundRect(rect, k.r, k.r, line);
                    text(c, k.label, k.cx, k.cy, k.r * 0.95f, k.hw * 1.8f);
                    break;
                case TouchLayout.DPAD:
                    drawDpad(c, k, on, a);
                    break;
                case TouchLayout.STICK: {
                    c.drawCircle(k.cx, k.cy, k.r, fill);
                    c.drawCircle(k.cx, k.cy, k.r, line);
                    float kx = stick ? knobX : k.cx, ky = stick ? knobY : k.cy;
                    fill.setColor(Ui.alpha(0xffffffff, a));
                    c.drawCircle(kx, ky, k.r * 0.42f, fill);
                    break;
                }
                default:
                    c.drawCircle(k.cx, k.cy, k.r, fill);
                    c.drawCircle(k.cx, k.cy, k.r, line);
                    text(c, k.label, k.cx, k.cy, k.r * 0.8f, k.r * 1.8f);
            }
        }
    }

    private void text(Canvas c, String s, float x, float y, float size, float maxWidth) {
        label.setTextSize(size);
        float w = label.measureText(s);
        if (w > maxWidth && w > 0) {
            label.setTextSize(size * maxWidth / w);
        }
        Paint.FontMetrics fm = label.getFontMetrics();
        c.drawText(s, x, y - (fm.ascent + fm.descent) / 2, label);
    }

    private void drawDpad(Canvas c, TouchLayout.Control d, int on, int a) {
        float arm = d.r * 0.36f;
        // the cross
        rect.set(d.cx - arm, d.cy - d.r, d.cx + arm, d.cy + d.r);
        c.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, fill);
        c.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, line);
        rect.set(d.cx - d.r, d.cy - arm, d.cx + d.r, d.cy + arm);
        c.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, fill);
        c.drawRoundRect(rect, arm * 0.4f, arm * 0.4f, line);
        Paint hl = fill;
        int[] bitsOf = {d.up, d.down, d.left, d.right};
        float[][] at = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        for (int i = 0; i < 4; i++) {
            if (bitsOf[i] < 0) {
                continue;
            }
            boolean pressed = (on & (1 << bitsOf[i])) != 0;
            hl.setColor(Ui.alpha(0xffffffff, pressed ? Math.min(100, a + 40) : a / 2));
            c.drawCircle(d.cx + at[i][0] * d.r * 0.68f, d.cy + at[i][1] * d.r * 0.68f, arm * 0.45f, hl);
        }
    }
}
