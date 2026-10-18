package main

// Gameplay recording for games played in Dolphin, and in PixelU (Wii U).
//
// Dolphin itself saves the game's picture and sound while the game runs (its
// "frame dump" and "audio dump"). That's only the game - no mouse pointer, no
// other windows, no black bars - and it follows the game's own clock, so the
// video is smooth however busy the PC is. When the game ends, RetroPixels
// turns those raw files into one .mp4 with ffmpeg and deletes them.
//
// PixelU does the same (asked with --rp-record, see internal/pixelu): just
// the TV picture as .mkv files in Frames and its sound as a .wav in Audio,
// so the same steps make its video.

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

type crop struct{ X, Y, W, H int }

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

func recLogPath() string { return filepath.Join(recordingsDir(), "logs", "last-recording.log") }

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

// ---------------- what Dolphin is asked to save ----------------

// dumpBitrate: Dolphin's raw video is only an in-between file, so it gets a
// high bitrate; RetroPixels makes the final, smaller .mp4 from it.
func dumpBitrate(quality string) int {
	switch quality {
	case "high":
		return 50000
	case "small":
		return 25000
	}
	return 35000
}

// dolphinRecordArgs: Dolphin settings for one run (-C doesn't change its
// saved settings) that make it save the game's picture and sound.
//
// The picture is saved as MPEG-4 in an .avi at a high bitrate: that's
// Dolphin's own default and is quick to encode, so it doesn't slow the game
// (a lossless codec would make files many times bigger, or cost more CPU).
// It's saved at the size the game is shown on screen, which is exactly the
// game picture without the black bars around it.
func dolphinRecordArgs(quality string) []string {
	kv := []string{
		"Dolphin.Movie.DumpFrames=True",
		"Dolphin.Movie.DumpFramesSilent=True", // never stop to ask about overwriting a file
		"Dolphin.DSP.DumpAudio=True",
		"Dolphin.DSP.DumpAudioSilent=True",
		"Graphics.Settings.DumpFormat=avi",
		"Graphics.Settings.DumpCodec=mpeg4",
		"Graphics.Settings.DumpEncoder=",
		"Graphics.Settings.UseFFV1=False",
		"Graphics.Settings.DumpFramesAsImages=False",
		"Graphics.Settings.DumpPath=", // a fixed file name would overwrite itself when the picture size changes
		"Graphics.Settings.InternalResolutionFrameDumps=False",
		fmt.Sprintf("Graphics.Settings.BitrateKbps=%d", dumpBitrate(quality)),
	}
	var args []string
	for _, s := range kv {
		args = append(args, "-C", s)
	}
	return args
}

// ---------------- Dolphin's dump files ----------------

type dumpFiles struct {
	Video []string // Dump\Frames\*.avi (Dolphin) or *.mkv (PixelU), in order (a new file starts when the picture size changes)
	DSP   []string // Dump\Audio\*dspdump*.wav: the game's sound
	DTK   []string // Dump\Audio\*dtkdump*.wav: music streamed from the disc (GameCube)
}

func (d dumpFiles) all() []string {
	return append(append(append([]string{}, d.Video...), d.DSP...), d.DTK...)
}

func (d dumpFiles) remove() {
	for _, f := range d.all() {
		os.Remove(f)
	}
}

// findDumps lists the dump files Dolphin wrote since a game started.
func findDumps(dir string, since time.Time) dumpFiles {
	pick := func(sub string, ok func(name string) bool) []string {
		type file struct {
			path string
			t    time.Time
		}
		var list []file
		entries, _ := os.ReadDir(filepath.Join(dir, sub))
		for _, e := range entries {
			name := strings.ToLower(e.Name())
			if e.IsDir() || !ok(name) {
				continue
			}
			p := filepath.Join(dir, sub, e.Name())
			st, err := os.Stat(p)
			if err != nil || st.ModTime().Before(since.Add(-2*time.Second)) {
				continue // from an earlier game
			}
			list = append(list, file{p, st.ModTime()})
		}
		// Oldest first: a file is finished before the next one starts.
		sort.SliceStable(list, func(i, j int) bool {
			if !list[i].t.Equal(list[j].t) {
				return list[i].t.Before(list[j].t)
			}
			return list[i].path < list[j].path
		})
		var out []string
		for _, f := range list {
			out = append(out, f.path)
		}
		return out
	}
	return dumpFiles{
		Video: pick("Frames", func(n string) bool { return strings.HasSuffix(n, ".avi") || strings.HasSuffix(n, ".mkv") }),
		DSP:   pick("Audio", func(n string) bool { return strings.HasSuffix(n, ".wav") && strings.Contains(n, "dspdump") }),
		DTK:   pick("Audio", func(n string) bool { return strings.HasSuffix(n, ".wav") && strings.Contains(n, "dtkdump") }),
	}
}

// wavSeconds: how much sound a .wav holds, worked out from its real size
// (the header's own length is only filled in when Dolphin closes the file).
func wavSeconds(path string) float64 {
	f, err := os.Open(path) // opening it reads the size as it is now, even while Dolphin writes
	if err != nil {
		return 0
	}
	defer f.Close()
	size, err := f.Seek(0, io.SeekEnd)
	if err != nil {
		return 0
	}
	f.Seek(0, io.SeekStart)
	hdr := make([]byte, 512)
	n, _ := io.ReadFull(f, hdr)
	hdr = hdr[:n]
	if len(hdr) < 12 || string(hdr[0:4]) != "RIFF" || string(hdr[8:12]) != "WAVE" {
		return 0
	}
	le16 := func(b []byte) int { return int(b[0]) | int(b[1])<<8 }
	le32 := func(b []byte) int { return le16(b) | le16(b[2:])<<16 }
	bytesPerSec := 0
	for off := 12; off+8 <= len(hdr); {
		id, sz := string(hdr[off:off+4]), le32(hdr[off+4:])
		body := off + 8
		switch id {
		case "fmt ":
			if body+16 <= len(hdr) {
				bytesPerSec = le32(hdr[body+8:])
			}
		case "data":
			if bytesPerSec <= 0 {
				return 0
			}
			return float64(size-int64(body)) / float64(bytesPerSec)
		}
		off = body + sz + sz&1
	}
	return 0
}

func wavTotal(files []string) float64 {
	t := 0.0
	for _, f := range files {
		t += wavSeconds(f)
	}
	return t
}

// ---------------- one game's recording ----------------

// mark is a moment in the game: how much sound Dolphin had saved by then
// (which follows the game's own clock) and the time on the PC's clock.
type mark struct {
	Audio float64 // seconds of game sound saved so far, -1 if unknown
	Wall  time.Time
	Edge  bool // the very start (From) or end (To) of the game
}

// clip is one stretch to keep: the whole game, or the part between pressing
// Record and Stop.
type clip struct {
	From, To mark
	Open     bool // still recording
}

type recSession struct {
	Game     string
	Launched time.Time
	Ended    time.Time
	dumpDir  string
	Emu      string // "PixelU (Wii/GameCube)" or "PixelU" (Wii U): named in messages
	Quality  string // "" = the Recordings setting
	NoCrop   bool   // keep every pixel: the emulator saves only the game's own picture, without bars
	ownDir   bool   // dumpDir was made for this game alone: removed once the video is made

	mu         sync.Mutex
	clips      []clip
	firstVideo time.Time // when Dolphin's first video file appeared
	warned     bool
}

var (
	recMu      sync.Mutex
	recCur     *recSession // the game being played, when Dolphin saves it
	recLastErr string
	recLastMsg string
	recSaving  *saveState
	recQueued  int
	saveMu     sync.Mutex // one recording is saved at a time
)

type saveState struct {
	Game    string
	Step    string // "ffmpeg" (waiting for it), "saving"
	Percent int
}

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

// newRecSession: a game about to start with Dolphin saving it (PixelU sets
// the other fields after).
func newRecSession(game, dumpDir string) *recSession {
	now := time.Now()
	return &recSession{Game: game, Launched: now, dumpDir: dumpDir, Emu: wiiEngineLabel,
		clips: []clip{{From: mark{Audio: 0, Wall: now, Edge: true}, Open: true}}}
}

// markNow: where the game is up to right now.
func (s *recSession) markNow() mark {
	m := mark{Audio: -1, Wall: time.Now()}
	d := findDumps(s.dumpDir, s.Launched)
	// Same choice of sound as when the video is made. If it can't be read
	// right now, the PC's clock is used instead.
	if a := wavTotal(d.DSP); a > 0 {
		m.Audio = a
	} else if a := wavTotal(d.DTK); a > 0 && len(d.DSP) == 0 {
		m.Audio = a
	}
	return m
}

// stop ends the current clip. The game is still saved by Dolphin until it
// ends (that can't be switched off from outside), so the rest is cut off
// when the video is made.
func (s *recSession) stop() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := len(s.clips)
	if n == 0 || !s.clips[n-1].Open {
		return false
	}
	s.clips[n-1].To = s.markNow()
	s.clips[n-1].Open = false
	return true
}

// resume starts a new clip from now; it becomes a separate video.
func (s *recSession) resume() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if n := len(s.clips); n > 0 && s.clips[n-1].Open {
		return
	}
	s.clips = append(s.clips, clip{From: s.markNow(), Open: true})
}

// recording: whether a clip is open, and since when.
func (s *recSession) recording() (bool, time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := len(s.clips)
	if n == 0 || !s.clips[n-1].Open {
		return false, time.Time{}
	}
	c := s.clips[n-1]
	if c.From.Edge {
		if !s.firstVideo.IsZero() {
			return true, s.firstVideo
		}
		return true, s.Launched
	}
	return true, c.From.Wall
}

func (s *recSession) logf(f string, a ...any) {
	p := recLogPath()
	os.MkdirAll(filepath.Dir(p), 0o755)
	if lf, err := os.OpenFile(p, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
		fmt.Fprintf(lf, f+"\n", a...)
		lf.Close()
	}
}

// startRecSession is called as the game starts.
func startRecSession(s *recSession, args []string) {
	os.MkdirAll(filepath.Dir(recLogPath()), 0o755)
	os.WriteFile(recLogPath(), []byte(fmt.Sprintf("=== %s  %s\n%s saves the game to %s\n%s %s\n",
		s.Launched.Format(time.RFC3339), s.Game, s.Emu, s.dumpDir, s.Emu, strings.Join(args, " "))), 0o644)
	recMu.Lock()
	recCur, recLastErr, recLastMsg = s, "", ""
	recMu.Unlock()
}

var recNoVideoAfter = 45 * time.Second

// follow watches one game while it runs; when it ends, the video is made.
func (s *recSession) follow(done <-chan struct{}) {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case <-done:
			s.mu.Lock()
			s.Ended = time.Now()
			if n := len(s.clips); n > 0 && s.clips[n-1].Open {
				s.clips[n-1].To = mark{Audio: -1, Wall: s.Ended, Edge: true}
				s.clips[n-1].Open = false
			}
			s.mu.Unlock()
			recMu.Lock()
			if recCur == s {
				recCur = nil
			}
			recMu.Unlock()
			s.finish()
			return
		case <-tick.C:
		}
		s.mu.Lock()
		waiting := s.firstVideo.IsZero()
		s.mu.Unlock()
		if !waiting {
			continue
		}
		if d := findDumps(s.dumpDir, s.Launched); len(d.Video) > 0 {
			s.mu.Lock()
			s.firstVideo = time.Now()
			warned := s.warned
			s.mu.Unlock()
			s.logf("%s started saving the picture: %s", s.Emu, filepath.Base(d.Video[0]))
			if warned {
				setRecError("")
			}
		} else if time.Since(s.Launched) > recNoVideoAfter {
			s.mu.Lock()
			first := !s.warned
			s.warned = true
			s.mu.Unlock()
			if first {
				s.logf("No video from %s yet in %s", s.Emu, filepath.Join(s.dumpDir, "Frames"))
				setRecError(s.Emu + " hasn't started saving the game's picture, so this game may not be recorded. " +
					s.upToDate() + " Details are in " + recLogPath())
			}
		}
	}
}

// upToDate: which emulator version can save games.
func (s *recSession) upToDate() string {
	if s.Emu == "PixelU" {
		return "A PixelU build that can record (made with RetroPixels' recording support) is needed."
	}
	return `The PixelU folder's Wii/GameCube engine (wii\PixelU-Wii.exe) should save it; try an up-to-date PixelU folder.`
}

// ---------------- making the video ----------------

// clipRange turns a clip into start and end times in the video. Dolphin's
// sound starts a moment before its first picture, and both end together when
// the game closes, so the sound is lined up with the video by their lengths.
func clipRange(c clip, videoLen, audioLen float64, firstVideo time.Time) (from, to float64) {
	off := 0.0 // sound time = video time + off
	if audioLen > 0 {
		off = audioLen - videoLen
	}
	at := func(m mark, end bool) float64 {
		switch {
		case m.Edge && end:
			return videoLen
		case m.Edge:
			return 0
		case audioLen > 0 && m.Audio >= 0:
			return m.Audio - off
		case !firstVideo.IsZero():
			return m.Wall.Sub(firstVideo).Seconds()
		}
		return 0
	}
	clamp := func(v float64) float64 { return math.Max(0, math.Min(videoLen, v)) }
	return clamp(at(c.From, false)), clamp(at(c.To, true))
}

type vidSeg struct {
	Path string
	Dur  float64
	W, H int
	FPS  float64
	Crop *crop // black bars to cut off, if any
}

type finalJob struct {
	Video          []vidSeg
	DSP, DTK       []string
	DSPLen, DTKLen float64
	From, To       float64 // part of the video to keep, in seconds
	FPS            float64
	Out            string
}

func ffNum(v float64) string { return strconv.FormatFloat(v, 'f', 3, 64) }

// finalSize: every part of the video is scaled to the biggest one (Dolphin
// starts a new file when the picture size changes, e.g. the window is resized).
func finalSize(segs []vidSeg) (int, int) {
	w, h := 0, 0
	for _, s := range segs {
		sw, sh := s.W, s.H
		if s.Crop != nil {
			sw, sh = s.Crop.W, s.Crop.H
		}
		if sw*sh > w*h {
			w, h = sw, sh
		}
	}
	return w &^ 1, h &^ 1
}

// finalArgs builds the ffmpeg command that joins Dolphin's files into the .mp4.
func finalArgs(j finalJob, enc, quality string) []string {
	args := []string{"-hide_banner", "-loglevel", "warning", "-nostats", "-progress", "pipe:1", "-y"}
	for _, v := range j.Video {
		args = append(args, "-i", v.Path)
	}
	for _, a := range append(append([]string{}, j.DSP...), j.DTK...) {
		args = append(args, "-ignore_length", "1", "-i", a) // the header may not have its final length
	}
	W, H := finalSize(j.Video)
	var g []string
	labels := ""
	videoLen := 0.0
	for i, v := range j.Video {
		f := ""
		if v.Crop != nil {
			f = fmt.Sprintf("crop=%d:%d:%d:%d,", v.Crop.W, v.Crop.H, v.Crop.X, v.Crop.Y)
		}
		if v.W != W || v.H != H || v.Crop != nil {
			// A part of a different shape fills the frame (a little is cut off
			// rather than stretching it or adding bars).
			f += fmt.Sprintf("scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d,", W, H, W, H)
		}
		g = append(g, fmt.Sprintf("[%d:v]%ssetsar=1[v%d]", i, f, i))
		labels += fmt.Sprintf("[v%d]", i)
		videoLen += v.Dur
	}
	if len(j.Video) > 1 {
		labels += fmt.Sprintf("concat=n=%d:v=1:a=0,", len(j.Video))
	}
	pix := "yuv420p"
	if enc == "h264_qsv" {
		pix = "nv12"
	}
	dur := j.To - j.From
	g = append(g, fmt.Sprintf("%sfps=%s,trim=start=%s:end=%s,setpts=PTS-STARTPTS,format=%s[v]",
		labels, ffNum(j.FPS), ffNum(j.From), ffNum(j.To), pix))

	// Sound: both kinds started together, so both get the same shift.
	clock := j.DSPLen
	if clock <= 0 {
		clock = j.DTKLen
	}
	off := clock - videoLen
	in := len(j.Video)
	var mix []string
	group := func(files []string, name string) {
		if len(files) == 0 {
			return
		}
		ls := ""
		for k := range files {
			g = append(g, fmt.Sprintf("[%d:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo[%s%d]", in, name, k))
			ls += fmt.Sprintf("[%s%d]", name, k)
			in++
		}
		var f []string
		if len(files) > 1 {
			f = append(f, fmt.Sprintf("concat=n=%d:v=0:a=1", len(files)))
		}
		skip := 0.0
		if off < 0 { // sound shorter than the video: silence first
			f = append(f, fmt.Sprintf("adelay=%d:all=1", int(math.Round(-off*1000))))
		} else {
			skip = off
		}
		f = append(f, fmt.Sprintf("atrim=start=%s:end=%s,asetpts=PTS-STARTPTS", ffNum(j.From+skip), ffNum(j.To+skip)))
		g = append(g, ls+strings.Join(f, ",")+"["+name+"]")
		mix = append(mix, "["+name+"]")
	}
	if j.DSPLen > 0 {
		group(j.DSP, "dsp")
	} else {
		in += len(j.DSP)
	}
	if j.DTKLen > 0 {
		group(j.DTK, "dtk")
	}
	pad := fmt.Sprintf("apad=whole_dur=%s,atrim=end=%s[a]", ffNum(dur), ffNum(dur))
	switch len(mix) {
	case 1:
		g = append(g, mix[0]+pad)
	case 2:
		g = append(g, mix[0]+mix[1]+"amix=inputs=2:duration=longest:normalize=0,"+pad)
	}
	args = append(args, "-filter_complex", strings.Join(g, ";"), "-map", "[v]")
	if len(mix) > 0 {
		args = append(args, "-map", "[a]")
	}
	args = append(args, encoderArgs(enc, quality)...)
	args = append(args, "-g", fmt.Sprint(int(math.Round(j.FPS*2))))
	if len(mix) > 0 {
		args = append(args, "-c:a", "aac", "-b:a", "192k")
	}
	return append(args, "-movflags", "+faststart", j.Out)
}

var (
	reCrop   = regexp.MustCompile(`crop=(\d+):(\d+):(\d+):(\d+)`)
	reVidHdr = regexp.MustCompile(`Stream #[^\n]*Video: [^\n]*?, (\d{2,5})x(\d{2,5})[^\n]*`)
	reFPS    = regexp.MustCompile(`, ([\d.]+) (?:fps|tbr)`)
	reOutUS  = regexp.MustCompile(`out_time_us=(\d+)`)
)

// parseBars reads ffmpeg's cropdetect output: the area the game actually drew
// into. Only real bars count (at least 2% of the picture), never something odd.
func parseBars(out string, fw, fh int) *crop {
	all := reCrop.FindAllStringSubmatch(out, -1)
	if len(all) == 0 || fw <= 0 || fh <= 0 {
		return nil
	}
	atoi := func(x string) int { n, _ := strconv.Atoi(x); return n }
	last := all[len(all)-1]
	w, h, x, y := atoi(last[1])&^1, atoi(last[2])&^1, atoi(last[3])&^1, atoi(last[4])&^1
	l, r, t, b := x, fw-w-x, y, fh-h-y
	if w < fw/3 || h < fh/3 || l < 0 || r < 0 || t < 0 || b < 0 || (l+r < fw/50 && t+b < fh/50) {
		return nil
	}
	return &crop{X: x, Y: y, W: w, H: h}
}

// probeSegment reads one of Dolphin's video files: length, size, frame rate
// and any black bars the game itself draws around the picture.
func probeSegment(ff, path string) (vidSeg, error) {
	v := vidSeg{Path: path}
	// Length: copying the stream to nowhere reads to the end quickly, and
	// works even if Dolphin couldn't finish the file.
	cmd := lowPriority(hiddenCommand(ff, "-hide_banner", "-nostats", "-progress", "pipe:1", "-i", path,
		"-map", "0:v:0", "-c", "copy", "-f", "null", "-"))
	out, err := cmd.Output()
	if m := reOutUS.FindAllStringSubmatch(string(out), -1); len(m) > 0 {
		us, _ := strconv.ParseInt(m[len(m)-1][1], 10, 64)
		v.Dur = float64(us) / 1e6
	}
	// Size, frame rate and bars (looking at key frames from the first 15 minutes).
	cmd = lowPriority(hiddenCommand(ff, "-hide_banner", "-skip_frame", "nokey", "-t", "900", "-i", path,
		"-map", "0:v:0", "-vf", "cropdetect=limit=24:round=2:reset=0", "-an", "-f", "null", "-"))
	info, _ := cmd.CombinedOutput()
	s := string(info)
	if m := reVidHdr.FindStringSubmatch(s); m != nil {
		v.W, _ = strconv.Atoi(m[1])
		v.H, _ = strconv.Atoi(m[2])
		if f := reFPS.FindStringSubmatch(m[0]); f != nil {
			v.FPS, _ = strconv.ParseFloat(f[1], 64)
		}
	}
	if v.W == 0 || v.H == 0 {
		if err == nil {
			err = errors.New("ffmpeg couldn't read it")
		}
		return v, fmt.Errorf("%s: %v", filepath.Base(path), err)
	}
	v.Crop = parseBars(s, v.W, v.H)
	return v, nil
}

// outputFPS: Dolphin saves the picture at the console's refresh rate (about
// 60 or 50 a second). The 30 FPS setting keeps every other frame.
func outputFPS(src float64, want int) float64 {
	if src < 20 || src > 125 {
		src = 60
	}
	if want == 30 && src > 40 {
		return src / 2
	}
	return src
}

func diskFull(msg string) bool {
	m := strings.ToLower(msg)
	return strings.Contains(m, "no space left") || strings.Contains(m, "not enough space") || strings.Contains(m, "disk full") ||
		strings.Contains(m, "disk is full")
}

var errDiskFull = errors.New("disk full")

// tail keeps the end of ffmpeg's messages.
type tail struct{ b []byte }

func (t *tail) Write(p []byte) (int, error) {
	t.b = append(t.b, p...)
	if len(t.b) > 8192 {
		t.b = t.b[len(t.b)-8192:]
	}
	return len(p), nil
}

// encodeClip makes one .mp4, trying the PC's video encoders in order
// (NVIDIA, AMD, Intel, then software).
func encodeClip(ff string, j finalJob, quality string, logf func(string, ...any), progress func(sec float64)) error {
	lastErr := errors.New("no video encoder worked on this PC")
	for _, enc := range encoderOrder {
		if !encoderWorks(ff, enc) {
			logf("encoder %s not available", enc)
			continue
		}
		args := finalArgs(j, enc, quality)
		logf("ffmpeg %s", strings.Join(args, " "))
		cmd := lowPriority(hiddenCommand(ff, args...))
		errs := &tail{}
		cmd.Stderr = errs
		stdout, err := cmd.StdoutPipe()
		if err != nil {
			return err
		}
		if err := cmd.Start(); err != nil {
			return err
		}
		buf := make([]byte, 4096)
		for {
			n, rerr := stdout.Read(buf)
			if m := reOutUS.FindAllSubmatch(buf[:n], -1); len(m) > 0 {
				us, _ := strconv.ParseInt(string(m[len(m)-1][1]), 10, 64)
				progress(float64(us) / 1e6)
			}
			if rerr != nil {
				break
			}
		}
		err = cmd.Wait()
		logf("%s", strings.TrimSpace(string(errs.b)))
		if st, serr := os.Stat(j.Out); err == nil && serr == nil && st.Size() > 0 {
			return nil
		}
		os.Remove(j.Out)
		if diskFull(string(errs.b)) {
			return errDiskFull
		}
		if err == nil {
			err = errors.New("ffmpeg made an empty file")
		}
		lastErr = fmt.Errorf("%s: %v", enc, err)
		logf("%v", lastErr)
	}
	return lastErr
}

var ffWaitPoll = 2 * time.Second

// waitForFFmpeg: ffmpeg is needed to make the video. If it isn't there yet
// it's downloaded now, and the recording waits for it.
func waitForFFmpeg(setStep func(string)) string {
	asked, told := false, false
	for {
		if ff := findFFmpeg(); ff != "" {
			return ff
		}
		setStep("ffmpeg")
		st, _ := ffStatus()["state"].(string)
		switch st {
		case "downloading", "verifying", "installing":
		case "error":
			if !told {
				told = true
				setRecError("ffmpeg couldn't be downloaded, so your recording is waiting to be saved. " +
					"Go to the Recordings tab and click Download ffmpeg to try again (keep RetroPixels open).")
			}
		default:
			if !asked {
				asked = true
				startFFmpegInstall()
			}
		}
		time.Sleep(ffWaitPoll)
	}
}

func setSaving(f func(*saveState)) {
	recMu.Lock()
	if recSaving != nil {
		f(recSaving)
	}
	recMu.Unlock()
}

// uniqueOut: "<game> <date time>.mp4", with a number added if that's taken.
func uniqueOut(dir, game string, t time.Time) string {
	base := filepath.Join(dir, safeFile(game)+" "+t.Format("2006-01-02 15-04-05"))
	out := base + ".mp4"
	for n := 2; ; n++ {
		if _, err := os.Stat(out); err != nil {
			return out
		}
		out = fmt.Sprintf("%s (%d).mp4", base, n)
	}
}

// finish makes the .mp4 (one per clip) from Dolphin's files once the game
// has ended, then deletes those files.
func (s *recSession) finish() {
	recMu.Lock()
	recQueued++
	recMu.Unlock()
	saveMu.Lock()
	defer saveMu.Unlock()
	recMu.Lock()
	recQueued--
	recSaving = &saveState{Game: s.Game, Step: "saving"}
	recMu.Unlock()
	defer func() { recMu.Lock(); recSaving = nil; recMu.Unlock() }()
	if s.ownDir {
		defer os.RemoveAll(s.dumpDir) // runs last: after the raw files are removed
	}

	d := findDumps(s.dumpDir, s.Launched)
	s.logf("\n=== game ended after %s\nvideo: %v\nsound: %v %v", s.Ended.Sub(s.Launched).Round(time.Second), d.Video, d.DSP, d.DTK)
	if len(d.Video) == 0 {
		d.remove()
		if s.Ended.Sub(s.Launched) > 20*time.Second {
			setRecError(s.Emu + " didn't save the game's picture, so nothing was recorded. " +
				s.upToDate() + " Details are in " + recLogPath())
		}
		return
	}
	defer d.remove() // the raw files are huge

	s.mu.Lock()
	clips := append([]clip{}, s.clips...)
	firstVideo := s.firstVideo
	s.mu.Unlock()
	if firstVideo.IsZero() {
		firstVideo = s.Launched
	}

	ff := waitForFFmpeg(func(step string) { setSaving(func(st *saveState) { st.Step = step }) })
	setSaving(func(st *saveState) { st.Step = "saving" })

	var segs []vidSeg
	videoLen := 0.0
	for _, f := range d.Video {
		v, err := probeSegment(ff, f)
		if s.NoCrop {
			v.Crop = nil
		}
		if err != nil || v.Dur < 0.05 {
			s.logf("skipping %s: %v (%.2fs)", filepath.Base(f), err, v.Dur)
			continue
		}
		s.logf("%s: %.2fs %dx%d %.3f fps bars=%v", filepath.Base(f), v.Dur, v.W, v.H, v.FPS, v.Crop)
		segs = append(segs, v)
		videoLen += v.Dur
	}
	if len(segs) == 0 {
		setRecError(s.Emu + "'s video for this game couldn't be read, so it wasn't saved. Details are in " + recLogPath())
		return
	}
	j := finalJob{Video: segs, DSP: d.DSP, DTK: d.DTK, DSPLen: wavTotal(d.DSP), DTKLen: wavTotal(d.DTK),
		FPS: outputFPS(segs[0].FPS, recFPS())}
	audioLen := j.DSPLen
	if audioLen <= 0 {
		audioLen = j.DTKLen
	}
	s.logf("video %.2fs, sound %.2fs (disc music %.2fs)", videoLen, j.DSPLen, j.DTKLen)

	dir := recordingsDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		setRecError("Couldn't make the recordings folder: " + err.Error())
		return
	}
	quality := recQuality()
	if s.Quality != "" {
		quality = s.Quality
	}
	type part struct {
		from, to float64
		wall     time.Time
	}
	var parts []part
	total := 0.0
	for _, c := range clips {
		from, to := clipRange(c, videoLen, audioLen, firstVideo)
		if to-from < 1 {
			s.logf("skipping a clip under a second long (%.2f-%.2f)", from, to)
			continue
		}
		w := c.From.Wall
		if c.From.Edge {
			w = firstVideo
		}
		parts = append(parts, part{from, to, w})
		total += to - from
	}
	var saved []string
	failMsg := ""
	done := 0.0
	for _, p := range parts {
		out := uniqueOut(dir, s.Game, p.wall)
		j.From, j.To = p.from, p.to
		j.Out = strings.TrimSuffix(out, ".mp4") + ".saving.mp4"
		s.logf("making %s from %.2fs to %.2fs", filepath.Base(out), p.from, p.to)
		err := encodeClip(ff, j, quality, s.logf, func(sec float64) {
			pc := int(100 * (done + math.Min(sec, p.to-p.from)) / total)
			setSaving(func(st *saveState) { st.Percent = pc })
		})
		done += p.to - p.from
		if err == errDiskFull {
			failMsg = "Your disk is full, so the recording couldn't be saved. Free up some space (or choose another folder in the Recordings tab) and try again."
			break
		}
		if err != nil {
			failMsg = "The recording couldn't be saved. Details are in " + recLogPath()
			s.logf("failed: %v", err)
			continue
		}
		if err := os.Rename(j.Out, out); err != nil {
			os.Remove(j.Out)
			failMsg = "The recording couldn't be saved: " + err.Error()
			continue
		}
		saved = append(saved, filepath.Base(out))
		s.logf("saved %s", out)
	}
	recMu.Lock()
	if failMsg != "" {
		recLastErr = failMsg
	}
	if len(saved) > 0 {
		recLastMsg = "Saved " + strings.Join(saved, ", ")
	}
	recMu.Unlock()
}

// ---------------- recording the next game ----------------

var (
	manualMu   sync.Mutex
	manualFlag bool
)

func manualWanted() bool     { manualMu.Lock(); defer manualMu.Unlock(); return manualFlag }
func clearManualWanted()     { manualMu.Lock(); manualFlag = false; manualMu.Unlock() }
func setManualWanted(v bool) { manualMu.Lock(); manualFlag = v; manualMu.Unlock() }

// recordingWanted: whether the game about to start should be recorded.
func recordingWanted() bool { return autoRecordOn() || manualWanted() }

// ---------------- status & routes ----------------

func recStatus() map[string]any {
	recMu.Lock()
	defer recMu.Unlock()
	st := map[string]any{
		"auto":    autoRecordOn(),
		"fps":     recFPS(),
		"quality": recQuality(),
		"dir":     recordingsDir(),
		"ffmpeg":  findFFmpeg() != "",
		"install": ffStatus(),
		"error":   recLastErr,
		"message": recLastMsg,
		"next":    manualWanted(),
		"dumping": recCur != nil,
		"queued":  recQueued,
	}
	if recCur != nil {
		if on, since := recCur.recording(); on {
			st["active"] = map[string]any{"game": recCur.Game, "started": since}
		}
	}
	if recSaving != nil {
		st["saving"] = map[string]any{"game": recSaving.Game, "step": recSaving.Step, "percent": recSaving.Percent}
	}
	return st
}

func listRecordings() []map[string]any {
	entries, _ := os.ReadDir(recordingsDir())
	out := []map[string]any{}
	for _, e := range entries {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".mp4") || strings.Contains(e.Name(), ".saving.") || strings.Contains(e.Name(), ".trimming.") {
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

// recControl handles Record / Stop while a game is running. It returns a
// short note to show the player.
func recControl(action string) (string, error) {
	runMu.Lock()
	gameOn := running != nil
	emu := "PixelU"
	if gameOn && running.Emu != "" {
		emu = running.Emu
	}
	runMu.Unlock()
	recMu.Lock()
	s := recCur
	recMu.Unlock()
	switch action {
	case "start":
		setRecError("")
		if s != nil {
			s.resume()
			return "Recording", nil
		}
		// Dolphin and PixelU can only be told to save a game as it starts.
		setManualWanted(true)
		if gameOn {
			return "Recording will start when you start your next game (" + emu + " can only record from the start of a game)", nil
		}
		return "Your next game will be recorded", nil
	case "stop":
		if s != nil && s.stop() {
			return "Recording stopped. The video is saved when the game ends", nil
		}
		if manualWanted() {
			setManualWanted(false)
			return "Your next game won't be recorded", nil
		}
		return "", nil
	}
	return "", errors.New("unknown action")
}

func registerRecorderRoutes(mux *http.ServeMux) {
	// A recording made in the page (the built-in systems record their own
	// picture there) is saved in the recordings folder, next to the others.
	mux.HandleFunc("/api/rec/upload", guard(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			fail(w, errors.New("send the video with POST"))
			return
		}
		p, err := saveUploadedRecording(r.URL.Query().Get("name"), http.MaxBytesReader(w, r.Body, 8<<30))
		if err != nil {
			fail(w, err)
			return
		}
		writeJSON(w, 200, map[string]any{"ok": true, "file": filepath.Base(p), "dir": filepath.Dir(p)})
	}))
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
		note := ""
		switch b.Action {
		case "start", "stop":
			var err error
			if note, err = recControl(b.Action); err != nil {
				fail(w, err)
				return
			}
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
		writeJSON(w, 200, map[string]any{"status": recStatus(), "note": note})
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

// saveUploadedRecording writes a video from the page into the recordings
// folder as name (cleaned up; .mp4 or .webm), never over an existing file.
func saveUploadedRecording(name string, body io.Reader) (string, error) {
	ext := strings.ToLower(filepath.Ext(name))
	if ext != ".mp4" && ext != ".webm" {
		ext = ".mp4"
	}
	base := strings.TrimSuffix(filepath.Base(strings.ReplaceAll(name, "\\", "/")), filepath.Ext(name))
	base = strings.Map(func(r rune) rune {
		if r < 32 || strings.ContainsRune(`<>:"/\\|?*`, r) {
			return '_'
		}
		return r
	}, base)
	base = strings.Trim(strings.TrimSpace(base), ".")
	if base == "" {
		base = "Recording " + time.Now().Format("2006-01-02 15-04-05")
	}
	dir := recordingsDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	p := filepath.Join(dir, base+ext)
	for i := 2; ; i++ {
		if _, err := os.Stat(p); os.IsNotExist(err) {
			break
		}
		p = filepath.Join(dir, fmt.Sprintf("%s (%d)%s", base, i, ext))
	}
	tmp := p + ".saving" + ext
	f, err := os.Create(tmp)
	if err != nil {
		return "", err
	}
	n, err := io.Copy(f, body)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err == nil && n == 0 {
		err = errors.New("the recording was empty")
	}
	if err != nil {
		os.Remove(tmp)
		return "", err
	}
	return p, os.Rename(tmp, p)
}
