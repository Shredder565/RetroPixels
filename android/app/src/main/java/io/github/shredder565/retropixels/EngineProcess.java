package io.github.shredder565.retropixels;

import android.content.Context;

import io.github.shredder565.retropixels.engine.EngineClient;
import io.github.shredder565.retropixels.engine.Json;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.TimeZone;

/**
 * Starts the emulator engine: librpengine.so, a program (not a library) the
 * build puts with the app's native libraries, which Android unpacks to the
 * app's native library folder, where an app may run programs. It keeps the
 * saves in the app's files folder, as RetroPixels Native keeps them in
 * %APPDATA%\RetroPixels Native.
 */
final class EngineProcess {
    final Process process;
    final EngineClient client;
    private final StringBuilder errors = new StringBuilder();

    private EngineProcess(Process p) throws IOException {
        process = p;
        final InputStream err = p.getErrorStream();
        Thread t = new Thread(new Runnable() {
            public void run() {
                byte[] b = new byte[1024];
                try {
                    int n;
                    while ((n = err.read(b)) > 0) {
                        synchronized (errors) {
                            errors.append(new String(b, 0, n, "UTF-8"));
                            if (errors.length() > 4000) {
                                errors.delete(0, errors.length() - 4000);
                            }
                        }
                    }
                } catch (IOException ignored) {
                    // the engine has gone
                }
            }
        }, "engine-errors");
        t.setDaemon(true);
        t.start();
        EngineClient c;
        try {
            c = EngineClient.start(p);
        } catch (IOException e) {
            p.destroy();
            throw new IOException(e.getMessage() + tailNote());
        }
        client = c;
    }

    static File engineFile(Context c) {
        return new File(c.getApplicationInfo().nativeLibraryDir, "librpengine.so");
    }

    static EngineProcess start(Context c) throws IOException {
        File exe = engineFile(c);
        if (!exe.exists()) {
            throw new IOException("The emulator engine is missing from this copy of the app (" + exe.getPath()
                    + "). This phone's processor may not be one the app was built for.");
        }
        TimeZone tz = TimeZone.getDefault();
        ProcessBuilder pb = new ProcessBuilder(exe.getPath(), "-dir", c.getFilesDir().getPath(),
                "-tz", tz.getID(), "-tzoffset", String.valueOf(tz.getOffset(System.currentTimeMillis()) / 1000));
        pb.environment().put("TMPDIR", c.getCacheDir().getPath());
        pb.directory(c.getFilesDir());
        return new EngineProcess(pb.start());
    }

    /** What the engine last wrote to its error output. */
    String tail() {
        synchronized (errors) {
            return errors.toString().trim();
        }
    }

    String tailNote() {
        try {
            Thread.sleep(100); // let the last words arrive
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        String t = tail();
        return t.isEmpty() ? "" : "\n\n" + t;
    }

    /** Ends the engine: it writes the battery save and quits; it is stopped if it doesn't. */
    void stop() {
        Thread t = new Thread(new Runnable() {
            public void run() {
                try {
                    client.call(Json.obj("cmd", "quit"));
                } catch (IOException ignored) {
                    // already gone
                }
            }
        }, "engine-quit");
        t.setDaemon(true);
        t.start();
        try {
            t.join(3000);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        client.close();
        process.destroy();
    }
}
