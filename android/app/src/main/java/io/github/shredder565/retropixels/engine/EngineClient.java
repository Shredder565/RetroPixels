package io.github.shredder565.retropixels.engine;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.Closeable;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.util.Map;

/**
 * Talks to the emulator engine (librpengine.so, cmd/retropixels-android in
 * the RetroPixels sources) over its standard input and output. Every message
 * is a little-endian u32 length, a type byte and a body: 'J' JSON commands
 * and replies, 'R' run requests and 'F' frames (see proto.go there).
 *
 * Plain Java, no Android classes: the tests drive the engine built for the
 * computer with it. Calls are serialised, so the game thread and the menus
 * may use one client together.
 */
public final class EngineClient implements Closeable {
    public static final int PROTOCOL = 1;
    private static final Charset UTF8 = Charset.forName("UTF-8");

    private final InputStream in;
    private final OutputStream out;
    private final Closeable onClose;
    private final Map<String, Object> hello;
    private byte[] buf = new byte[1 << 16];
    private final byte[] hdr = new byte[5];

    /** Wraps a started engine's output (in) and input (out) and reads its greeting. */
    public EngineClient(InputStream engineOut, OutputStream engineIn, Closeable onClose) throws IOException {
        this.in = new BufferedInputStream(engineOut, 1 << 16);
        this.out = new BufferedOutputStream(engineIn, 1 << 12);
        this.onClose = onClose;
        Map<String, Object> h = readJSON();
        if (!"RetroPixels engine".equals(h.get("hello"))) {
            throw new IOException("that isn't the RetroPixels engine");
        }
        if (Json.integer(h, "protocol", 0) != PROTOCOL) {
            throw new IOException("the engine speaks protocol " + h.get("protocol") + ", the app " + PROTOCOL);
        }
        hello = h;
    }

    public static EngineClient start(Process p) throws IOException {
        final Process proc = p;
        return new EngineClient(p.getInputStream(), p.getOutputStream(), new Closeable() {
            public void close() {
                proc.destroy();
            }
        });
    }

    public String engineVersion() {
        return Json.str(hello, "version", "?");
    }

    /** Sends a JSON command and returns its reply (check "ok"). */
    public synchronized Map<String, Object> call(Map<String, Object> request) throws IOException {
        byte[] body = Json.write(request).getBytes(UTF8);
        send('J', body, body.length);
        return readJSON();
    }

    /** Like call, but a reply that isn't ok throws EngineError with its message. */
    public Map<String, Object> must(Map<String, Object> request) throws IOException {
        Map<String, Object> r = call(request);
        if (!Json.bool(r, "ok")) {
            throw new EngineError(Json.str(r, "error", "the engine said no"), r);
        }
        return r;
    }

    /** Runs frames: sends the input, fills f with what came back. */
    public synchronized Frame run(RunInput input, Frame f) throws IOException {
        int n = input.encode();
        send('R', input.bytes(), n);
        int len = readMessage();
        if (buf[0] != 'F') {
            throw new IOException("the engine answered a run with message type " + (char) buf[0]);
        }
        if (f == null) {
            f = new Frame();
        }
        f.decode(buf, 1, len - 1);
        return f;
    }

    private void send(char type, byte[] body, int n) throws IOException {
        int len = n + 1;
        hdr[0] = (byte) len;
        hdr[1] = (byte) (len >>> 8);
        hdr[2] = (byte) (len >>> 16);
        hdr[3] = (byte) (len >>> 24);
        hdr[4] = (byte) type;
        out.write(hdr, 0, 5);
        out.write(body, 0, n);
        out.flush();
    }

    private Map<String, Object> readJSON() throws IOException {
        int len = readMessage();
        if (buf[0] != 'J') {
            throw new IOException("expected a JSON reply from the engine, got type " + (char) buf[0]);
        }
        return Json.parseObject(new String(buf, 1, len - 1, UTF8));
    }

    /** Reads one message into buf (type at buf[0]) and returns its length. */
    private int readMessage() throws IOException {
        readFully(hdr, 4);
        int len = (hdr[0] & 0xff) | (hdr[1] & 0xff) << 8 | (hdr[2] & 0xff) << 16 | (hdr[3] & 0xff) << 24;
        if (len < 1 || len > (256 << 20)) {
            throw new IOException("bad message length from the engine: " + len);
        }
        if (buf.length < len) {
            buf = new byte[len + len / 4];
        }
        readFully(buf, len);
        return len;
    }

    private void readFully(byte[] b, int n) throws IOException {
        int off = 0;
        while (off < n) {
            int r = in.read(b, off, n - off);
            if (r < 0) {
                throw new EOFException("the engine stopped");
            }
            off += r;
        }
    }

    public void close() {
        try {
            synchronized (this) {
                out.close();
            }
        } catch (IOException ignored) {
            // already gone
        }
        try {
            if (onClose != null) {
                onClose.close();
            }
        } catch (IOException ignored) {
            // already gone
        }
    }

    /** A command the engine refused; reply holds the rest (such as "need"). */
    public static final class EngineError extends IOException {
        public final Map<String, Object> reply;

        public EngineError(String msg, Map<String, Object> reply) {
            super(msg);
            this.reply = reply;
        }
    }
}
