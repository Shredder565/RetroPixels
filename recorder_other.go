//go:build !windows

package main

// Test stand-ins so the recorder can be exercised off Windows.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

func hiddenCommand(name string, args ...string) *exec.Cmd { return exec.Command(name, args...) }

// RETRO_TEST_WIN="L,T,R,B;monL,monT,monR,monB;window title" (file path also accepted so tests can change it)
func findGameWindow(pid int) (gameWindow, bool) {
	spec := os.Getenv("RETRO_TEST_WIN")
	if b, err := os.ReadFile(spec); err == nil {
		spec = strings.TrimSpace(string(b))
	}
	parts := strings.SplitN(spec, ";", 3)
	if len(parts) < 3 {
		return gameWindow{}, false
	}
	var c, m rect
	fmt.Sscanf(parts[0], "%d,%d,%d,%d", &c.L, &c.T, &c.R, &c.B)
	fmt.Sscanf(parts[1], "%d,%d,%d,%d", &m.L, &m.T, &m.R, &m.B)
	return gameWindow{Title: parts[2], Client: c, Output: m}, true
}

type toneCapture struct{}

func (toneCapture) Format() audioFormat { return audioFormat{Sample: "s16le", Rate: 48000, Ch: 2} }
func (toneCapture) Source() string      { return "game" }
func (toneCapture) Run(write func([]byte), stop <-chan struct{}) error {
	t := time.NewTicker(20 * time.Millisecond)
	defer t.Stop()
	n := 0
	for {
		select {
		case <-stop:
			return nil
		case <-t.C:
			buf := make([]byte, 960*4)
			for i := 0; i < 960; i++ {
				v := int16(8000 * math.Sin(2*math.Pi*440*float64(n)/48000))
				binary.LittleEndian.PutUint16(buf[i*4:], uint16(v))
				binary.LittleEndian.PutUint16(buf[i*4+2:], uint16(v))
				n++
			}
			write(buf)
		}
	}
}

func openProcessAudio(pid int) (audioCapture, error) {
	if os.Getenv("RETRO_TEST_NOAUDIO") == "1" {
		return nil, errors.New("no audio")
	}
	return toneCapture{}, nil
}
func openSystemAudio() (audioCapture, error) { return nil, errors.New("no system audio in tests") }

func newAudioPipe() (*audioPipe, error) {
	dir, err := os.MkdirTemp("", "retroaudio")
	if err != nil {
		return nil, err
	}
	p := filepath.Join(dir, "audio.fifo")
	if err := syscall.Mkfifo(p, 0o600); err != nil {
		return nil, err
	}
	return &audioPipe{
		Path:    p,
		openFn:  func() (io.WriteCloser, error) { return os.OpenFile(p, os.O_WRONLY, 0) },
		closeFn: func() { os.RemoveAll(dir) },
	}, nil
}

func init() {
	if os.Getenv("RETRO_TEST_VIDEO") == "1" { // stand-in for the Windows screen grabber
		videoSource = func(win gameWindow, c crop, fps int) string {
			return fmt.Sprintf("testsrc2=size=%dx%d:rate=%d,realtime,format=bgra", c.W, c.H, fps)
		}
	}
}
