package mame

// Recording a game played in MAME: MAME itself saves the game's picture and
// sound as it runs (-aviwrite), at the game's own size and on the game's own
// clock, so only the game is in it (no window, menus or mouse pointer).
// When the game ends, that AVI (uncompressed, so big) is turned into an .mp4
// with ffmpeg and deleted. RetroPixels.exe and RetroPixels Native can both
// use this.

import (
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"retropixels/internal/ffmpeg"
)

// AVIName is the file MAME is asked to write (in the recording's folder).
const AVIName = "game.avi"

// RecordArgs makes MAME save the game into dir (a folder for this game
// alone). It's nil when this MAME can't record (opts from OptionsOf; nil
// opts: it didn't say, and every Windows MAME for years has -aviwrite).
// MAME writes movies, and F12 screenshots, into its snapshot folder, so
// that folder is dir while the game plays (see KeepSnapshots).
func RecordArgs(dir string, opts Options) []string {
	if opts != nil && (!opts["aviwrite"] || !opts["snapshot_directory"]) {
		return nil
	}
	return []string{"-snapshot_directory", dir, "-aviwrite", AVIName}
}

// AVIs lists the movie files MAME wrote in dir, in order.
func AVIs(dir string) []string {
	var out []string
	filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() && strings.EqualFold(filepath.Ext(p), ".avi") {
			out = append(out, p)
		}
		return nil
	})
	sort.Strings(out)
	return out
}

// KeepSnapshots moves the screenshots taken while recording (F12) from dir
// to MAME's own snap folder next to exe, where they'd have gone anyway.
func KeepSnapshots(dir, exe string) {
	snap := filepath.Join(filepath.Dir(exe), "snap")
	filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() || !strings.EqualFold(filepath.Ext(p), ".png") {
			return nil
		}
		rel, rerr := filepath.Rel(dir, p)
		if rerr != nil {
			return nil
		}
		dst := filepath.Join(snap, rel)
		if _, serr := os.Stat(dst); serr == nil {
			ext := filepath.Ext(dst)
			dst = strings.TrimSuffix(dst, ext) + time.Now().Format(" 150405") + ext
		}
		os.MkdirAll(filepath.Dir(dst), 0o755)
		os.Rename(p, dst)
		return nil
	})
}

// ---------------- making the .mp4 ----------------

// Clip is one AVI as ffmpeg reads it.
type Clip struct {
	Path  string
	W, H  int
	Dur   float64 // seconds
	Sound bool
}

var (
	reVideo    = regexp.MustCompile(`Stream #[^\n]*Video: [^\n]*?\b(\d{2,5})x(\d{2,5})\b`)
	reAudio    = regexp.MustCompile(`Stream #[^\n]*Audio: `)
	reDuration = regexp.MustCompile(`Duration: (\d+):(\d\d):(\d\d(?:\.\d+)?)`)
	reOutTime  = regexp.MustCompile(`out_time_us=(\d+)`)
)

// ParseProbe reads what "ffmpeg -i file" says about a file.
func ParseProbe(path, out string) (Clip, error) {
	c := Clip{Path: path}
	if m := reVideo.FindStringSubmatch(out); m != nil {
		c.W, _ = strconv.Atoi(m[1])
		c.H, _ = strconv.Atoi(m[2])
	}
	if c.W == 0 || c.H == 0 {
		return c, errors.New(filepath.Base(path) + ": no picture in it")
	}
	if m := reDuration.FindStringSubmatch(out); m != nil {
		h, _ := strconv.Atoi(m[1])
		mi, _ := strconv.Atoi(m[2])
		s, _ := strconv.ParseFloat(m[3], 64)
		c.Dur = float64(h*3600+mi*60) + s
	}
	c.Sound = reAudio.MatchString(out)
	return c, nil
}

// OutputSize: the video's size for a game picture of w×h pixels. The
// picture is made bigger by a whole number (up to about 1080 lines, so the
// pixels stay sharp), and shown at the arcade screen's shape: 4:3, or 3:4
// for an upright screen. Odd shapes (several screens side by side, a
// handheld) keep their own.
func OutputSize(w, h int) (int, int) {
	if w <= 0 || h <= 0 {
		return 0, 0
	}
	k := int(math.Max(1, math.Floor(1080/float64(h))))
	oh := (h * k) &^ 1
	r := float64(w) / float64(h)
	switch {
	case r >= 1 && r <= 1.8:
		r = 4.0 / 3
	case r >= 0.55 && r < 1:
		r = 3.0 / 4
	}
	ow := int(math.Round(float64(oh)*r)) &^ 1
	return ow, oh
}

// CRF: the picture quality (x264's scale; lower is better, 18 looks the same as the game).
const CRF = 17

// ConvertArgs is ffmpeg's command line that turns the AVIs into one .mp4
// (H.264 picture, AAC sound), with its progress on stdout.
func ConvertArgs(clips []Clip, out string) []string {
	args := []string{"-hide_banner", "-nostats", "-loglevel", "error", "-progress", "pipe:1", "-y"}
	for _, c := range clips {
		args = append(args, "-i", c.Path)
	}
	if len(clips) == 0 {
		return args
	}
	ow, oh := OutputSize(clips[0].W, clips[0].H)
	scale := func(c Clip) string {
		k := oh / c.H
		if k < 1 {
			k = 1
		}
		// Whole-number growth keeps the pixels sharp; the last step only
		// changes the shape a little.
		return fmt.Sprintf("scale=%d:%d:flags=neighbor,scale=%d:%d:flags=lanczos,setsar=1", c.W*k, c.H*k, ow, oh)
	}
	sound := true
	for _, c := range clips {
		sound = sound && c.Sound
	}
	if len(clips) == 1 {
		args = append(args, "-map", "0:v:0", "-map", "0:a:0?", "-vf", scale(clips[0])+",format=yuv420p")
	} else {
		var g []string
		labels := ""
		for i, c := range clips {
			g = append(g, fmt.Sprintf("[%d:v:0]%s[v%d]", i, scale(c), i))
			labels += fmt.Sprintf("[v%d]", i)
			if sound {
				labels += fmt.Sprintf("[%d:a:0]", i)
			}
		}
		a := 0
		outs := "[cv]"
		if sound {
			a, outs = 1, "[cv][a]"
		}
		g = append(g, fmt.Sprintf("%sconcat=n=%d:v=1:a=%d%s", labels, len(clips), a, outs), "[cv]format=yuv420p[v]")
		args = append(args, "-filter_complex", strings.Join(g, ";"), "-map", "[v]")
		if sound {
			args = append(args, "-map", "[a]")
		}
	}
	args = append(args, "-c:v", "libx264", "-preset", "fast", "-crf", strconv.Itoa(CRF), "-pix_fmt", "yuv420p",
		"-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", out)
	return args
}

// MinSeconds: a recording shorter than this (the game closed straight away,
// or never showed a picture) isn't kept.
const MinSeconds = 5.0

// ErrTooShort: there was nothing worth keeping.
var ErrTooShort = errors.New("the game ended before there was anything to record")

// Convert makes out (an .mp4) from the AVIs MAME wrote in dir, using ffmpeg
// ff. The AVIs are deleted either way. progress (may be nil) gets how far
// along it is, 0–100.
func Convert(ff, dir, out string, progress func(int)) error {
	files := AVIs(dir)
	defer func() {
		for _, f := range files {
			os.Remove(f)
		}
	}()
	var clips []Clip
	total := 0.0
	for _, f := range files {
		cmd := ffmpeg.Hidden(ff, "-hide_banner", "-i", f)
		b, _ := cmd.CombinedOutput() // "ffmpeg -i" alone always "fails": no output file
		c, err := ParseProbe(f, string(b))
		if st, serr := os.Stat(f); err == nil && c.Dur == 0 && serr == nil {
			// MAME was closed before it finished the file: guess from its
			// size (uncompressed, 3 bytes a pixel, about 60 pictures a second).
			c.Dur = float64(st.Size()) / float64(c.W*c.H*3*60)
		}
		if err != nil || c.Dur < 0.1 {
			continue
		}
		clips = append(clips, c)
		total += c.Dur
	}
	if total < MinSeconds {
		return ErrTooShort
	}
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return errors.New("couldn't make the recordings folder: " + err.Error())
	}
	part := strings.TrimSuffix(out, filepath.Ext(out)) + ".saving.mp4"
	cmd := ffmpeg.LowPriority(ffmpeg.Hidden(ff, ConvertArgs(clips, part)...))
	errs := &Output{}
	cmd.Stderr = errs
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("couldn't start ffmpeg: %v", err)
	}
	buf := make([]byte, 4096)
	for {
		n, rerr := stdout.Read(buf)
		if m := reOutTime.FindAllSubmatch(buf[:n], -1); len(m) > 0 && progress != nil {
			us, _ := strconv.ParseInt(string(m[len(m)-1][1]), 10, 64)
			progress(int(math.Min(99, float64(us)/1e4/total)))
		}
		if rerr != nil {
			break
		}
	}
	err = cmd.Wait()
	if st, serr := os.Stat(part); err != nil || serr != nil || st.Size() == 0 {
		os.Remove(part)
		msg := strings.TrimSpace(errs.String())
		if len(msg) > 300 {
			msg = msg[len(msg)-300:]
		}
		if err == nil {
			err = errors.New("ffmpeg made an empty file")
		}
		return fmt.Errorf("ffmpeg couldn't make the video: %v %s", err, msg)
	}
	if err := os.Rename(part, out); err != nil {
		os.Remove(part)
		return err
	}
	return nil
}
