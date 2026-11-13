package main

// Recording (-record DIR -ffmpeg PATH, given by RetroPixels when it
// records games automatically): every frame the game makes goes to ffmpeg
// as it is, which writes DIR\Frames\pixelds_framedump0.mkv, and the
// game's sound goes to DIR\Audio\pixelds_dspdump.wav (48 kHz, 16-bit,
// stereo). RetroPixels makes the finished video from those two when the
// game ends, as it does for its other emulators. Both follow the game's
// own clock, so the video is smooth however busy the PC is.

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
)

type recorder struct {
	cmd     *exec.Cmd
	video   io.WriteCloser
	wav     *os.File
	samples int64
	frames  int64
	buf     []byte
	failed  bool
}

// newRecorder starts recording when RetroPixels asked for it (nil when
// not, or when ffmpeg won't start: then the game just isn't recorded).
func newRecorder(e interface {
	Width() int
	Height() int
	FPS() float64
}) *recorder {
	if *recordDir == "" || *ffmpegPath == "" {
		return nil
	}
	r, err := startRecording(*recordDir, *ffmpegPath, e.Width(), e.Height(), e.FPS(), sampleRate)
	if err != nil {
		fmt.Fprintln(os.Stderr, "PixelDS: not recording:", err)
		return nil
	}
	return r
}

// startRecording starts ffmpeg for a w×h picture at fps frames a second.
func startRecording(dir, ff string, w, h int, fps float64, rate int) (*recorder, error) {
	if err := os.MkdirAll(filepath.Join(dir, "Frames"), 0o755); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(filepath.Join(dir, "Audio"), 0o755); err != nil {
		return nil, err
	}
	wav, err := os.Create(filepath.Join(dir, "Audio", "pixelds_dspdump.wav"))
	if err != nil {
		return nil, err
	}
	wav.Write(wavHeader(rate, 0))
	cmd := exec.Command(ff, "-hide_banner", "-loglevel", "warning", "-y",
		"-f", "rawvideo", "-pix_fmt", "bgr0", "-video_size", fmt.Sprintf("%dx%d", w, h),
		"-framerate", strconv.FormatFloat(fps, 'f', 4, 64), "-i", "pipe:0", "-an",
		// lossless at the screens' own size, full colour (4:4:4): RetroPixels
		// scales it up with crisp pixels when it makes the .mp4, so the
		// picture is only compressed once
		"-c:v", "libx264", "-preset", "ultrafast", "-qp", "0", "-pix_fmt", "yuv444p",
		filepath.Join(dir, "Frames", "pixelds_framedump0.mkv"))
	hideConsole(cmd)
	in, err := cmd.StdinPipe()
	if err != nil {
		wav.Close()
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		wav.Close()
		return nil, err
	}
	return &recorder{cmd: cmd, video: in, wav: wav}, nil
}

func wavHeader(rate int, dataBytes uint32) []byte {
	h := make([]byte, 44)
	le := binary.LittleEndian
	copy(h, "RIFF")
	le.PutUint32(h[4:], 36+dataBytes)
	copy(h[8:], "WAVEfmt ")
	le.PutUint32(h[16:], 16)
	le.PutUint16(h[20:], 1)
	le.PutUint16(h[22:], 2)
	le.PutUint32(h[24:], uint32(rate))
	le.PutUint32(h[28:], uint32(rate*4))
	le.PutUint16(h[32:], 4)
	le.PutUint16(h[34:], 16)
	copy(h[36:], "data")
	le.PutUint32(h[40:], dataBytes)
	return h
}

// frame records one frame's picture and the sound made with it. A frame
// without sound (a 3DS game before it starts its sound) gets silence, so
// the two stay together.
func (r *recorder) frame(pic []uint32, sound []float32, fps float64, rate int) {
	if r == nil || r.failed {
		return
	}
	if cap(r.buf) < len(pic)*4 {
		r.buf = make([]byte, len(pic)*4)
	}
	b := r.buf[:len(pic)*4]
	for i, c := range pic {
		binary.LittleEndian.PutUint32(b[4*i:], c)
	}
	if _, err := r.video.Write(b); err != nil {
		r.failed = true
		return
	}
	// the sound this frame should have, by the game's clock
	want := int64(float64(r.frames+1)*float64(rate)/fps) - r.samples
	pcm := make([]byte, 0, int(max(want, int64(len(sound)/2)))*4)
	n := int64(len(sound) / 2)
	if n == 0 {
		n = max(want, 0)
		pcm = append(pcm, make([]byte, n*4)...)
	} else {
		for _, s := range sound {
			v := int16(max(-1, min(1, s)) * 32767)
			pcm = binary.LittleEndian.AppendUint16(pcm, uint16(v))
		}
	}
	r.wav.Write(pcm)
	r.samples += n
	r.frames++
}

// stop finishes the files.
func (r *recorder) stop(rate int) {
	if r == nil {
		return
	}
	r.video.Close()
	r.cmd.Wait()
	r.wav.Seek(0, io.SeekStart)
	r.wav.Write(wavHeader(rate, uint32(r.samples*4)))
	r.wav.Close()
}
