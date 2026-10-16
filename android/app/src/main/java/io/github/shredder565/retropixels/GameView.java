package io.github.shredder565.retropixels;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.view.View;

import io.github.shredder565.retropixels.engine.Fit;

import java.nio.ByteBuffer;

/**
 * The game's picture, scaled to the screen at the game's shape with black
 * borders (sharp or smooth pixels), as the native app's window draws it. In
 * portrait with the touch controls showing, it sits at the top.
 */
final class GameView extends View {
    private final Object lock = new Object();
    private Bitmap front, back;
    private final Paint paint = new Paint();
    private final Rect dst = new Rect();
    private volatile double aspect = 4.0 / 3;
    private volatile boolean top;
    private boolean fresh;

    GameView(Context c) {
        super(c);
        setBackgroundColor(Color.BLACK);
    }

    void setSmooth(boolean smooth) {
        paint.setFilterBitmap(smooth);
        invalidate();
    }

    void setAspect(double a) {
        aspect = a > 0 ? a : 4.0 / 3;
        postInvalidate();
    }

    void setTop(boolean t) {
        top = t;
        invalidate();
    }

    /** Where the picture is on the screen (for the Atari paddle). */
    Fit picture() {
        return Fit.fit(getWidth(), getHeight(), aspect, top);
    }

    /** A new picture (from the game's thread): RGBA bytes, w x h. */
    void setFrame(byte[] rgba, int w, int h) {
        if (w <= 0 || h <= 0) {
            return;
        }
        Bitmap b;
        synchronized (lock) {
            b = back;
            if (b == null || b.getWidth() != w || b.getHeight() != h) {
                b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            }
            back = null;
        }
        b.copyPixelsFromBuffer(ByteBuffer.wrap(rgba, 0, w * h * 4));
        synchronized (lock) {
            back = front;
            front = b;
            fresh = true;
        }
        postInvalidateOnAnimation();
    }

    /** The current picture (for save state thumbnails, the menu's background). */
    Bitmap current() {
        synchronized (lock) {
            return front;
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.drawColor(Color.BLACK);
        synchronized (lock) {
            if (front == null) {
                return;
            }
            Fit f = picture();
            dst.set(f.x, f.y, f.x + f.w, f.y + f.h);
            canvas.drawBitmap(front, null, dst, paint);
            fresh = false;
        }
    }
}
