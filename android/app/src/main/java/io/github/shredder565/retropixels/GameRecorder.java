package io.github.shredder565.retropixels;

import android.Manifest;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.util.Log;
import android.view.Surface;

import io.github.shredder565.retropixels.engine.Recording;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;

/**
 * Records a game as it plays (Settings > Record games automatically): the
 * game's picture exactly as the engine draws it (not the touch controls) and
 * its sound, encoded as H.264 and AAC into an .mp4 by the phone's own
 * encoders. No screen capture, so nothing to allow.
 *
 * The game's thread hands over each picture and its sound (frame, audio) and
 * goes on at once; the recorder's own thread scales the picture up sharp
 * with OpenGL into the encoder, encodes the sound and writes the file. The
 * video goes to Movies/RetroPixels: through MediaStore on Android 10 and
 * newer (it shows up only once it is complete), in the public Movies folder
 * on Android 7 to 9 (the app's own Movies folder if storage access wasn't
 * allowed). Anything going wrong stops the recording, never the game.
 */
final class GameRecorder {
    private static final String TAG = "RetroPixels";
    static final String FOLDER = "RetroPixels";

    /** What becomes of a recording (called on the recorder's thread). */
    interface Listener {
        /** The recording couldn't start or stopped with an error. */
        void failed(String why);

        /** Saved: where, said for the player ("Movies/RetroPixels/... .mp4"). */
        void saved(String where, boolean announce);
    }

    private final Context app;
    private final String game;
    private final double aspect;
    private final int rate;
    private final Listener listener;
    private final Recording.Clock clock;
    private final double fps;
    private final LinkedBlockingQueue<Object> queue = new LinkedBlockingQueue<Object>();
    private final ArrayBlockingQueue<Pic> free = new ArrayBlockingQueue<Pic>(4);
    private volatile boolean stopping, dead;
    private volatile boolean announce;
    private volatile Thread thread;
    private int queuedAudio; // sample pairs waiting (game thread)
    private final Object countLock = new Object();

    private static final class Pic {
        byte[] rgba = new byte[0];
        int w, h;
        long us;
    }

    private static final class Pcm {
        final short[] s;
        final int pairs, silence;
        final long start; // the first pair's place on the recording's clock (pairs)

        Pcm(short[] s, int pairs, int silence, long start) {
            this.s = s;
            this.pairs = pairs;
            this.silence = silence;
            this.start = start;
        }
    }

    private static final Object STOP = new Object();

    private GameRecorder(Context c, String game, double aspect, double fps, int rate, Listener l) {
        this.app = c.getApplicationContext();
        this.game = game;
        this.aspect = aspect;
        this.rate = rate;
        this.listener = l;
        this.clock = new Recording.Clock(fps, rate);
        this.fps = fps > 0 ? fps : 60;
        for (int i = 0; i < 4; i++) {
            free.add(new Pic());
        }
    }

    /** Starts recording; the first picture sets the video's size. Never throws: problems come to the listener. */
    static GameRecorder start(Context c, String game, double aspect, double fps, int rate, Listener l) {
        final GameRecorder r = new GameRecorder(c, game, aspect, fps, rate, l);
        Thread t = new Thread(new Runnable() {
            public void run() {
                r.work();
            }
        }, "recorder");
        t.setPriority(Thread.NORM_PRIORITY - 1);
        r.thread = t;
        t.start();
        return r;
    }

    /**
     * Waits (at most ms) for the recording to be finished and saved after
     * stop(): for Star Wars Trilogy Arcade, whose process ends with the game.
     */
    boolean awaitFinished(long ms) {
        try {
            thread.join(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return !thread.isAlive();
    }

    boolean dead() {
        return dead;
    }

    // ---------------- the game's thread ----------------

    /**
     * A run's result: its picture (or null when it brought none) and sound
     * (s16 stereo values). frames: how many frames it ran; fast: fast-forward.
     */
    void frame(byte[] rgba, int w, int h, int frames, boolean fast, short[] samples, int sampleCount) {
        if (stopping || dead) {
            return;
        }
        if (rgba != null && w > 0 && h > 0) {
            long us = clock.video(frames, fast);
            Pic p = free.poll(); // none free: the encoder is behind, so this picture is left out
            if (p != null) {
                int n = w * h * 4;
                if (p.rgba.length < n) {
                    p.rgba = new byte[n];
                }
                System.arraycopy(rgba, 0, p.rgba, 0, n);
                p.w = w;
                p.h = h;
                p.us = us;
                queue.add(p);
            }
        } else {
            clock.skip(frames, fast);
        }
        long start = clock.audioPairs();
        int pairs = clock.audio(Math.max(0, sampleCount / 2));
        int silence = clock.silencePairs();
        if (pairs + silence > 0) {
            synchronized (countLock) {
                if (queuedAudio > rate * 4) { // four seconds behind: the phone can't keep up
                    fail("the sound encoder is too slow");
                    return;
                }
                queuedAudio += pairs + silence;
            }
            short[] copy = new short[pairs * 2];
            System.arraycopy(samples, 0, copy, 0, pairs * 2);
            queue.add(new Pcm(copy, pairs, silence, start));
        }
    }

    /** Ends the recording and saves it (in the background). announce: say where it went. */
    void stop(boolean announce) {
        if (stopping) {
            return;
        }
        this.announce = announce;
        stopping = true;
        queue.add(STOP);
    }

    private void fail(String why) {
        if (!dead) {
            dead = true;
            queue.add(STOP);
            Log.w(TAG, "recording failed: " + why);
        }
    }

    // ---------------- the recorder's thread ----------------

    private MediaCodec video, audio;
    private MediaMuxer muxer;
    private int videoTrack = -1, audioTrack = -1;
    private boolean muxing;
    private MediaFormat videoFormat, audioFormat;
    private final List<Pending> pending = new ArrayList<Pending>();
    private boolean videoDone, audioDone;
    private long lastVideoUs = -1;
    private long audioEnd; // where the sound given to the encoder ends (pairs)
    private int outW, outH;
    private final MediaCodec.BufferInfo bi = new MediaCodec.BufferInfo();

    // where it goes
    private Uri uri; // Android 10+
    private ParcelFileDescriptor pfd;
    private File file; // Android 7-9
    private String where;

    // OpenGL
    private EGLDisplay eglDisplay = EGL14.EGL_NO_DISPLAY;
    private EGLContext eglContext = EGL14.EGL_NO_CONTEXT;
    private EGLSurface eglSurface = EGL14.EGL_NO_SURFACE;
    private Surface input;
    private int program, tex, texW, texH, aPos, aTex;
    private FloatBuffer quad;

    private static final class Pending {
        final boolean isVideo;
        final ByteBuffer data;
        final long us;
        final int flags;

        Pending(boolean isVideo, ByteBuffer data, long us, int flags) {
            this.isVideo = isVideo;
            this.data = data;
            this.us = us;
            this.flags = flags;
        }
    }

    private void work() {
        String error = null;
        boolean started = false;
        try {
            while (true) {
                Object o = queue.take();
                if (o == STOP) {
                    break;
                }
                if (o instanceof Pic) {
                    Pic p = (Pic) o;
                    try {
                        if (!started) {
                            open(p.w, p.h);
                            started = true;
                        }
                        drawFrame(p);
                    } finally {
                        free.offer(p);
                    }
                } else if (o instanceof Pcm) {
                    Pcm a = (Pcm) o;
                    synchronized (countLock) {
                        queuedAudio -= a.pairs + a.silence;
                    }
                    if (started) {
                        writeAudio(a);
                    }
                }
                drain(false);
            }
            if (dead) {
                error = "the encoder was too slow";
            } else if (started) {
                finish();
            }
        } catch (Throwable e) { // an encoder or OpenGL problem: the recording ends, the game plays on
            error = e.getMessage() != null ? e.getMessage() : e.toString();
            Log.w(TAG, "recording failed", e);
        }
        dead = true;
        stopping = true;
        queue.clear();
        boolean keep = error == null && started && muxing && lastVideoUs >= Recording.MIN_SECONDS * 1e6;
        releaseAll(keep);
        if (error != null) {
            listener.failed(error);
        } else if (keep) {
            listener.saved(where, announce);
        }
    }

    private void open(int w, int h) throws IOException {
        // the video's size: ~720 lines at the game's shape, fitted to the encoder
        int[] want = Recording.videoSize(h, aspect);
        video = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        int[] size = want;
        try {
            MediaCodecInfo.VideoCapabilities vc = video.getCodecInfo()
                    .getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).getVideoCapabilities();
            int maxW = vc.getSupportedWidths().getUpper(), maxH = vc.getSupportedHeights().getUpper();
            size = Recording.fit(want[0], want[1], vc.getWidthAlignment(), vc.getHeightAlignment(),
                    vc.getSupportedWidths().getLower(), vc.getSupportedHeights().getLower(),
                    Math.min(maxW, 1920), Math.min(maxH, 1088));
            for (int i = 0; i < 6 && !vc.isSizeSupported(size[0], size[1]); i++) { // smaller until it takes it
                size = Recording.fit(size[0] * 3 / 4, size[1] * 3 / 4, vc.getWidthAlignment(), vc.getHeightAlignment(),
                        vc.getSupportedWidths().getLower(), vc.getSupportedHeights().getLower(), maxW, maxH);
            }
        } catch (RuntimeException e) {
            size = Recording.fit(want[0], want[1], 16, 16, 0, 0, 1280, 720);
        }
        outW = size[0];
        outH = size[1];
        MediaFormat vf = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, outW, outH);
        vf.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        vf.setInteger(MediaFormat.KEY_BIT_RATE, Recording.bitRate(outW, outH, fps));
        vf.setInteger(MediaFormat.KEY_FRAME_RATE, (int) Math.max(1, Math.round(fps))); // a hint: each frame has its time
        vf.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        video.configure(vf, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        input = video.createInputSurface();
        video.start();
        setUpGL();

        MediaFormat af = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, rate, 2);
        af.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC);
        af.setInteger(MediaFormat.KEY_BIT_RATE, 192000);
        af.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384);
        audio = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC);
        audio.configure(af, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        audio.start();

        openOutput();
        Log.i(TAG, "recording " + w + "x" + h + " as " + outW + "x" + outH + " to " + where);
    }

    /** The file: MediaStore (Android 10+) or a file in Movies/RetroPixels. */
    private void openOutput() throws IOException {
        String base = Recording.baseName(game, new Date());
        if (Build.VERSION.SDK_INT >= 29) {
            ContentResolver cr = app.getContentResolver();
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.DISPLAY_NAME, base + ".mp4");
            v.put(MediaStore.MediaColumns.MIME_TYPE, "video/mp4");
            v.put("relative_path", Environment.DIRECTORY_MOVIES + "/" + FOLDER); // MediaColumns.RELATIVE_PATH
            v.put("is_pending", 1); // MediaColumns.IS_PENDING: hidden until it is complete
            uri = cr.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, v);
            if (uri == null) {
                throw new IOException("the Movies folder can't be written");
            }
            pfd = cr.openFileDescriptor(uri, "rw");
            if (pfd == null) {
                throw new IOException("the Movies folder can't be written");
            }
            try { // MediaMuxer(FileDescriptor, int) is Android 8's; the app is built against older classes
                muxer = MediaMuxer.class.getConstructor(FileDescriptor.class, int.class)
                        .newInstance(pfd.getFileDescriptor(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            } catch (ReflectiveOperationException e) {
                Throwable c = e.getCause() != null ? e.getCause() : e;
                throw new IOException("the video file couldn't be made: " + c.getMessage());
            }
            where = Environment.DIRECTORY_MOVIES + "/" + FOLDER;
        } else {
            File dir = legacyFolder(app);
            if (!dir.isDirectory() && !dir.mkdirs()) {
                throw new IOException("couldn't make the folder " + dir);
            }
            final File d = dir;
            file = new File(dir, Recording.uniqueFile(base, new Recording.Taken() {
                public boolean taken(String name) {
                    return new File(d, name).exists();
                }
            }));
            muxer = new MediaMuxer(file.getPath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            where = file.getParent();
        }
    }

    /** Android 7-9: the public Movies/RetroPixels if storage may be written, else the app's own Movies folder. */
    @SuppressWarnings("deprecation")
    static File legacyFolder(Context c) {
        if (c.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED) {
            return new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES), FOLDER);
        }
        File own = c.getExternalFilesDir(Environment.DIRECTORY_MOVIES);
        return new File(own != null ? own : c.getFilesDir(), FOLDER);
    }

    // ---- the picture ----

    private static final String VERTEX = "attribute vec2 pos;\nattribute vec2 uv;\nvarying vec2 v;\n"
            + "void main() { v = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
    private static final String FRAGMENT = "precision mediump float;\nvarying vec2 v;\nuniform sampler2D t;\n"
            + "void main() { gl_FragColor = vec4(texture2D(t, v).rgb, 1.0); }\n";

    private void setUpGL() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] ver = new int[2];
        if (eglDisplay == EGL14.EGL_NO_DISPLAY || !EGL14.eglInitialize(eglDisplay, ver, 0, ver, 1)) {
            throw new IllegalStateException("OpenGL isn't available");
        }
        int[] attrs = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, 0x3142 /* EGL_RECORDABLE_ANDROID */, 1, EGL14.EGL_NONE};
        EGLConfig[] cfg = new EGLConfig[1];
        int[] n = new int[1];
        if (!EGL14.eglChooseConfig(eglDisplay, attrs, 0, cfg, 0, 1, n, 0) || n[0] < 1) {
            throw new IllegalStateException("no OpenGL setup for recording");
        }
        eglContext = EGL14.eglCreateContext(eglDisplay, cfg[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            throw new IllegalStateException("OpenGL context: error " + EGL14.eglGetError());
        }
        eglSurface = EGL14.eglCreateWindowSurface(eglDisplay, cfg[0], input, new int[] {EGL14.EGL_NONE}, 0);
        if (eglSurface == EGL14.EGL_NO_SURFACE) {
            throw new IllegalStateException("OpenGL surface: error " + EGL14.eglGetError());
        }
        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            throw new IllegalStateException("OpenGL: error " + EGL14.eglGetError());
        }
        program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, shader(GLES20.GL_VERTEX_SHADER, VERTEX));
        GLES20.glAttachShader(program, shader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT));
        GLES20.glLinkProgram(program);
        int[] ok = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, ok, 0);
        if (ok[0] == 0) {
            throw new IllegalStateException("OpenGL program: " + GLES20.glGetProgramInfoLog(program));
        }
        aPos = GLES20.glGetAttribLocation(program, "pos");
        aTex = GLES20.glGetAttribLocation(program, "uv");
        // the whole video; the picture's top row (the first in memory) at the top
        float[] q = {-1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0};
        quad = ByteBuffer.allocateDirect(q.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        quad.put(q).position(0);
        int[] t = new int[1];
        GLES20.glGenTextures(1, t, 0);
        tex = t[0];
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex);
        // sharp pixels, as Windows records them
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1);
    }

    private static int shader(int type, String src) {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
        if (ok[0] == 0) {
            throw new IllegalStateException("OpenGL shader: " + GLES20.glGetShaderInfoLog(s));
        }
        return s;
    }

    private ByteBuffer upload = ByteBuffer.allocateDirect(0);

    /** Draws a picture into the encoder at its time. A new picture size (the game changed mode) is scaled the same way. */
    private void drawFrame(Pic p) {
        if (p.us <= lastVideoUs) {
            return;
        }
        int n = p.w * p.h * 4;
        if (upload.capacity() < n) {
            upload = ByteBuffer.allocateDirect(n);
        }
        upload.clear();
        upload.put(p.rgba, 0, n).position(0);
        GLES20.glViewport(0, 0, outW, outH);
        GLES20.glUseProgram(program);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex);
        if (p.w != texW || p.h != texH) {
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, p.w, p.h, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, upload);
            texW = p.w;
            texH = p.h;
        } else {
            GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, p.w, p.h, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, upload);
        }
        quad.position(0);
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, quad);
        GLES20.glEnableVertexAttribArray(aPos);
        quad.position(2);
        GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 16, quad);
        GLES20.glEnableVertexAttribArray(aTex);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        int err = GLES20.glGetError();
        if (err != GLES20.GL_NO_ERROR) {
            throw new IllegalStateException("OpenGL drawing: error " + err);
        }
        EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, p.us * 1000);
        if (!EGL14.eglSwapBuffers(eglDisplay, eglSurface)) {
            throw new IllegalStateException("OpenGL: error " + EGL14.eglGetError());
        }
        lastVideoUs = p.us;
    }

    // ---- the sound ----

    private void writeAudio(Pcm a) {
        int total = a.silence + a.pairs;
        int done = 0; // pairs written of total (silence first)
        long giveUp = System.nanoTime() + 3000000000L;
        while (done < total) {
            int i = audio.dequeueInputBuffer(10000);
            if (i < 0) {
                drain(false);
                if (dead) {
                    return;
                }
                if (System.nanoTime() > giveUp) {
                    throw new IllegalStateException("the sound encoder stopped taking sound");
                }
                continue;
            }
            ByteBuffer b = audio.getInputBuffer(i);
            b.clear();
            b.order(ByteOrder.LITTLE_ENDIAN);
            int fit = Math.max(1, b.remaining() / 4);
            int k = Math.min(fit, total - done);
            long us = (a.start + done) * 1000000L / rate;
            for (int j = 0; j < k; j++) {
                int at = done + j;
                if (at < a.silence) {
                    b.putInt(0);
                } else {
                    int s = (at - a.silence) * 2;
                    b.put((byte) a.s[s]).put((byte) (a.s[s] >> 8)).put((byte) a.s[s + 1]).put((byte) (a.s[s + 1] >> 8));
                }
            }
            audio.queueInputBuffer(i, 0, k * 4, us, 0);
            done += k;
        }
        audioEnd = a.start + total;
    }

    // ---- the encoders' output ----

    /** Takes what the encoders made and writes it; until: wait for both to end. */
    private void drain(boolean until) {
        if (video == null) {
            return;
        }
        long deadline = System.nanoTime() + 3000000000L;
        while (true) {
            boolean any = false;
            if (!videoDone) {
                any |= drainOne(video, true, until ? 10000 : 0);
            }
            if (!audioDone) {
                any |= drainOne(audio, false, until ? 10000 : 0);
            }
            if (!until && !any) {
                return;
            }
            if (until && ((videoDone && audioDone) || System.nanoTime() > deadline)) {
                return;
            }
        }
    }

    /** One step of one encoder's output; true if something came. */
    private boolean drainOne(MediaCodec c, boolean isVideo, long timeoutUs) {
        int i = c.dequeueOutputBuffer(bi, timeoutUs);
        if (i == MediaCodec.INFO_TRY_AGAIN_LATER) {
            return false;
        }
        if (i == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
            if (isVideo) {
                videoFormat = c.getOutputFormat();
            } else {
                audioFormat = c.getOutputFormat();
            }
            startMuxer(false);
            return true;
        }
        if (i < 0) {
            return true; // output buffers changed (older phones): they are fetched each time
        }
        ByteBuffer b = c.getOutputBuffer(i);
        boolean eos = (bi.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
        if ((bi.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0 && bi.size > 0) {
            b.position(bi.offset);
            b.limit(bi.offset + bi.size);
            if (muxing) {
                write(isVideo, b, bi.presentationTimeUs, bi.flags);
            } else {
                if (pending.size() > 2000) {
                    throw new IllegalStateException("the encoders didn't start together");
                }
                ByteBuffer copy = ByteBuffer.allocate(bi.size);
                copy.put(b);
                copy.flip();
                pending.add(new Pending(isVideo, copy, bi.presentationTimeUs, bi.flags & ~MediaCodec.BUFFER_FLAG_END_OF_STREAM));
            }
        }
        c.releaseOutputBuffer(i, false);
        if (eos) {
            if (isVideo) {
                videoDone = true;
            } else {
                audioDone = true;
            }
        }
        return true;
    }

    private final MediaCodec.BufferInfo wbi = new MediaCodec.BufferInfo();

    private void write(boolean isVideo, ByteBuffer b, long us, int flags) {
        int track = isVideo ? videoTrack : audioTrack;
        if (track < 0) {
            return;
        }
        // the sound's times must go up (the picture's may be reordered by the encoder: the muxer takes that)
        if (!isVideo) {
            if (us <= lastWrittenAudio) {
                us = lastWrittenAudio + 1;
            }
            lastWrittenAudio = us;
        }
        wbi.set(b.position(), b.remaining(), us, flags & ~MediaCodec.BUFFER_FLAG_END_OF_STREAM);
        muxer.writeSampleData(track, b, wbi);
    }

    private long lastWrittenAudio = -1;

    /** The muxer starts once both encoders said what they make (or, at the end, with what there is). */
    private void startMuxer(boolean whatThereIs) {
        if (muxing || (!whatThereIs && (videoFormat == null || audioFormat == null))) {
            return;
        }
        if (videoFormat == null) {
            return; // no picture at all: nothing to keep
        }
        videoTrack = muxer.addTrack(videoFormat);
        if (audioFormat != null) {
            audioTrack = muxer.addTrack(audioFormat);
        }
        muxer.start();
        muxing = true;
        for (Pending p : pending) {
            write(p.isVideo, p.data, p.us, p.flags);
        }
        pending.clear();
    }

    /** The game ended: the encoders finish, the file is closed. */
    private void finish() {
        try {
            video.signalEndOfInputStream();
        } catch (RuntimeException e) {
            videoDone = true;
        }
        int i = -1;
        for (int tries = 0; tries < 100 && i < 0; tries++) {
            i = audio.dequeueInputBuffer(10000);
            if (i < 0) {
                drain(false);
            }
        }
        if (i >= 0) {
            audio.queueInputBuffer(i, 0, 0, audioEnd * 1000000L / rate, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
        } else {
            audioDone = true;
        }
        drain(true);
        startMuxer(true);
    }

    private void releaseAll(boolean keep) {
        if (video != null) {
            try {
                video.stop();
            } catch (RuntimeException ignored) {
                // already stopped
            }
            video.release();
        }
        if (audio != null) {
            try {
                audio.stop();
            } catch (RuntimeException ignored) {
                // already stopped
            }
            audio.release();
        }
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (eglSurface != EGL14.EGL_NO_SURFACE) {
                EGL14.eglDestroySurface(eglDisplay, eglSurface);
            }
            if (eglContext != EGL14.EGL_NO_CONTEXT) {
                EGL14.eglDestroyContext(eglDisplay, eglContext);
            }
            EGL14.eglReleaseThread();
            EGL14.eglTerminate(eglDisplay);
        }
        if (input != null) {
            input.release();
        }
        boolean closed = false;
        if (muxer != null) {
            try {
                if (muxing) {
                    muxer.stop();
                }
                closed = true;
            } catch (RuntimeException e) {
                Log.w(TAG, "recording: the file couldn't be finished", e);
            }
            try {
                muxer.release();
            } catch (RuntimeException ignored) {
                // released
            }
        }
        keep &= closed;
        if (pfd != null) {
            try {
                pfd.close();
            } catch (IOException e) {
                keep = false;
            }
        }
        if (uri != null) {
            ContentResolver cr = app.getContentResolver();
            try {
                if (keep) {
                    ContentValues v = new ContentValues();
                    v.put("is_pending", 0);
                    cr.update(uri, v, null, null);
                } else {
                    cr.delete(uri, null, null);
                }
            } catch (RuntimeException e) {
                Log.w(TAG, "recording: MediaStore", e);
            }
        }
        if (file != null) {
            if (keep) {
                where = file.getPath();
                MediaScannerConnection.scanFile(app, new String[] {file.getPath()}, new String[] {"video/mp4"}, null);
            } else if (file.exists() && !file.delete()) {
                Log.w(TAG, "recording: couldn't delete " + file);
            }
        }
    }
}
