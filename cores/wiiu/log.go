package wiiu

// The Wii U log: what the program printed and what RetroPixels noticed
// (library functions it doesn't have yet, crashes, how far the program
// got), written to "RetroPixels Wii U log.txt" next to RetroPixels so a
// player can send it in when a game doesn't work.

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	machine "retropixels/wiiu"
)

const (
	logName  = "RetroPixels Wii U log.txt"
	logLimit = 4 << 20 // stop writing past this many bytes
	logEvery = 300     // frames between progress lines
)

type gameLog struct {
	f *os.File
	n int
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
			l := &gameLog{f: f}
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
	if l == nil || l.n > logLimit {
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
		if m.Frame%(logEvery*4) == 0 {
			l.printf("%s", m.ThreadsSummary())
		}
	}
}

func (l *gameLog) close() {
	if l != nil {
		l.f.Close()
	}
}
