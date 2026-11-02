package dolphin

import (
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// Leftover is a recording PixelU-Wii saved that RetroPixels never made into a
// video: RetroPixels was closed, or didn't see the game end. Its files stay
// in the dump folder until RetroPixels finishes it (FindLeftovers).
type Leftover struct {
	GameID  string
	Started time.Time // when the game started (from the file names)
	Last    time.Time // when PixelU-Wii last wrote to it
	Files   Dumps
}

// Dolphin names its files "<game id>_<date>_<time>..." from when the game
// started: "_<n>.avi" for the picture (a new piece on a state load or a size
// change), "_dspdump.wav" / "_dtkdump.wav" for the sound ("..dump1.wav" when
// the sample rate changes).
var reDumpName = regexp.MustCompile(`^(.+)_(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})_`)

// leftoverGap: the sound starts a moment before the picture, so their names'
// times can differ by a few seconds.
const leftoverGap = 30 * time.Second

// FindLeftovers lists the recordings in dumpDir that nothing is writing to
// any more (no file changed for quiet), oldest first.
func FindLeftovers(dumpDir string, quiet time.Duration) []Leftover {
	type file struct {
		path, kind, id string
		start, mod     time.Time
	}
	var files []file
	for _, sub := range []string{"Frames", "Audio"} {
		entries, _ := os.ReadDir(filepath.Join(dumpDir, sub))
		for _, e := range entries {
			name := strings.ToLower(e.Name())
			kind := ""
			switch {
			case sub == "Frames" && strings.HasSuffix(name, ".avi"):
				kind = "video"
			case sub == "Audio" && strings.HasSuffix(name, ".wav") && strings.Contains(name, "dspdump"):
				kind = "dsp"
			case sub == "Audio" && strings.HasSuffix(name, ".wav") && strings.Contains(name, "dtkdump"):
				kind = "dtk"
			}
			m := reDumpName.FindStringSubmatch(e.Name())
			if e.IsDir() || kind == "" || m == nil {
				continue
			}
			start, err := time.ParseInLocation("2006-01-02_15-04-05", m[2], time.Local)
			st, serr := e.Info()
			if err != nil || serr != nil {
				continue
			}
			files = append(files, file{filepath.Join(dumpDir, sub, e.Name()), kind, m[1], start, st.ModTime()})
		}
	}
	sort.SliceStable(files, func(i, j int) bool {
		if !files[i].start.Equal(files[j].start) {
			return files[i].start.Before(files[j].start)
		}
		if !files[i].mod.Equal(files[j].mod) {
			return files[i].mod.Before(files[j].mod)
		}
		return files[i].path < files[j].path
	})
	// One game's files: the same game, started within a few seconds.
	var out []Leftover
	for _, f := range files {
		n := len(out)
		if n == 0 || !strings.EqualFold(out[n-1].GameID, f.id) || f.start.Sub(out[n-1].Started) > leftoverGap {
			out = append(out, Leftover{GameID: f.id, Started: f.start})
			n++
		}
		l := &out[n-1]
		switch f.kind {
		case "video":
			l.Files.Video = append(l.Files.Video, f.path)
		case "dsp":
			l.Files.DSP = append(l.Files.DSP, f.path)
		default:
			l.Files.DTK = append(l.Files.DTK, f.path)
		}
		if f.mod.After(l.Last) {
			l.Last = f.mod
		}
	}
	var done []Leftover
	for _, l := range out {
		switch {
		case time.Since(l.Last) < quiet:
			continue // still being written: a game is playing
		case len(l.Files.Video) == 0:
			l.Files.Remove() // sound without a picture can't be made into a video
		default:
			done = append(done, l)
		}
	}
	return done
}

// LeftoverRecording makes the video for a leftover recording, the whole of
// it, with Save (which deletes its files once the video is made).
func LeftoverRecording(l Leftover, game, log string) *Recording {
	files := l.Files
	r := &Recording{Game: game, Launched: l.Started, Ended: l.Last, DumpDir: "", Log: log, files: &files,
		clips:      []clip{{From: mark{Audio: 0, Wall: l.Started, Edge: true}, To: mark{Audio: -1, Wall: l.Last, Edge: true}}},
		firstVideo: l.Started}
	r.logf("\n=== %s  %s: finishing a recording RetroPixels didn't get to save (it was closed, or missed the game's end)",
		time.Now().Format(time.RFC3339), game)
	return r
}
