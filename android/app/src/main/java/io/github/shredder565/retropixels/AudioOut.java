package io.github.shredder565.retropixels;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;

/**
 * The game's sound: 16-bit stereo at the phone's own rate (so nothing is
 * resampled), written without waiting. How much is queued paces the game
 * (Pacer), so the sound's clock sets the speed, as in the native app.
 */
final class AudioOut {
    final int rate;
    private final AudioTrack track;
    private final int bufferFrames;
    private long written; // frames written since the last flush

    AudioOut() {
        int r = AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC);
        if (r < 8000 || r > 192000) {
            r = 48000;
        }
        rate = r;
        int min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        int bytes = Math.max(min * 2, rate / 5 * 4); // at least 200 ms
        AudioTrack t;
        try {
            t = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(rate)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .build())
                    .setBufferSizeInBytes(bytes)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
            if (t.getState() != AudioTrack.STATE_INITIALIZED) {
                t.release();
                t = null;
            }
        } catch (RuntimeException e) {
            t = null;
        }
        track = t;
        bufferFrames = t != null ? t.getBufferSizeInFrames() : 0;
        if (t != null) {
            t.play();
        }
    }

    boolean ok() {
        return track != null;
    }

    /** How much sound the output holds, in seconds. */
    double bufferSeconds() {
        return track == null ? 0 : bufferFrames / (double) rate;
    }

    /** Seconds of sound waiting to be played (-1 with no sound output: the clock alone paces the game). */
    double queued() {
        if (track == null) {
            return -1;
        }
        long head = track.getPlaybackHeadPosition() & 0xffffffffL;
        long q = written - head;
        return q < 0 ? 0 : q / (double) rate;
    }

    /** Writes s16 stereo samples (what doesn't fit is dropped). */
    void write(short[] s, int n) {
        if (track == null || n <= 0) {
            return;
        }
        int w = track.write(s, 0, n, AudioTrack.WRITE_NON_BLOCKING);
        if (w > 0) {
            written += w / 2;
        }
    }

    void pause() {
        if (track != null) {
            track.pause();
        }
    }

    void resume() {
        if (track != null) {
            track.play();
        }
    }

    /** Drops the queued sound (after a reset or a loaded state). */
    void clear() {
        if (track == null) {
            return;
        }
        track.pause();
        track.flush();
        written = 0;
        track.play();
    }

    void release() {
        if (track != null) {
            track.pause();
            track.flush();
            track.release();
        }
    }
}
