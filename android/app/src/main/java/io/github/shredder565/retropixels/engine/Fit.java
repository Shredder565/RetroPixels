package io.github.shredder565.retropixels.engine;

/** Where the picture goes on the screen (fitRect in RetroPixels Native's layout.go, draw() in host.js). */
public final class Fit {
    public final int x, y, w, h;

    public Fit(int x, int y, int w, int h) {
        this.x = x;
        this.y = y;
        this.w = w;
        this.h = h;
    }

    /**
     * The biggest picture of that aspect (width / height) that fits cw x ch,
     * centred across, and up and down too unless top puts it at the top (to leave
     * room for touch controls below a portrait picture).
     */
    public static Fit fit(int cw, int ch, double aspect, boolean top) {
        if (cw <= 0 || ch <= 0 || !(aspect > 0)) {
            return new Fit(0, 0, 0, 0);
        }
        int dw = cw, dh = (int) Math.round(cw / aspect);
        if (dh > ch) {
            dh = ch;
            dw = (int) Math.round(ch * aspect);
        }
        return new Fit((cw - dw) / 2, top ? 0 : (ch - dh) / 2, dw, dh);
    }
}
