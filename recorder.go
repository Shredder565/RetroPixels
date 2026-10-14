package main

// Automatic gameplay recording for games played in Dolphin.
//
// When a game starts, RetroPixels finds the emulator's game window,
// works out the part of the screen the game actually draws into (cropping the
// black bars a fullscreen game leaves on a monitor with a different shape),
// captures only the emulator's own sound, and has ffmpeg write an .mp4.

import (
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type rect struct{ L, T, R, B int }

func (r rect) W() int { return r.R - r.L }
func (r rect) H() int { return r.B - r.T }
func (r rect) intersect(o rect) rect {
	x := rect{max(r.L, o.L), max(r.T, o.T), min(r.R, o.R), min(r.B, o.B)}
	if x.R < x.L {
		x.R = x.L
	}
	if x.B < x.T {
		x.B = x.T
	}
	return x
}
func near(a, b int) bool { return a-b <= 2 && b-a <= 2 }
func (r rect) same(o rect) bool {
	return near(r.L, o.L) && near(r.T, o.T) && near(r.R, o.R) && near(r.B, o.B)
}

// gameWindow describes the emulator window on screen.
type gameWindow struct {
	Handle   uintptr
	Title    string
	Client   rect // window's drawing area, in screen pixels
	Output   rect // the monitor it's on (DXGI desktop coordinates)
	Adapter  int  // DXGI adapter index of that monitor
	OutIndex int  // DXGI output index on that adapter
	Embedded bool // docked inside the RetroPixels window
}

func (g gameWindow) Fullscreen() bool { return g.Client.same(g.Output) }

type crop struct{ X, Y, W, H int } // relative to the monitor

// gameCrop returns the part of the monitor the game draws into. In fullscreen
// (and when docked inside RetroPixels, where the game fills the window too)
// the emulator letterboxes the game to its aspect ratio, so the black bars
// are cut off by fitting that ratio into the window.
func gameCrop(win gameWindow, aspect float64) (crop, error) {
	area := win.Client.intersect(win.Output)
	if area.W() < 64 || area.H() < 64 {
		return crop{}, errors.New("the game window is too small or off screen")
	}
	x, y, w, h := float64(area.L), float64(area.T), float64(area.W()), float64(area.H())
	if aspect > 0 && (win.Fullscreen() || win.Embedded) {
		if w/h > aspect { // wider than the game: bars left and right
			nw := h * aspect
			x += (w - nw) / 2
			w = nw
		} else if w/h < aspect { // taller than the game: bars top and bottom
			nh := w / aspect
			y += (h - nh) / 2
			h = nh
		}
	}
	c := crop{X: int(math.Round(x)) - win.Output.L, Y: int(math.Round(y)) - win.Output.T, W: int(w) &^ 1, H: int(h) &^ 1}
	if c.X < 0 {
		c.X = 0
	}
	if c.Y < 0 {
		c.Y = 0
	}
	if c.X+c.W > win.Output.W() {
		c.W = (win.Output.W() - c.X) &^ 1
	}
	if c.Y+c.H > win.Output.H() {
		c.H = (win.Output.H() - c.Y) &^ 1
	}
	return c, nil
}

// ---------------- settings ----------------

func autoRecordOn() bool {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	return cfg.AutoRecord == nil || *cfg.AutoRecord
}

func recFPS() int {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	if cfg.RecordFPS == 30 {
		return 30
	}
	return 60
}

func recQuality() string {
	cfgMu.Lock()
	defer cfgMu.Unlock()
	switch cfg.RecordQuality {
	case "high", "small":
		return cfg.RecordQuality
	}
	return "balanced"
}

func recordingsDir() string {
	cfgMu.Lock()
	d := cfg.RecordDir
	cfgMu.Unlock()
	if d != "" {
		return d
	}
	if t := os.Getenv("RETRO_REC_DIR"); t != "" {
		return t
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, "Videos", "RetroPixels")
}

// ---------------- ffmpeg ----------------

func findFFmpeg() string {
	exe, _ := os.Executable()
	dir := filepath.Dir(exe)
	cands := []string{
		filepath.Join(dir, "ffmpeg", "ffmpeg.exe"),
		filepath.Join(dir, "ffmpeg.exe"),
		filepath.Join(dir, "ffmpeg", "bin", "ffmpeg.exe"),
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" {
		cands = append(cands, filepath.Join(la, "RetroPixels", "ffmpeg", "ffmpeg.exe"))
	}
	for _, p := range cands {
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	if p := os.Getenv("RETRO_FFMPEG"); p != "" {
		return p
	}
	if p, err := exec.LookPath("ffmpeg"); err == nil {
		return p
	}
	return ""
}

type audioFormat struct {
	Sample string // ffmpeg raw format: s16le / f32le
	Rate   int
	Ch     int
}

var encoderOrder = []string{"h264_nvenc", "h264_amf", "h264_qsv", "libx264"}

func encoderArgs(enc, quality string) []string {
	q := map[string][3]string{ // high, balanced, small
		"h264_nvenc": {"18", "21", "26"},
		"h264_amf":   {"18", "22", "27"},
		"h264_qsv":   {"18", "22", "27"},
		"libx264":    {"18", "21", "26"},
	}[enc]
	v := q[1]
	if quality == "high" {
		v = q[0]
	} else if quality == "small" {
		v = q[2]
	}
	switch enc {
	case "h264_nvenc":
		return []string{"-c:v", "h264_nvenc", "-preset", "p5", "-tune", "hq", "-rc", "vbr", "-cq", v, "-b:v", "0", "-profile:v", "high"}
	case "h264_amf":
		return []string{"-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", v, "-qp_p", v}
	case "h264_qsv":
		return []string{"-c:v", "h264_qsv", "-preset", "medium", "-global_quality", v}
	}
	return []string{"-c:v", "libx264", "-preset", "veryfast", "-crf", v}
}

// videoSource is the ffmpeg filter that grabs the screen. Tests swap it out.
var videoSource = func(win gameWindow, c crop, fps int) string {
	return fmt.Sprintf("ddagrab=output_idx=%d:framerate=%d:draw_mouse=0:offset_x=%d:offset_y=%d:video_size=%dx%d,hwdownload,format=bgra",
		win.OutIndex, fps, c.X, c.Y, c.W, c.H)
}

func ffmpegArgs(enc string, win gameWindow, c crop, fps int, quality string, a *audioFormat, audioPath, out string) []string {
	args := []string{"-hide_banner", "-loglevel", "warning", "-nostats", "-progress", "pipe:1", "-stats_period", "0.5", "-y"}
	if win.Adapter > 0 {
		args = append(args, "-init_hw_device", fmt.Sprintf("d3d11va=dd:%d", win.Adapter), "-filter_hw_device", "dd")
	}
	if a != nil {
		args = append(args, "-thread_queue_size", "4096", "-probesize", "32", "-analyzeduration", "0",
			"-f", a.Sample, "-ar", fmt.Sprint(a.Rate), "-ac", fmt.Sprint(a.Ch), "-i", audioPath)
	}
	graph := videoSource(win, c, fps)
	switch enc {
	case "libx264":
		graph += ",format=yuv420p"
	case "h264_qsv":
		graph += ",format=nv12"
	}
	args = append(args, "-filter_complex", graph+"[v]", "-map", "[v]")
	args = append(args, encoderArgs(enc, quality)...)
	args = append(args, "-g", fmt.Sprint(fps*2))
	if a != nil {
		args = append(args, "-map", "0:a", "-c:a", "aac", "-b:a", "192k", "-af", "aresample=async=1000")
	}
	return append(args, "-movflags", "+faststart", out)
}

var (
	encMu    sync.Mutex
	encFound map[string]bool
)

// encoderWorks test-encodes a few frames to see if this PC supports an encoder.
func encoderWorks(ff, enc string) bool {
	encMu.Lock()
	defer encMu.Unlock()
	if encFound == nil {
		encFound = map[string]bool{}
	}
	if v, ok := encFound[enc]; ok {
		return v
	}
	args := append([]string{"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=black:s=640x360:r=30:d=0.3"},
		encoderArgs(enc, "balanced")...)
	args = append(args, "-pix_fmt", "yuv420p", "-f", "null", "-")
	cmd := hiddenCommand(ff, args...)
	err := runWithTimeout(cmd, 15*time.Second)
	encFound[enc] = err == nil
	return err == nil
}

func runWithTimeout(cmd *exec.Cmd, d time.Duration) error {
	if err := cmd.Start(); err != nil {
		return err
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		return err
	case <-time.After(d):
		cmd.Process.Kill()
		return errors.New("timed out")
	}
}

// ---------------- audio ----------------

// audioCapture streams raw PCM from the emulator (or the whole PC as a fallback).
type audioCapture interface {
	Format() audioFormat
	Source() string // "game" or "system"
	Run(write func([]byte), stop <-chan struct{}) error
}

// ---------------- recording session ----------------

type recSession struct {
	Game     string
	File     string
	Started  time.Time
	Encoder  string
	Audio    string
	Crop     crop
	Monitor  string
	cmd      *exec.Cmd
	stdin    io.WriteCloser
	stopOnce sync.Once
	audOnce  sync.Once
	stopAud  chan struct{}
	exited   chan struct{}
}

var (
	recMu       sync.Mutex
	recCur      *recSession
	recLastErr  string
	recLastMsg  string
	recTrimming string                 // file being tidied up after recording
	recManual   = make(chan string, 4) // "start" / "stop" while a game runs
)

func safeFile(s string) string {
	s = regexp.MustCompile(`[<>:"/\\|?*\x00-\x1f™®]+`).ReplaceAllString(s, "")
	s = strings.TrimSpace(strings.TrimRight(s, ". "))
	if s == "" {
		return "Game"
	}
	if len(s) > 80 {
		s = s[:80]
	}
	return s
}

func setRecError(msg string) {
	recMu.Lock()
	recLastErr = msg
	recMu.Unlock()
}

// startRecording begins capturing the given window. It tries the PC's video
// encoders in order (NVIDIA, AMD, Intel, then software).
func startRecording(game string, pid int, win gameWindow, aspect float64) (*recSession, error) {
	ff := findFFmpeg()
	if ff == "" {
		return nil, errors.New("Recording needs ffmpeg. Go to the Recordings tab and click Download ffmpeg")
	}
	c, err := gameCrop(win, aspect)
	if err != nil {
		return nil, err
	}
	dir := recordingsDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	out := filepath.Join(dir, safeFile(game)+" "+time.Now().Format("2006-01-02 15-04-05")+".mp4")
	fps, quality := recFPS(), recQuality()

	logPath := filepath.Join(dir, "logs", "last-recording.log")
	os.MkdirAll(filepath.Dir(logPath), 0o755)
	logf, _ := os.Create(logPath)
	logLine := func(f string, a ...any) {
		if logf != nil {
			fmt.Fprintf(logf, f+"\n", a...)
		}
	}

	lastErr := errors.New("no video encoder worked on this PC")
	for _, enc := range encoderOrder {
		if !encoderWorks(ff, enc) {
			logLine("encoder %s not available", enc)
			continue
		}
		s, err := tryRecording(ff, enc, game, pid, win, c, fps, quality, out, logf, logLine)
		if err != nil {
			lastErr = fmt.Errorf("%v (details in %s)", err, logPath)
			logLine("%s failed: %v", enc, err)
			continue
		}
		recMu.Lock()
		recCur, recLastErr, recLastMsg = s, "", ""
		recMu.Unlock()
		return s, nil
	}
	os.Remove(out)
	return nil, lastErr
}

func tryRecording(ff, enc, game string, pid int, win gameWindow, c crop, fps int, quality, out string,
	logf io.Writer, logLine func(string, ...any)) (*recSession, error) {
	// Game sound (or the whole PC's sound if that isn't possible).
	var ac audioCapture
	if a, err := openProcessAudio(pid); err == nil {
		ac = a
	} else {
		logLine("game-only audio unavailable: %v", err)
		if a, err := openSystemAudio(); err == nil {
			ac = a
		} else {
			logLine("PC audio unavailable: %v", err)
		}
	}
	var af *audioFormat
	var ap *audioPipe
	if ac != nil {
		f := ac.Format()
		var err error
		if ap, err = newAudioPipe(); err != nil {
			logLine("audio pipe: %v", err)
			ac = nil
		} else {
			af = &f
		}
	}
	audioPath := ""
	if ap != nil {
		audioPath = ap.Path
	}

	args := ffmpegArgs(enc, win, c, fps, quality, af, audioPath, out)
	logLine("\n=== %s  %s  crop=%+v\nffmpeg %s", time.Now().Format(time.RFC3339), enc, c, strings.Join(args, " "))
	cmd := hiddenCommand(ff, args...)
	if logf != nil {
		cmd.Stderr = logf
	}
	progress, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		if ap != nil {
			ap.Close()
		}
		return nil, err
	}
	s := &recSession{Game: game, File: out, Started: time.Now(), Encoder: enc, Crop: c, cmd: cmd, stdin: stdin,
		stopAud: make(chan struct{}), exited: make(chan struct{}), Audio: "none",
		Monitor: fmt.Sprintf("%dx%d", win.Output.W(), win.Output.H())}
	go func() { cmd.Wait(); close(s.exited) }()

	// ffmpeg reports progress on stdout; the first encoded frame means video is flowing.
	videoReady := make(chan struct{})
	var readyFrames int // frames already encoded when video was first reported
	go func() {
		buf := make([]byte, 4096)
		var line []byte
		ready := false
		for {
			n, err := progress.Read(buf)
			for _, ch := range buf[:n] {
				if ch != '\n' {
					line = append(line, ch)
					continue
				}
				l := strings.TrimSpace(string(line))
				line = line[:0]
				if !ready && strings.HasPrefix(l, "frame=") && l != "frame=0" {
					ready = true
					fmt.Sscan(strings.TrimPrefix(l, "frame="), &readyFrames)
					close(videoReady)
				}
			}
			if err != nil {
				return
			}
		}
	}()

	if ap != nil {
		s.Audio = ac.Source()
		data := make(chan []byte, 1024)
		go func() {
			w, err := ap.Open() // waits for ffmpeg to open the pipe
			if err != nil {
				logLine("audio pipe open: %v", err)
				for range data {
				}
				return
			}
			defer w.Close()
			// A sliver of silence lets ffmpeg identify the stream. Sound captured
			// while ffmpeg is still starting is dropped so it lines up with the video.
			f := ac.Format()
			bytesPer := map[string]int{"s16le": 2, "s24le": 3, "s32le": 4, "f32le": 4}[f.Sample] * f.Ch
			packet := make([]byte, 1024*bytesPer) // ffmpeg reads raw sound 1024 samples at a time
			w.Write(packet)
			w.Write(packet)
			sent := 2048 // samples of silence written so far
			startedAt := time.Now()
			lastTopUp := startedAt
			live := false
			for b := range data {
				if !live {
					select {
					case <-videoReady:
						live = true
					default:
						// Still starting: a small top-up now and then so ffmpeg never waits on us.
						if time.Since(startedAt) > 800*time.Millisecond && time.Since(lastTopUp) > 400*time.Millisecond {
							w.Write(packet)
							sent += 1024
							lastTopUp = time.Now()
						}
						continue
					}
					// Line the sound up with the video: by now ffmpeg has encoded
					// readyFrames frames (plus a few still inside the encoder).
					lag := 4
					if enc == "libx264" {
						lag = 12
					}
					target := (readyFrames + lag) * f.Rate / fps
					if pad := target - sent; pad > 0 {
						w.Write(make([]byte, pad*bytesPer))
					} else if drop := -pad * bytesPer; drop > 0 {
						if drop >= len(b) {
							continue
						}
						b = b[drop:]
					}
				}
				if _, err := w.Write(b); err != nil {
					for range data {
					}
					return
				}
			}
		}()
		go func() {
			err := ac.Run(func(b []byte) {
				select {
				case data <- b:
				default: // ffmpeg fell behind; drop rather than stall the capture
				}
			}, s.stopAud)
			close(data)
			if err != nil {
				logLine("audio: %v", err)
			}
		}()
	}

	// Wait for video to start. If ffmpeg gives up, the caller tries the next encoder.
	select {
	case <-videoReady:
		return s, nil
	case <-s.exited:
	case <-time.After(15 * time.Second):
		cmd.Process.Kill()
		<-s.exited
	}
	s.audOnce.Do(func() { close(s.stopAud) })
	if ap != nil {
		ap.Close()
	}
	os.Remove(out)
	return nil, fmt.Errorf("%s didn't start", enc)
}

// stop ends the recording: "q" tells ffmpeg to finish writing the file.
func (s *recSession) stop() {
	s.stopOnce.Do(func() {
		io.WriteString(s.stdin, "q")
		time.AfterFunc(5*time.Second, func() { s.stdin.Close() })
	})
	select {
	case <-s.exited:
	case <-time.After(20 * time.Second):
		s.cmd.Process.Kill()
		<-s.exited
	}
	s.audOnce.Do(func() { close(s.stopAud) })
	recMu.Lock()
	if recCur == s {
		recCur = nil
	}
	if st, err := os.Stat(s.File); err == nil && st.Size() > 0 {
		recLastMsg = "Saved " + filepath.Base(s.File)
	} else {
		recLastErr = "The recording didn't save. Details are in " + filepath.Join(recordingsDir(), "logs", "last-recording.log")
	}
	recMu.Unlock()
}

// gameShowing: the emulator's game window is up. (Dolphin is started in a mode
// where its only window is the game itself.)
func gameShowing(emuExe string, w gameWindow) bool { return w.Handle != 0 || w.Client.W() > 0 }

// recOpts: Aspect is the game's picture shape (<0 = 16:9, 0 = fill the
// window); PostCrop trims any black bars left in the finished video.
type recOpts struct {
	Aspect   float64
	PostCrop bool
}

// autoRecord follows one emulator run: records while a game is on screen and
// stops when it ends. done closes when the emulator exits.
func autoRecord(game, emuExe string, pid int, done <-chan struct{}, opt recOpts) {
	aspect := opt.Aspect
	if aspect < 0 {
		aspect = 16.0 / 9
	}
	var cur *recSession
	var lastRect rect
	stableSince := time.Time{}
	userStopped := false
	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	finish := func() {
		if cur != nil {
			cur.stop()
			if opt.PostCrop {
				go trimBlackBars(cur.File, cur.Encoder)
			}
			cur = nil
		}
	}
	defer finish()
	for {
		select {
		case <-done:
			return
		case m := <-recManual:
			switch m {
			case "stop":
				finish()
				userStopped = true
			case "start":
				userStopped = false
				stableSince = time.Time{}
			}
		case <-tick.C:
		}
		win, ok := findGameWindow(pid)
		showing := ok && gameShowing(emuExe, win)
		if cur != nil {
			if !showing {
				finish()
			} else {
				select {
				case <-cur.exited: // ffmpeg stopped by itself
					cur.stop()
					cur = nil
					setRecError("Recording stopped unexpectedly. Details are in " + filepath.Join(recordingsDir(), "logs", "last-recording.log"))
					userStopped = true
				default:
				}
			}
			continue
		}
		if !showing {
			stableSince = time.Time{}
			userStopped = false // next game in this session records again
			continue
		}
		if userStopped || !(autoRecordOn() || manualWanted()) {
			continue
		}
		// No ffmpeg yet: fetch it now and start recording as soon as it's ready,
		// instead of skipping this game.
		if findFFmpeg() == "" {
			if st := ffStatus()["state"]; st == "idle" || st == "" {
				startFFmpegInstall()
			}
			if st := ffStatus()["state"]; st != "error" && st != "done" {
				continue // downloading: try again on a later tick
			}
		}
		// Wait until the window has settled (going fullscreen takes a moment).
		if !win.Client.same(lastRect) || stableSince.IsZero() {
			lastRect = win.Client
			stableSince = time.Now()
			continue
		}
		if time.Since(stableSince) < 1500*time.Millisecond {
			continue
		}
		s, err := startRecording(game, pid, win, aspect)
		clearManualWanted()
		if err != nil {
			setRecError(err.Error())
			userStopped = true
			continue
		}
		cur = s
	}
}

var (
	manualMu   sync.Mutex
	manualFlag bool
)

func manualWanted() bool     { manualMu.Lock(); defer manualMu.Unlock(); return manualFlag }
func clearManualWanted()     { manualMu.Lock(); manualFlag = false; manualMu.Unlock() }
func setManualWanted(v bool) { manualMu.Lock(); manualFlag = v; manualMu.Unlock() }

// ---------------- status & routes ----------------

func recStatus() map[string]any {
	recMu.Lock()
	defer recMu.Unlock()
	st := map[string]any{
		"auto":     autoRecordOn(),
		"fps":      recFPS(),
		"quality":  recQuality(),
		"dir":      recordingsDir(),
		"ffmpeg":   findFFmpeg() != "",
		"install":  ffStatus(),
		"error":    recLastErr,
		"message":  recLastMsg,
		"trimming": recTrimming,
	}
	if recCur != nil {
		st["active"] = map[string]any{
			"game": recCur.Game, "file": filepath.Base(recCur.File), "started": recCur.Started,
			"encoder": recCur.Encoder, "audio": recCur.Audio,
			"size": fmt.Sprintf("%dx%d", recCur.Crop.W, recCur.Crop.H), "monitor": recCur.Monitor,
		}
	}
	return st
}

func listRecordings() []map[string]any {
	entries, _ := os.ReadDir(recordingsDir())
	out := []map[string]any{}
	recMu.Lock()
	active := ""
	if recCur != nil {
		active = filepath.Base(recCur.File)
	}
	recMu.Unlock()
	for _, e := range entries {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".mp4") || e.Name() == active || strings.Contains(e.Name(), ".trimming.") {
			continue
		}
		if info, err := e.Info(); err == nil {
			out = append(out, map[string]any{"name": e.Name(), "size": info.Size(), "time": info.ModTime()})
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i]["time"].(time.Time).After(out[j]["time"].(time.Time)) })
	if len(out) > 20 {
		out = out[:20]
	}
	return out
}

func registerRecorderRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/rec/status", guard(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]any{"status": recStatus(), "recordings": listRecordings()})
	}))
	mux.HandleFunc("/api/rec/settings", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Auto    *bool  `json:"auto"`
			FPS     int    `json:"fps"`
			Quality string `json:"quality"`
		}
		if !readBody(w, r, &b) {
			return
		}
		cfgMu.Lock()
		if b.Auto != nil {
			v := *b.Auto
			cfg.AutoRecord = &v
		}
		if b.FPS == 30 || b.FPS == 60 {
			cfg.RecordFPS = b.FPS
		}
		if b.Quality == "high" || b.Quality == "balanced" || b.Quality == "small" {
			cfg.RecordQuality = b.Quality
		}
		_ = saveConfigLocked()
		cfgMu.Unlock()
		writeJSON(w, 200, map[string]any{"status": recStatus()})
	}))
	mux.HandleFunc("/api/rec/control", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Action string `json:"action"`
		}
		if !readBody(w, r, &b) {
			return
		}
		runMu.Lock()
		gameOn := running != nil
		runMu.Unlock()
		switch b.Action {
		case "start":
			if !gameOn {
				fail(w, errors.New("start a GameCube or Wii game first"))
				return
			}
			setRecError("")
			setManualWanted(true)
			recManual <- "start"
		case "stop":
			recManual <- "stop"
		case "open-folder":
			os.MkdirAll(recordingsDir(), 0o755)
			if err := openFolder(recordingsDir()); err != nil {
				fail(w, err)
				return
			}
		case "choose-folder":
			dir, err := pickFolder("Choose where to save recordings", recordingsDir())
			if err != nil {
				fail(w, err)
				return
			}
			if dir != "" {
				cfgMu.Lock()
				cfg.RecordDir = dir
				_ = saveConfigLocked()
				cfgMu.Unlock()
			}
		default:
			fail(w, errors.New("unknown action"))
			return
		}
		time.Sleep(150 * time.Millisecond)
		writeJSON(w, 200, map[string]any{"status": recStatus()})
	}))
	mux.HandleFunc("/api/rec/open", guard(func(w http.ResponseWriter, r *http.Request) {
		var b struct {
			Name string `json:"name"`
		}
		if !readBody(w, r, &b) {
			return
		}
		if b.Name != filepath.Base(b.Name) || !strings.EqualFold(filepath.Ext(b.Name), ".mp4") {
			fail(w, errors.New("bad file name"))
			return
		}
		if err := openFolder(filepath.Join(recordingsDir(), b.Name)); err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	}))
}

var reCrop = regexp.MustCompile(`crop=(\d+):(\d+):(\d+):(\d+)`)
var reSize = regexp.MustCompile(`Video: h264[^\n]*?, (\d+)x(\d+)`)

// trimBlackBars finds the area the game actually drew into across the whole
// video and, if there are black bars, re-encodes the video without them.
func trimBlackBars(file, enc string) {
	ff := findFFmpeg()
	if ff == "" {
		return
	}
	out, err := hiddenCommand(ff, "-hide_banner", "-skip_frame", "nokey", "-i", file,
		"-vf", "cropdetect=limit=24:round=2:reset=0", "-an", "-f", "null", "-").CombinedOutput()
	if err != nil {
		return
	}
	s := string(out)
	sz := reSize.FindStringSubmatch(s)
	all := reCrop.FindAllStringSubmatch(s, -1)
	if sz == nil || len(all) == 0 {
		return
	}
	atoi := func(x string) int { n, _ := strconv.Atoi(x); return n }
	fw, fh := atoi(sz[1]), atoi(sz[2])
	last := all[len(all)-1]
	w, h, x, y := atoi(last[1])&^1, atoi(last[2])&^1, atoi(last[3])&^1, atoi(last[4])&^1
	l, r, t, b := x, fw-w-x, y, fh-h-y
	// Only trim real bars (at least 2% of the picture), never something odd.
	if w < fw/3 || h < fh/3 || l < 0 || r < 0 || t < 0 || b < 0 || (l+r < fw/50 && t+b < fh/50) {
		return
	}
	recMu.Lock()
	recTrimming = filepath.Base(file)
	recMu.Unlock()
	defer func() { recMu.Lock(); recTrimming = ""; recMu.Unlock() }()

	tmp := strings.TrimSuffix(file, filepath.Ext(file)) + ".trimming.mp4"
	vf := fmt.Sprintf("crop=%d:%d:%d:%d", w, h, x, y)
	if enc == "" || !encoderWorks(ff, enc) {
		enc = "libx264"
	}
	if enc == "libx264" || enc == "h264_qsv" {
		vf += ",format=" + map[string]string{"libx264": "yuv420p", "h264_qsv": "nv12"}[enc]
	}
	args := []string{"-hide_banner", "-loglevel", "error", "-y", "-i", file, "-map", "0", "-vf", vf}
	args = append(args, encoderArgs(enc, "high")...)
	args = append(args, "-c:a", "copy", "-movflags", "+faststart", tmp)
	if err := hiddenCommand(ff, args...).Run(); err != nil {
		os.Remove(tmp)
		return
	}
	if st, err := os.Stat(tmp); err != nil || st.Size() == 0 {
		os.Remove(tmp)
		return
	}
	if err := os.Rename(tmp, file); err != nil {
		os.Remove(tmp)
	}
}

// audioPipe carries raw sound to ffmpeg (a named pipe on Windows, a FIFO elsewhere).
type audioPipe struct {
	Path    string
	openFn  func() (io.WriteCloser, error)
	closeFn func()
	once    sync.Once
}

func (a *audioPipe) Open() (io.WriteCloser, error) { return a.openFn() }
func (a *audioPipe) Close()                        { a.once.Do(a.closeFn) }
