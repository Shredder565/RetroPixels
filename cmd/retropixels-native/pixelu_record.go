package main

// Recording Wii U games, as RetroPixels.exe does: PixelU is started with
// --rp-record (internal/pixelu) and saves just the game's TV picture as
// .mkv files in <dir>\Frames and its sound as a .wav in <dir>\Audio while
// the game runs. When the game ends, those become one .mp4 in the
// recordings folder and the folder is deleted. pixelu_windows.go starts it.

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

	"retropixels/internal/dolphin"
	"retropixels/internal/ffmpeg"
	"retropixels/internal/mame"
)

// pixeluRecDir makes a folder of its own for one game's raw files, under base.
func pixeluRecDir(base string) (string, error) { return engineRecDir(base, "PixelU-") }

// engineRecDir makes a folder of its own (named prefix…) for one game's raw
// files, under base, with the Frames and Audio folders that PixelU and
// RetroPixels' Supermodel save the picture and sound into.
func engineRecDir(base, prefix string) (string, error) {
	if err := os.MkdirAll(base, 0o755); err != nil {
		return "", errors.New("couldn't make the recordings folder: " + err.Error())
	}
	dir, err := os.MkdirTemp(base, prefix)
	if err != nil {
		return "", errors.New("couldn't make the recordings folder: " + err.Error())
	}
	os.MkdirAll(filepath.Join(dir, "Frames"), 0o755)
	os.MkdirAll(filepath.Join(dir, "Audio"), 0o755)
	return dir, nil
}

// pixeluDumps: PixelU's picture files (a new one starts when the picture
// size changes) and sound files, in order.
func pixeluDumps(dir string) (video, sound []string) {
	list := func(sub, ext string) []string {
		var out []string
		entries, _ := os.ReadDir(filepath.Join(dir, sub))
		for _, e := range entries {
			if !e.IsDir() && strings.EqualFold(filepath.Ext(e.Name()), ext) {
				out = append(out, filepath.Join(dir, sub, e.Name()))
			}
		}
		// pixelu_framedump0.mkv, ..., pixelu_framedump10.mkv: by number
		num := regexp.MustCompile(`(\d+)\.[^.]+$`)
		n := func(p string) int {
			if m := num.FindStringSubmatch(p); m != nil {
				v, _ := strconv.Atoi(m[1])
				return v
			}
			return -1
		}
		sort.SliceStable(out, func(i, j int) bool { return n(out[i]) < n(out[j]) })
		return out
	}
	return list("Frames", ".mkv"), list("Audio", ".wav")
}

// pixeluSaveArgs is ffmpeg's command line that makes the .mp4: the picture
// parts joined at the first part's size, and the sound lined up with the
// picture by their lengths (PixelU's sound starts a moment before its
// first picture; both end when the game closes).
func pixeluSaveArgs(video []mame.Clip, sound []string, videoLen, soundLen float64, enc, out string) []string {
	args := []string{"-hide_banner", "-nostats", "-loglevel", "error", "-progress", "pipe:1", "-y"}
	for _, v := range video {
		args = append(args, "-i", v.Path)
	}
	for _, s := range sound {
		args = append(args, "-ignore_length", "1", "-i", s) // the header may not have its final length
	}
	W, H := video[0].W&^1, video[0].H&^1
	var g []string
	labels := ""
	for i, v := range video {
		f := ""
		if v.W != W || v.H != H {
			f = fmt.Sprintf("scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,", W, H, W, H)
		}
		g = append(g, fmt.Sprintf("[%d:v:0]%ssetsar=1[v%d]", i, f, i))
		labels += fmt.Sprintf("[v%d]", i)
	}
	pix := "yuv420p"
	if enc == "h264_qsv" {
		pix = "nv12"
	}
	if len(video) > 1 {
		labels += fmt.Sprintf("concat=n=%d:v=1:a=0,", len(video))
	}
	g = append(g, labels+"format="+pix+"[v]")
	withSound := len(sound) > 0 && soundLen > 0
	if withSound {
		ls := ""
		for k := range sound {
			ls += fmt.Sprintf("[%d:a:0]", len(video)+k)
		}
		var f []string
		if len(sound) > 1 {
			f = append(f, fmt.Sprintf("concat=n=%d:v=0:a=1", len(sound)))
		}
		f = append(f, "aresample=48000", "aformat=sample_fmts=fltp:channel_layouts=stereo")
		if off := soundLen - videoLen; off > 0.01 {
			f = append(f, "atrim=start="+ffSec(off), "asetpts=PTS-STARTPTS")
		} else if off < -0.01 {
			f = append(f, fmt.Sprintf("adelay=%d:all=1", int(math.Round(-off*1000))))
		}
		f = append(f, "apad=whole_dur="+ffSec(videoLen), "atrim=end="+ffSec(videoLen))
		g = append(g, ls+strings.Join(f, ",")+"[a]")
	}
	args = append(args, "-filter_complex", strings.Join(g, ";"), "-map", "[v]")
	if withSound {
		args = append(args, "-map", "[a]")
	}
	args = append(args, ffmpeg.EncoderArgs(enc, "high")...)
	if withSound {
		args = append(args, "-c:a", "aac", "-b:a", "192k")
	}
	return append(args, "-movflags", "+faststart", out)
}

func ffSec(v float64) string { return strconv.FormatFloat(v, 'f', 3, 64) }

var reOutUS = regexp.MustCompile(`out_time_us=(\d+)`)

// probeClip: one of PixelU's picture files, and how long it really is
// (read to the end, so it works even if PixelU couldn't finish the file).
func probeClip(ff, path string) (mame.Clip, error) {
	b, _ := ffmpeg.Hidden(ff, "-hide_banner", "-i", path).CombinedOutput()
	c, err := mame.ParseProbe(path, string(b))
	if err != nil {
		return c, err
	}
	out, _ := ffmpeg.LowPriority(ffmpeg.Hidden(ff, "-hide_banner", "-nostats", "-progress", "pipe:1", "-i", path,
		"-map", "0:v:0", "-c", "copy", "-f", "null", "-")).Output()
	if m := reOutUS.FindAllStringSubmatch(string(out), -1); len(m) > 0 {
		us, _ := strconv.ParseInt(m[len(m)-1][1], 10, 64)
		c.Dur = float64(us) / 1e6
	}
	return c, nil
}

// minRecording: an automatic recording shorter than this (seconds) isn't kept.
const minRecording = 5.0

// errNoPicture: PixelU ran a while but saved no picture.
var errNoPicture = errors.New("PixelU didn't save the game's picture, so nothing was recorded. " +
	"A PixelU build that can record (made with RetroPixels' recording support) is needed")

// pixeluSave makes out from PixelU's files in dir with ffmpeg ff (encoder
// enc first, then the CPU's if that fails). The files are left for the
// caller to delete. errRecTooShort: nothing worth keeping.
func pixeluSave(ff, enc, dir, out string, ranFor float64, progress func(int)) error {
	video, sound := pixeluDumps(dir)
	var clips []mame.Clip
	videoLen := 0.0
	for _, f := range video {
		c, err := probeClip(ff, f)
		if err != nil || c.Dur < 0.05 {
			continue
		}
		clips = append(clips, c)
		videoLen += c.Dur
	}
	if len(clips) == 0 && ranFor > 30 {
		return errNoPicture
	}
	if videoLen < minRecording {
		return errRecTooShort
	}
	soundLen := 0.0
	for _, s := range sound {
		soundLen += dolphin.WavSeconds(s)
	}
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return errors.New("couldn't make the recordings folder: " + err.Error())
	}
	part := strings.TrimSuffix(out, ".mp4") + ".saving.mp4"
	encs := []string{enc}
	if enc != "libx264" {
		encs = append(encs, "libx264")
	}
	var lastErr error
	for _, e := range encs {
		cmd := ffmpeg.LowPriority(ffmpeg.Hidden(ff, pixeluSaveArgs(clips, sound, videoLen, soundLen, e, part)...))
		log := &tailBuf{}
		cmd.Stderr = log
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
			if m := reOutUS.FindAllSubmatch(buf[:n], -1); len(m) > 0 && progress != nil {
				us, _ := strconv.ParseInt(string(m[len(m)-1][1]), 10, 64)
				progress(int(math.Min(99, float64(us)/1e4/videoLen)))
			}
			if rerr != nil {
				break
			}
		}
		err = cmd.Wait()
		if st, serr := os.Stat(part); err == nil && serr == nil && st.Size() > 0 {
			return os.Rename(part, out)
		}
		os.Remove(part)
		if err == nil {
			err = errors.New("ffmpeg made an empty file")
		}
		lastErr = fmt.Errorf("ffmpeg couldn't make the video (%s): %v %s", e, err, log.String())
	}
	return lastErr
}
