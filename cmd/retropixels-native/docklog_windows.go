package main

// The dock log: what dockWatch saw and did while it docked an emulator's game
// screen, so a game that stays in its own window can be looked into. One file
// per emulator (dock-dol.log, dock-pu.log, ...) next to the settings,
// started afresh with each game.

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	"retropixels/internal/dolphin"
)

const dockLogMaxLines = 600

type dockLog struct {
	f      *os.File
	r      *extRun
	start  time.Time
	last   time.Time // when the window list was last written by every
	lines  int
	warned bool // the player was told once that docking failed
}

func newDockLog(ev string, r *extRun) *dockLog {
	lg := &dockLog{r: r, start: time.Now()}
	if os.MkdirAll(appDir(), 0o755) == nil {
		lg.f, _ = os.Create(filepath.Join(appDir(), "dock-"+ev+".log"))
	}
	lg.printf("RetroPixels Native %s: docking %s (pid %d)", version, r.image, r.pid)
	return lg
}

func (lg *dockLog) printf(format string, args ...any) {
	if lg == nil || lg.f == nil || lg.lines >= dockLogMaxLines {
		return
	}
	lg.lines++
	fmt.Fprintf(lg.f, "%8.2fs  %s\n", time.Since(lg.start).Seconds(), fmt.Sprintf(format, args...))
}

// windows writes the emulator's top-level windows (all its processes).
func (lg *dockLog) windows() {
	pids := []int{lg.r.pid}
	if lg.r.image != "" {
		for _, p := range dolphin.PIDsOf(lg.r.image) {
			if p != lg.r.pid {
				pids = append(pids, p)
			}
		}
	}
	for _, p := range pids {
		ws := dolphin.DescribeWindows(p)
		lg.printf("process %d: %d windows", p, len(ws))
		for _, w := range ws {
			lg.printf("    %s", w)
		}
	}
}

// every writes the window list at most once per d, for the first 30 seconds.
func (lg *dockLog) every(d time.Duration) {
	if lg == nil || time.Since(lg.start) > 30*time.Second || time.Since(lg.last) < d {
		return
	}
	lg.last = time.Now()
	lg.windows()
}

func (lg *dockLog) close() {
	if lg != nil && lg.f != nil {
		lg.f.Close()
	}
}
