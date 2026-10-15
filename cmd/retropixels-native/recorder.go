package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"retropixels/internal/ffmpeg"
)

// Recording: only the game's picture and sound, straight from the emulator
// (no window frame, menus or mouse pointer), on the game's own clock, so the
// video is smooth whatever the PC is doing.
//
// While recording, the frames go to ffmpeg, which encodes them as they come
// (the picture scaled up sharp to about 720 lines, like the web version's
// recordings), and the sound goes to a .wav file. Stopping joins the two
// into one .mp4 in Videos\RetroPixels.

// recordingsDir: the same folder RetroPixels.exe records to.
func recordingsDir() string {
	if t := os.Getenv("RETRO_REC_DIR"); t != "" {
		return t
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, "Videos", "RetroPixels")
}

// uniqueOut: "<game> <date time>.mp4", with a number added if that's taken.
func uniqueOut(dir, game string, t time.Time) string {
	base := filepath.Join(dir, safeName(game)+" "+t.Format("2006-01-02 15-04-05"))
	out := base + ".mp4"
	for n := 2; ; n++ {
		if _, err := os.Stat(out); err != nil {
			return out
		}
		out = fmt.Sprintf("%s (%d).mp4", base, n)
	}
}

type recorder struct {
	ff         string
	out        string
	videoPath  string
	audioPath  string
	w, h       int // picture size going to ffmpeg (the game's size when recording started)
	rate       int
	started    time.Time
	cmd        *exec.Cmd
	stdin      io.WriteCloser
	frames     chan []byte
	free       chan []byte
	writerDone chan struct{}
	wav        *os.File
	samples    int64 // stereo sample pairs written
	pcm        []int16
	scaled     []uint32
	log        *tailBuf

	mu  sync.Mutex
	err error
}

// recVideoArgs: ffmpeg's options for the picture part.
func recVideoArgs(w, h int, fps float64, outW, outH int, enc, quality, out string) []string {
	args := []string{"-hide_banner", "-loglevel", "error", "-y",
		"-f", "rawvideo", "-pix_fmt", "bgr0", "-s", fmt.Sprintf("%dx%d", w, h),
		"-framerate", strconv.FormatFloat(fps, 'f', 6, 64), "-i", "pipe:0",
		"-vf", fmt.Sprintf("scale=%d:%d:flags=neighbor,setsar=1", outW, outH)}
	args = append(args, ffmpeg.EncoderArgs(enc, quality)...)
	return append(args, "-pix_fmt", "yuv420p", "-f", "matroska", out)
}

// recMuxArgs: joining picture and sound into the final .mp4.
func recMuxArgs(video, audio, out string) []string {
	args := []string{"-hide_banner", "-loglevel", "error", "-y", "-i", video}
	if audio != "" {
		args = append(args, "-i", audio, "-map", "0:v:0", "-map", "1:a:0", "-c:a", "aac", "-b:a", "192k")
	}
	return append(args, "-c:v", "copy", "-movflags", "+faststart", out)
}

// quality: "high", "balanced" or "small" (Record > Quality).
func startRecorder(ff, enc, quality, dir, game string, w, h int, aspect, fps float64, rate int) (*recorder, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("couldn't make the recordings folder: %v", err)
	}
	now := time.Now()
	r := &recorder{ff: ff, out: uniqueOut(dir, game, now), w: w, h: h, rate: rate, started: now,
		frames: make(chan []byte, 180), free: make(chan []byte, 8), writerDone: make(chan struct{}), log: &tailBuf{}}
	r.videoPath = strings.TrimSuffix(r.out, ".mp4") + ".video.part"
	r.audioPath = strings.TrimSuffix(r.out, ".mp4") + ".audio.part"
	ow, oh := recordSize(h, aspect)
	r.cmd = ffmpeg.LowPriority(ffmpeg.Hidden(ff, recVideoArgs(w, h, fps, ow, oh, enc, quality, r.videoPath)...))
	r.cmd.Stderr = r.log
	var err error
	if r.stdin, err = r.cmd.StdinPipe(); err != nil {
		return nil, err
	}
	if r.wav, err = os.Create(r.audioPath); err != nil {
		return nil, fmt.Errorf("couldn't write in the recordings folder: %v", err)
	}
	r.wav.Write(wavHeader(rate, 0))
	if err := r.cmd.Start(); err != nil {
		r.wav.Close()
		os.Remove(r.audioPath)
		return nil, fmt.Errorf("couldn't start ffmpeg: %v", err)
	}
	go r.writer()
	return r, nil
}

func (r *recorder) writer() {
	defer close(r.writerDone)
	failed := false
	for f := range r.frames {
		if !failed {
			if _, err := r.stdin.Write(f); err != nil {
				failed = true
				r.fail(fmt.Errorf("ffmpeg stopped: %s", r.log.String()))
			}
		}
		select {
		case r.free <- f:
		default:
		}
	}
	r.stdin.Close()
}

func (r *recorder) fail(err error) {
	r.mu.Lock()
	if r.err == nil {
		r.err = err
	}
	r.mu.Unlock()
}

func (r *recorder) failed() error {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.err
}

// frame adds one picture (called once per emulated frame).
func (r *recorder) frame(pix []uint32, w, h int) {
	if r.failed() != nil || w <= 0 || h <= 0 || len(pix) < w*h {
		return
	}
	if w != r.w || h != r.h {
		if len(r.scaled) != r.w*r.h {
			r.scaled = make([]uint32, r.w*r.h)
		}
		scaleTo(r.scaled, r.w, r.h, pix, w, h)
		pix = r.scaled
	}
	n := r.w * r.h * 4
	var buf []byte
	select {
	case buf = <-r.free:
	default:
	}
	if len(buf) != n {
		buf = make([]byte, n)
	}
	for i, p := range pix[:r.w*r.h] {
		binary.LittleEndian.PutUint32(buf[i*4:], p)
	}
	r.frames <- buf
}

// audio adds the frame's sound (as the core made it, before the volume).
func (r *recorder) audio(samples []float32, stereo bool) {
	if r.failed() != nil || len(samples) == 0 {
		return
	}
	r.pcm = toPCM(r.pcm, samples, stereo, 1)
	b := make([]byte, len(r.pcm)*2)
	for i, s := range r.pcm {
		binary.LittleEndian.PutUint16(b[i*2:], uint16(s))
	}
	if _, err := r.wav.Write(b); err != nil {
		r.fail(fmt.Errorf("couldn't save the sound: %v", err))
		return
	}
	r.samples += int64(len(r.pcm) / 2)
}

// stop finishes the recording and returns the .mp4's path. It takes a moment
// (ffmpeg finishes the picture, then joins the sound), so call it off the
// game's thread.
func (r *recorder) stop() (string, error) {
	close(r.frames)
	<-r.writerDone
	werr := r.cmd.Wait()
	// the .wav header gets its real length now
	if _, err := r.wav.Seek(0, io.SeekStart); err == nil {
		r.wav.Write(wavHeader(r.rate, r.samples))
	}
	r.wav.Close()
	defer os.Remove(r.videoPath)
	defer os.Remove(r.audioPath)
	if err := r.failed(); err != nil {
		return "", err
	}
	if werr != nil {
		return "", fmt.Errorf("ffmpeg couldn't make the video: %v %s", werr, r.log.String())
	}
	audio := r.audioPath
	if r.samples == 0 {
		audio = ""
	}
	mux := ffmpeg.LowPriority(ffmpeg.Hidden(r.ff, recMuxArgs(r.videoPath, audio, r.out)...))
	log := &tailBuf{}
	mux.Stderr = log
	if err := ffmpeg.RunTimeout(mux, 30*time.Minute); err != nil {
		os.Remove(r.out)
		return "", fmt.Errorf("ffmpeg couldn't save the video: %v %s", err, log.String())
	}
	return r.out, nil
}

// wavHeader: a 16-bit stereo .wav header for n sample pairs.
func wavHeader(rate int, n int64) []byte {
	data := uint32(n * 4)
	h := make([]byte, 44)
	copy(h, "RIFF")
	binary.LittleEndian.PutUint32(h[4:], 36+data)
	copy(h[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(h[16:], 16)
	binary.LittleEndian.PutUint16(h[20:], 1)
	binary.LittleEndian.PutUint16(h[22:], 2)
	binary.LittleEndian.PutUint32(h[24:], uint32(rate))
	binary.LittleEndian.PutUint32(h[28:], uint32(rate*4))
	binary.LittleEndian.PutUint16(h[32:], 4)
	binary.LittleEndian.PutUint16(h[34:], 16)
	copy(h[36:], "data")
	binary.LittleEndian.PutUint32(h[40:], data)
	return h
}

// tailBuf keeps the end of ffmpeg's messages.
type tailBuf struct {
	mu sync.Mutex
	b  []byte
}

func (t *tailBuf) Write(p []byte) (int, error) {
	t.mu.Lock()
	t.b = append(t.b, p...)
	if len(t.b) > 4096 {
		t.b = t.b[len(t.b)-4096:]
	}
	t.mu.Unlock()
	return len(p), nil
}

func (t *tailBuf) String() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return strings.TrimSpace(string(t.b))
}

var errNoFFmpeg = errors.New("ffmpeg isn't installed")
