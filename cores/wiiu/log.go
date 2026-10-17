package wiiu

// The Wii U log: what the program printed and what RetroPixels noticed
// (library functions it doesn't have yet, crashes, how far the program
// got), written to "RetroPixels Wii U log.txt" next to RetroPixels so a
// player can send it in when a game doesn't work.

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"time"

	machine "retropixels/wiiu"
)

const (
	logName  = "RetroPixels Wii U log.txt"
	logLimit = 4 << 20 // stop writing past this many bytes
	logEvery = 300     // frames between progress lines
)

type gameLog struct {
	mu sync.Mutex
	f  *os.File
	n  int

	// the watchdog: notes a frame that never finishes (a hang)
	frameStart atomic.Int64 // when the running frame started (unix ns), 0 between frames
	frameNo    atomic.Int64
	stop       chan struct{}
	m          atomic.Pointer[machine.Machine]
}

// openLog starts a new log next to RetroPixels (or in the temp folder when
// that isn't writable). Nil when neither works.
func openLog(path string) *gameLog {
	var dirs []string
	if exe, err := os.Executable(); err == nil {
		dirs = append(dirs, filepath.Dir(exe))
	}
	dirs = append(dirs, os.TempDir())
	for _, d := range dirs {
		f, err := os.Create(filepath.Join(d, logName))
		if err == nil {
			l := &gameLog{f: f, stop: make(chan struct{})}
			go l.watch()
			l.printf("RetroPixels Wii U log, %s", time.Now().Format("2006-01-02 15:04:05"))
			if path != "" {
				size := int64(-1)
				if st, err := os.Stat(path); err == nil {
					size = st.Size()
				}
				l.printf("game: %s (%d bytes)", filepath.Base(path), size)
			}
			return l
		}
	}
	return nil
}

func (l *gameLog) printf(format string, a ...any) {
	if l == nil {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.n > logLimit {
		return
	}
	s := fmt.Sprintf(format, a...) + "\r\n"
	l.n += len(s)
	l.f.WriteString(s)
	if l.n > logLimit {
		l.f.WriteString("(the log stops here: it's big enough)\r\n")
	}
}

// attach writes what the machine logged while it was being made and
// follows the rest.
func (l *gameLog) attach(m *machine.Machine) {
	if l == nil {
		return
	}
	for _, s := range m.RecentLog() {
		l.printf("%s", s)
	}
	l.m.Store(m)
	m.Log = func(s string) { l.printf("%s", s) }
	l.printf("drawing with %s", m.Renderer())
}

// frame notes progress now and then, and any newly missing functions.
func (l *gameLog) frame(m *machine.Machine) {
	if l == nil {
		return
	}
	if m.Frame%logEvery == 0 {
		l.printf("[%s]", m.Stats())
		l.printf("busiest calls: %s", m.BusyCalls())
	}
	if m.Frame == 120 || m.Frame%(logEvery*2) == 0 {
		l.printf("%s", m.ThreadsSummary())
	}
}

// begin and end bracket each frame, for the watchdog.
func (l *gameLog) begin(frame uint64) {
	if l != nil {
		l.frameNo.Store(int64(frame))
		l.frameStart.Store(time.Now().UnixNano())
	}
}

func (l *gameLog) end() {
	if l != nil {
		l.frameStart.Store(0)
	}
}

// watch writes a line when a frame has run for a long time: the program,
// or RetroPixels, is stuck inside it.
func (l *gameLog) watch() {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	var said int64 = -1
	next := 5 * time.Second
	for {
		select {
		case <-l.stop:
			return
		case <-tick.C:
		}
		start := l.frameStart.Load()
		if start == 0 {
			continue
		}
		frame := l.frameNo.Load()
		if frame != said {
			said, next = frame, 5*time.Second
		}
		took := time.Since(time.Unix(0, start))
		if took < next {
			continue
		}
		next *= 3
		last := "?"
		if m := l.m.Load(); m != nil {
			func() {
				defer func() { recover() }()
				last = m.LastCall() + ";" + m.CorePCs()
			}()
		}
		l.printf("frame %d has been running for %.0f s (stuck?); the latest library call was %s", frame, took.Seconds(), last)
	}
}

func (l *gameLog) close() {
	if l != nil {
		close(l.stop)
		l.mu.Lock()
		l.f.Close()
		l.mu.Unlock()
	}
}
