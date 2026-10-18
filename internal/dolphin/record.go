package dolphin

// Recording, as RetroPixels.exe does it for Dolphin games.
//
// Dolphin itself saves the game's picture and sound while the game runs (its
// "frame dump" and "audio dump"). That's only the game - no mouse pointer, no
// other windows, no black bars - and it follows the game's own clock, so the
// video is smooth however busy the PC is. When the game ends, those raw files
// are turned into one .mp4 per recorded stretch with ffmpeg and deleted.

import (
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"retropixels/internal/ffmpeg"
)

// ---------------- Dolphin's dump files ----------------

// Dumps are the files Dolphin wrote for one game.
type Dumps struct {
	Video []string // Dump\Frames\*.avi, in order (a new file starts when the picture size changes)
	DSP   []string // Dump\Audio\*dspdump*.wav: the game's sound
	DTK   []string // Dump\Audio\*dtkdump*.wav: music streamed from the disc (GameCube)
}

// All lists every file.
func (d Dumps) All() []string {
	return append(append(append([]string{}, d.Video...), d.DSP...), d.DTK...)
}

// Remove deletes them (they're huge).
func (d Dumps) Remove() {
	for _, f := range d.All() {
		os.Remove(f)
	}
}

// FindDumps lists the dump files Dolphin wrote in dir since a game started.
func FindDumps(dir string, since time.Time) Dumps {
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
	return Dumps{
		Video: pick("Frames", func(n string) bool { return strings.HasSuffix(n, ".avi") }),
		DSP:   pick("Audio", func(n string) bool { return strings.HasSuffix(n, ".wav") && strings.Contains(n, "dspdump") }),
		DTK:   pick("Audio", func(n string) bool { return strings.HasSuffix(n, ".wav") && strings.Contains(n, "dtkdump") }),
	}
}

// WavSeconds: how much sound a .wav holds, worked out from its real size
// (the header's own length is only filled in when Dolphin closes the file).
func WavSeconds(path string) float64 {
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
		t += WavSeconds(f)
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

// Recording is one game being saved by Dolphin.
type Recording struct {
	Game     string
	Launched time.Time
	Ended    time.Time
	DumpDir  string
	Log      string // the log file ("" = none)

	mu         sync.Mutex
	clips      []clip
	firstVideo time.Time // when Dolphin's first video file appeared
	warned     bool
}

// NewRecording: a game about to start with Dolphin saving it, recorded from
// its start.
func NewRecording(game, dumpDir, log string) *Recording {
	now := time.Now()
	return &Recording{Game: game, Launched: now, DumpDir: dumpDir, Log: log,
		clips: []clip{{From: mark{Audio: 0, Wall: now, Edge: true}, Open: true}}}
}

// markNow: where the game is up to right now.
func (r *Recording) markNow() mark {
	m := mark{Audio: -1, Wall: time.Now()}
	d := FindDumps(r.DumpDir, r.Launched)
	// Same choice of sound as when the video is made. If it can't be read
	// right now, the PC's clock is used instead.
	if a := wavTotal(d.DSP); a > 0 {
		m.Audio = a
	} else if a := wavTotal(d.DTK); a > 0 && len(d.DSP) == 0 {
		m.Audio = a
	}
	return m
}

// Stop ends the current stretch. Dolphin keeps saving the game until it ends
// (that can't be switched off from outside), so the rest is cut off when the
// video is made.
func (r *Recording) Stop() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	n := len(r.clips)
	if n == 0 || !r.clips[n-1].Open {
		return false
	}
	r.clips[n-1].To = r.markNow()
	r.clips[n-1].Open = false
	return true
}

// Resume starts a new stretch from now; it becomes a separate video.
func (r *Recording) Resume() {
	r.mu.Lock()
	defer r.mu.Unlock()
	if n := len(r.clips); n > 0 && r.clips[n-1].Open {
		return
	}
	r.clips = append(r.clips, clip{From: r.markNow(), Open: true})
}

// Active: whether a stretch is being recorded, and since when.
func (r *Recording) Active() (bool, time.Time) {
	r.mu.Lock()
	defer r.mu.Unlock()
	n := len(r.clips)
	if n == 0 || !r.clips[n-1].Open {
		return false, time.Time{}
	}
	c := r.clips[n-1]
	if c.From.Edge {
		if !r.firstVideo.IsZero() {
			return true, r.firstVideo
		}
		return true, r.Launched
	}
	return true, c.From.Wall
}

func (r *Recording) logf(f string, a ...any) {
	if r.Log == "" {
		return
	}
	os.MkdirAll(filepath.Dir(r.Log), 0o755)
	if lf, err := os.OpenFile(r.Log, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
		fmt.Fprintf(lf, f+"\n", a...)
		lf.Close()
	}
}

// Start begins the log as the game starts (args: Dolphin's command line).
func (r *Recording) Start(args []string) {
	if r.Log == "" {
		return
	}
	os.MkdirAll(filepath.Dir(r.Log), 0o755)
	os.WriteFile(r.Log, []byte(fmt.Sprintf("=== %s  %s\nDolphin saves the game to %s\nDolphin %s\n",
		r.Launched.Format(time.RFC3339), r.Game, r.DumpDir, strings.Join(args, " "))), 0o644)
}

// NoVideoAfter: how long Follow waits for Dolphin's first picture before warning.
var NoVideoAfter = 45 * time.Second

// Follow watches the game while it runs and returns when done closes (the
// game ended). warn gets a message for the player when Dolphin isn't saving
// the picture, and "" if it starts after all.
func (r *Recording) Follow(done <-chan struct{}, warn func(string)) {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		select {
		case <-done:
			r.mu.Lock()
			r.Ended = time.Now()
			if n := len(r.clips); n > 0 && r.clips[n-1].Open {
				r.clips[n-1].To = mark{Audio: -1, Wall: r.Ended, Edge: true}
				r.clips[n-1].Open = false
			}
			r.mu.Unlock()
			return
		case <-tick.C:
		}
		r.mu.Lock()
		waiting := r.firstVideo.IsZero()
		r.mu.Unlock()
		if !waiting {
			continue
		}
		if d := FindDumps(r.DumpDir, r.Launched); len(d.Video) > 0 {
			r.mu.Lock()
			r.firstVideo = time.Now()
			warned := r.warned
			r.mu.Unlock()
			r.logf("Dolphin started saving the picture: %s", filepath.Base(d.Video[0]))
			if warned {
				warn("")
			}
		} else if time.Since(r.Launched) > NoVideoAfter {
			r.mu.Lock()
			first := !r.warned
			r.warned = true
			r.mu.Unlock()
			if first {
				r.logf("No video from Dolphin yet in %s", filepath.Join(r.DumpDir, "Frames"))
				warn("PixelU hasn't started saving the game's picture, so this game may not be recorded. " +
					"An up-to-date PixelU folder (with wii\\PixelU-Wii.exe) is needed. Details are in " + r.Log)
			}
		}
	}
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

type crop struct{ X, Y, W, H int }

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
	args = append(args, ffmpeg.EncoderArgs(enc, quality)...)
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
	cmd := ffmpeg.LowPriority(ffmpeg.Hidden(ff, "-hide_banner", "-nostats", "-progress", "pipe:1", "-i", path,
		"-map", "0:v:0", "-c", "copy", "-f", "null", "-"))
	out, err := cmd.Output()
	if m := reOutUS.FindAllStringSubmatch(string(out), -1); len(m) > 0 {
		us, _ := strconv.ParseInt(m[len(m)-1][1], 10, 64)
		v.Dur = float64(us) / 1e6
	}
	// Size, frame rate and bars (looking at key frames from the first 15 minutes).
	cmd = ffmpeg.LowPriority(ffmpeg.Hidden(ff, "-hide_banner", "-skip_frame", "nokey", "-t", "900", "-i", path,
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

var (
	encMu    sync.Mutex
	encFound = map[string]bool{} // ffmpeg|encoder -> works
)

// encoderWorks test-encodes a few frames to see if this PC supports an encoder.
func encoderWorks(ff, enc string) bool {
	encMu.Lock()
	defer encMu.Unlock()
	if v, ok := encFound[ff+"|"+enc]; ok {
		return v
	}
	args := append([]string{"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=black:s=640x360:r=30:d=0.3"},
		ffmpeg.EncoderArgs(enc, "balanced")...)
	args = append(args, "-pix_fmt", "yuv420p", "-f", "null", "-")
	err := ffmpeg.RunTimeout(ffmpeg.Hidden(ff, args...), 15*time.Second)
	encFound[ff+"|"+enc] = err == nil
	return err == nil
}

// encodeClip makes one .mp4, trying the PC's video encoders in order
// (NVIDIA, AMD, Intel, then software).
func encodeClip(ff string, j finalJob, quality string, logf func(string, ...any), progress func(sec float64)) error {
	lastErr := errors.New("no video encoder worked on this PC")
	for _, enc := range ffmpeg.Encoders {
		if !encoderWorks(ff, enc) {
			logf("encoder %s not available", enc)
			continue
		}
		args := finalArgs(j, enc, quality)
		logf("ffmpeg %s", strings.Join(args, " "))
		cmd := ffmpeg.LowPriority(ffmpeg.Hidden(ff, args...))
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

var reUnsafe = regexp.MustCompile(`[<>:"/\\|?*\x00-\x1f™®]+`)

// safeFile makes a game's name usable as a file name (as RetroPixels.exe names recordings).
func safeFile(s string) string {
	s = reUnsafe.ReplaceAllString(s, "")
	s = strings.TrimSpace(strings.TrimRight(s, ". "))
	if s == "" {
		return "Game"
	}
	if len(s) > 80 {
		s = s[:80]
	}
	return s
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

// SaveOptions: where and how the video is made.
type SaveOptions struct {
	Dir     string        // the recordings folder
	Quality string        // "high", "balanced" or "small"
	FPS     int           // 60, or 30
	FFmpeg  func() string // ffmpeg's path; may wait for a download ("" = there's none)
	// Progress gets "ffmpeg" while waiting for ffmpeg, then "saving" with
	// how far along it is.
	Progress func(step string, percent int)
}

// Save makes the .mp4 (one per recorded stretch) from Dolphin's files once
// the game has ended, deletes those files, and returns the videos' names.
// The error is a message for the player.
func (r *Recording) Save(o SaveOptions) ([]string, error) {
	progress := func(step string, pc int) {
		if o.Progress != nil {
			o.Progress(step, pc)
		}
	}
	d := FindDumps(r.DumpDir, r.Launched)
	r.logf("\n=== game ended after %s\nvideo: %v\nsound: %v %v", r.Ended.Sub(r.Launched).Round(time.Second), d.Video, d.DSP, d.DTK)
	if len(d.Video) == 0 {
		d.Remove()
		if r.Ended.Sub(r.Launched) > 20*time.Second {
			return nil, errors.New("PixelU didn't save the game's picture, so nothing was recorded. " +
				"An up-to-date PixelU folder (with wii\\PixelU-Wii.exe) is needed. Details are in " + r.Log)
		}
		return nil, nil
	}
	defer d.Remove() // the raw files are huge

	r.mu.Lock()
	clips := append([]clip{}, r.clips...)
	firstVideo := r.firstVideo
	r.mu.Unlock()
	if firstVideo.IsZero() {
		firstVideo = r.Launched
	}

	progress("ffmpeg", 0)
	ff := o.FFmpeg()
	if ff == "" {
		return nil, errors.New("the recording couldn't be saved because ffmpeg isn't there to make the video")
	}
	progress("saving", 0)

	var segs []vidSeg
	videoLen := 0.0
	for _, f := range d.Video {
		v, err := probeSegment(ff, f)
		if err != nil || v.Dur < 0.05 {
			r.logf("skipping %s: %v (%.2fs)", filepath.Base(f), err, v.Dur)
			continue
		}
		r.logf("%s: %.2fs %dx%d %.3f fps bars=%v", filepath.Base(f), v.Dur, v.W, v.H, v.FPS, v.Crop)
		segs = append(segs, v)
		videoLen += v.Dur
	}
	if len(segs) == 0 {
		return nil, errors.New("PixelU's video for this game couldn't be read, so it wasn't saved. Details are in " + r.Log)
	}
	j := finalJob{Video: segs, DSP: d.DSP, DTK: d.DTK, DSPLen: wavTotal(d.DSP), DTKLen: wavTotal(d.DTK),
		FPS: outputFPS(segs[0].FPS, o.FPS)}
	audioLen := j.DSPLen
	if audioLen <= 0 {
		audioLen = j.DTKLen
	}
	r.logf("video %.2fs, sound %.2fs (disc music %.2fs)", videoLen, j.DSPLen, j.DTKLen)

	if err := os.MkdirAll(o.Dir, 0o755); err != nil {
		return nil, errors.New("couldn't make the recordings folder: " + err.Error())
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
			r.logf("skipping a clip under a second long (%.2f-%.2f)", from, to)
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
	var failErr error
	done := 0.0
	for _, p := range parts {
		out := uniqueOut(o.Dir, r.Game, p.wall)
		j.From, j.To = p.from, p.to
		j.Out = strings.TrimSuffix(out, ".mp4") + ".saving.mp4"
		r.logf("making %s from %.2fs to %.2fs", filepath.Base(out), p.from, p.to)
		err := encodeClip(ff, j, o.Quality, r.logf, func(sec float64) {
			progress("saving", int(100*(done+math.Min(sec, p.to-p.from))/total))
		})
		done += p.to - p.from
		if err == errDiskFull {
			failErr = errors.New("your disk is full, so the recording couldn't be saved. Free up some space and try again")
			break
		}
		if err != nil {
			failErr = errors.New("the recording couldn't be saved. Details are in " + r.Log)
			r.logf("failed: %v", err)
			continue
		}
		if err := os.Rename(j.Out, out); err != nil {
			os.Remove(j.Out)
			failErr = errors.New("the recording couldn't be saved: " + err.Error())
			continue
		}
		saved = append(saved, filepath.Base(out))
		r.logf("saved %s", out)
	}
	return saved, failErr
}
