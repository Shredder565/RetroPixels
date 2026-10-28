package rpcs3

// Recording a game RPCS3 plays. RPCS3 saves the game's picture itself
// (its "Start/Stop Recording" key, F11, which RetroPixels presses for the
// player) as an .mp4 in RPCS3\recordings, without sound, and its sound as a
// .wav in RPCS3\cache when "Dump to file" is on. RetroPixels joins the two
// into one video in its recordings folder when the game ends.

import (
	"encoding/binary"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// recordingSettings: RPCS3's own recording at a smooth 60 pictures a second
// and a high bitrate, since RetroPixels encodes the final video again.
var recordingSettings = [][2]string{
	{"Framerate", "60"},
	{"Width", "1280"},
	{"Height", "720"},
	{"AVPixelFormat", "0"},
	{"AVCodecID", "12"}, // MPEG-4, which RPCS3 can always encode
	{"Video Bitrate", "25000000"},
	{"Max B-Frames", "0"},
	{"Group of Pictures Size", "30"},
}

// SetRecording turns RPCS3's sound dump on or off for the next game in its
// folder dir, and when on, sets up its picture recording.
func SetRecording(dir string, on bool) error {
	cfg := ConfigFile(dir)
	b, err := os.ReadFile(cfg)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	v := "false"
	if on {
		v = "true"
	}
	if err := os.WriteFile(cfg, SetConfig(b, "Audio", "Dump to file", v), 0o644); err != nil {
		return err
	}
	if !on {
		return nil
	}
	rc := filepath.Join(dir, "config", "recording.yml")
	b, err = os.ReadFile(rc)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	for _, kv := range recordingSettings {
		b = setTop(b, kv[0], kv[1])
	}
	if err := os.MkdirAll(filepath.Dir(rc), 0o755); err != nil {
		return err
	}
	return os.WriteFile(rc, b, 0o644)
}

// setTop sets a top-level "key: value" line of a YAML file's text.
func setTop(b []byte, key, value string) []byte {
	text := strings.TrimSuffix(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n")
	var lines []string
	if text != "" {
		lines = strings.Split(text, "\n")
	}
	entry := key + ": " + value
	for i, l := range lines {
		if strings.HasPrefix(l, key+":") {
			lines[i] = entry
			return []byte(strings.Join(lines, "\n") + "\n")
		}
	}
	return []byte(strings.Join(append(lines, entry), "\n") + "\n")
}

// Recording is what RPCS3 saved while a game was recorded: the picture
// files and the sound files, each list in order, and when each began.
type Recording struct {
	Videos     []string
	VideoStart time.Time
	Sounds     []string
	SoundStart time.Time
}

// FindRecording finds the files RPCS3 saved in its folder dir for a game
// started at since.
func FindRecording(dir string, since time.Time) Recording {
	var rec Recording
	since = since.Add(-2 * time.Second)
	find := func(root string, keep func(name string) bool) []string {
		type f struct {
			path string
			born time.Time
		}
		var found []f
		filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() || !keep(strings.ToLower(d.Name())) {
				return nil
			}
			if info, err := d.Info(); err == nil && info.ModTime().After(since) {
				if b := born(p, info); !b.Before(since) {
					found = append(found, f{p, b})
				}
			}
			return nil
		})
		sort.Slice(found, func(i, j int) bool { return found[i].born.Before(found[j].born) })
		out := make([]string, len(found))
		for i, x := range found {
			out[i] = x.path
		}
		return out
	}
	rec.Videos = find(filepath.Join(dir, "recordings"), func(n string) bool { return strings.HasSuffix(n, ".mp4") })
	rec.Sounds = find(filepath.Join(dir, "cache"), func(n string) bool {
		return strings.HasPrefix(n, "audio_") && strings.HasSuffix(n, ".wav")
	})
	if len(rec.Videos) > 0 {
		rec.VideoStart = bornOf(rec.Videos[0])
	}
	if len(rec.Sounds) > 0 {
		rec.SoundStart = bornOf(rec.Sounds[0])
	}
	return rec
}

func bornOf(p string) time.Time {
	info, err := os.Stat(p)
	if err != nil {
		return time.Time{}
	}
	return born(p, info)
}

// wavHeaderSize is the header RPCS3 writes before its sound (RIFF, fmt,
// fact and data).
const wavHeaderSize = 56

// RepairWav writes the header of a sound file RPCS3 couldn't finish (it
// only writes it when the file is closed): 32-bit float stereo at 48 kHz,
// what RPCS3 saves for stereo sound.
func RepairWav(path string) error {
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	var head [4]byte
	if _, err := f.ReadAt(head[:], 0); err != nil {
		return err
	}
	if string(head[:]) == "RIFF" {
		return nil
	}
	st, err := f.Stat()
	if err != nil {
		return err
	}
	data := st.Size() - wavHeaderSize
	if data < 0 {
		data = 0
	}
	const ch, rate, bytesPer = 2, 48000, 4
	h := make([]byte, 0, wavHeaderSize)
	le32 := func(v uint32) { h = binary.LittleEndian.AppendUint32(h, v) }
	le16 := func(v uint16) { h = binary.LittleEndian.AppendUint16(h, v) }
	h = append(h, "RIFF"...)
	le32(uint32(st.Size() - 8))
	h = append(h, "WAVEfmt "...)
	le32(16)
	le16(3) // IEEE float
	le16(ch)
	le32(rate)
	le32(rate * ch * bytesPer)
	le16(ch * bytesPer)
	le16(bytesPer * 8)
	h = append(h, "fact"...)
	le32(4)
	le32(uint32(data / (ch * bytesPer)))
	h = append(h, "data"...)
	le32(uint32(data))
	_, err = f.WriteAt(h, 0)
	return err
}

// Remove deletes what RPCS3 saved for a recording once the video is made.
func (r Recording) Remove() {
	for _, p := range append(append([]string{}, r.Videos...), r.Sounds...) {
		os.Remove(p)
	}
}
