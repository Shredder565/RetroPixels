package dolphin

import (
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

// writeWav writes a 16-bit stereo .wav holding secs of silence. With
// unfinished=true the header's lengths are left at 0, as when Dolphin is
// still writing it (or didn't get to close it).
func writeWav(t *testing.T, path string, rate int, secs float64, unfinished bool) {
	n := int(float64(rate)*secs) * 4
	h := make([]byte, 44)
	copy(h[0:], "RIFF")
	copy(h[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(h[16:], 16)
	binary.LittleEndian.PutUint16(h[20:], 1)
	binary.LittleEndian.PutUint16(h[22:], 2)
	binary.LittleEndian.PutUint32(h[24:], uint32(rate))
	binary.LittleEndian.PutUint32(h[28:], uint32(rate*4))
	binary.LittleEndian.PutUint16(h[32:], 4)
	binary.LittleEndian.PutUint16(h[34:], 16)
	copy(h[36:], "data")
	if !unfinished {
		binary.LittleEndian.PutUint32(h[4:], uint32(36+n))
		binary.LittleEndian.PutUint32(h[40:], uint32(n))
	}
	os.MkdirAll(filepath.Dir(path), 0o755)
	if err := os.WriteFile(path, append(h, make([]byte, n)...), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestWavSeconds(t *testing.T) {
	dir := t.TempDir()
	writeWav(t, filepath.Join(dir, "a.wav"), 32000, 2.5, false)
	writeWav(t, filepath.Join(dir, "b.wav"), 48000, 1.25, true)
	os.WriteFile(filepath.Join(dir, "c.wav"), []byte("not a wav"), 0o644)
	for name, want := range map[string]float64{"a.wav": 2.5, "b.wav": 1.25, "c.wav": 0, "missing.wav": 0} {
		if got := WavSeconds(filepath.Join(dir, name)); got < want-0.001 || got > want+0.001 {
			t.Errorf("%s: %.3f, want %.3f", name, got, want)
		}
	}
}

func TestFindDumps(t *testing.T) {
	dir := t.TempDir()
	old := filepath.Join(dir, "Frames", "GMSE01_2020-01-01_10-00-00_0.avi")
	os.MkdirAll(filepath.Dir(old), 0o755)
	os.WriteFile(old, []byte("x"), 0o644)
	os.Chtimes(old, time.Now().Add(-time.Hour), time.Now().Add(-time.Hour))
	since := time.Now()
	touch := func(rel string, age time.Duration) {
		p := filepath.Join(dir, rel)
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte("x"), 0o644)
		os.Chtimes(p, since.Add(age), since.Add(age))
	}
	touch("Frames/GMSE01_2026-10-14_12-00-00_1.avi", 20*time.Second)
	touch("Frames/GMSE01_2026-10-14_12-00-00_0.avi", 10*time.Second)
	touch("Frames/GMSE01_2026-10-14_12-00-00_0.png", 10*time.Second)
	touch("Audio/GMSE01_2026-10-14_12-00-00_dspdump.wav", 10*time.Second)
	touch("Audio/GMSE01_2026-10-14_12-00-00_dspdump1.wav", 30*time.Second)
	touch("Audio/dtkdump.wav", 30*time.Second)
	d := FindDumps(dir, since)
	base := func(l []string) string {
		var b []string
		for _, f := range l {
			b = append(b, filepath.Base(f))
		}
		return strings.Join(b, ",")
	}
	if got := base(d.Video); got != "GMSE01_2026-10-14_12-00-00_0.avi,GMSE01_2026-10-14_12-00-00_1.avi" {
		t.Errorf("video: %s", got)
	}
	if got := base(d.DSP); got != "GMSE01_2026-10-14_12-00-00_dspdump.wav,GMSE01_2026-10-14_12-00-00_dspdump1.wav" {
		t.Errorf("dsp: %s", got)
	}
	if got := base(d.DTK); got != "dtkdump.wav" {
		t.Errorf("dtk: %s", got)
	}
	d.Remove()
	if _, err := os.Stat(old); err != nil {
		t.Error("deleted a dump from an earlier game")
	}
	if len(FindDumps(dir, since).All()) != 0 {
		t.Error("dumps not deleted")
	}
}

// Stop and Record mid-game mark where the game is by how much sound Dolphin
// has saved; the video keeps only those parts.
func TestStopTrim(t *testing.T) {
	dir := t.TempDir()
	r := NewRecording("Wind Waker", dir, "")
	r.Launched = time.Now().Add(-time.Minute)
	dsp := filepath.Join(dir, "Audio", "GZLE01_dspdump.wav")
	writeWav(t, dsp, 32000, 12.5, true)
	if on, _ := r.Active(); !on {
		t.Fatal("not recording from the start")
	}
	if !r.Stop() {
		t.Fatal("stop did nothing")
	}
	if r.Stop() {
		t.Fatal("stopped twice")
	}
	if on, _ := r.Active(); on {
		t.Fatal("still recording after stop")
	}
	writeWav(t, dsp, 32000, 20, true)
	r.Resume()
	if on, since := r.Active(); !on || time.Since(since) > time.Second {
		t.Fatalf("resume: %v %v", on, since)
	}
	r.clips[1].Open, r.clips[1].To = false, mark{Audio: -1, Edge: true}

	if c := r.clips[0]; c.To.Audio < 12.49 || c.To.Audio > 12.51 {
		t.Fatalf("stop mark = %+v", c.To)
	}
	// Video 30s, sound 30.5s: the sound started half a second earlier.
	first := time.Now()
	from, to := clipRange(r.clips[0], 30, 30.5, first)
	if from != 0 || to < 11.99 || to > 12.01 {
		t.Errorf("first clip %.2f-%.2f, want 0-12", from, to)
	}
	from, to = clipRange(r.clips[1], 30, 30.5, first)
	if from < 19.49 || from > 19.51 || to != 30 {
		t.Errorf("second clip %.2f-%.2f, want 19.5-30", from, to)
	}
	// No sound saved: the PC's clock is used instead.
	c := clip{From: mark{Audio: -1, Wall: first.Add(5 * time.Second)}, To: mark{Audio: -1, Wall: first.Add(9 * time.Second)}}
	if from, to := clipRange(c, 30, 0, first); from != 5 || to != 9 {
		t.Errorf("clock-based clip %.2f-%.2f", from, to)
	}
	// Never past the ends of the video.
	c = clip{From: mark{Audio: 0.1}, To: mark{Audio: 99}}
	if from, to := clipRange(c, 30, 30.5, first); from != 0 || to != 30 {
		t.Errorf("clamped clip %.2f-%.2f", from, to)
	}
}

func TestFinalArgs(t *testing.T) {
	j := finalJob{
		Video: []vidSeg{{Path: "a.avi", Dur: 10, W: 960, H: 720}, {Path: "b.avi", Dur: 20, W: 1440, H: 1080, Crop: &crop{0, 60, 1440, 960}}},
		DSP:   []string{"dsp.wav"}, DSPLen: 30.5,
		DTK: []string{"dtk.wav"}, DTKLen: 30.5,
		From: 2, To: 12, FPS: 59.94, Out: "out.mp4",
	}
	args := strings.Join(finalArgs(j, "libx264", "balanced"), " ")
	for _, want := range []string{
		"-i a.avi -i b.avi -ignore_length 1 -i dsp.wav -ignore_length 1 -i dtk.wav",
		"[0:v]scale=1440:960:force_original_aspect_ratio=increase,crop=1440:960,setsar=1[v0]",
		"[1:v]crop=1440:960:0:60,scale=1440:960:force_original_aspect_ratio=increase,crop=1440:960,setsar=1[v1]",
		"[v0][v1]concat=n=2:v=1:a=0,fps=59.940,trim=start=2.000:end=12.000,setpts=PTS-STARTPTS,format=yuv420p[v]",
		"[2:a]aresample=48000", "[dsp0]atrim=start=2.500:end=12.500",
		"[3:a]aresample=48000", "[dtk0]atrim=start=2.500:end=12.500",
		"[dsp][dtk]amix=inputs=2:duration=longest:normalize=0,apad=whole_dur=10.000,atrim=end=10.000[a]",
		"-map [v] -map [a] -c:v libx264", "-g 120", "-c:a aac", "out.mp4",
	} {
		if !strings.Contains(args, want) {
			t.Errorf("missing %q in\n%s", want, args)
		}
	}
	// Sound shorter than the video: silence goes in front instead.
	j = finalJob{Video: []vidSeg{{Path: "a.avi", Dur: 10, W: 640, H: 480}}, DSP: []string{"x.wav", "y.wav"}, DSPLen: 9.75,
		From: 0, To: 10, FPS: 30, Out: "o.mp4"}
	args = strings.Join(finalArgs(j, "h264_qsv", "balanced"), " ")
	for _, want := range []string{"[dsp0][dsp1]concat=n=2:v=0:a=1,adelay=250:all=1,atrim=start=0.000:end=10.000", "[0:v]setsar=1[v0];[v0]fps=30.000", "format=nv12[v]"} {
		if !strings.Contains(args, want) {
			t.Errorf("missing %q in\n%s", want, args)
		}
	}
	// No sound at all.
	j.DSP, j.DSPLen = nil, 0
	if args := strings.Join(finalArgs(j, "libx264", "high"), " "); strings.Contains(args, "[a]") || strings.Contains(args, "-c:a") {
		t.Errorf("sound in a silent video:\n%s", args)
	}
}

func TestOutputFPSAndBars(t *testing.T) {
	for _, c := range []struct {
		src  float64
		want int
		out  float64
	}{{59.94, 60, 59.94}, {59.94, 30, 29.97}, {50, 30, 25}, {0, 60, 60}, {30, 30, 30}} {
		if got := outputFPS(c.src, c.want); got != c.out {
			t.Errorf("outputFPS(%v, %d) = %v want %v", c.src, c.want, got, c.out)
		}
	}
	out := "[Parsed_cropdetect_0] x1:160 ... crop=960:720:160:0\n[Parsed_cropdetect_0] crop=960:720:160:0\n"
	if c := parseBars(out, 1280, 720); c == nil || *c != (crop{160, 0, 960, 720}) {
		t.Errorf("bars = %+v", c)
	}
	if c := parseBars("crop=1280:718:0:0", 1280, 720); c != nil {
		t.Errorf("trimmed a sliver: %+v", c)
	}
	if c := parseBars("crop=200:100:0:0", 1280, 720); c != nil {
		t.Errorf("trimmed something odd: %+v", c)
	}
	if got := safeFile(`Zelda: "Wind" Waker™ / ok.`); got != "Zelda Wind Waker  ok" {
		t.Errorf("safeFile = %q", got)
	}
}

// ---------------- with a real ffmpeg ----------------

func testFFmpeg(t *testing.T) string {
	if testing.Short() {
		t.Skip("makes videos")
	}
	ff := os.Getenv("RETRO_FFMPEG")
	if ff == "" {
		if p, err := exec.LookPath("ffmpeg"); err == nil {
			ff = p
		}
	}
	if ff == "" {
		t.Skip("no ffmpeg (set RETRO_FFMPEG)")
	}
	return ff
}

func runFF(t *testing.T, ff string, args ...string) {
	t.Helper()
	if out, err := exec.Command(ff, append([]string{"-hide_banner", "-loglevel", "error", "-y"}, args...)...).CombinedOutput(); err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
}

// probe returns "WxH", duration seconds and whether there's an audio stream.
func probe(t *testing.T, ff, file string) (string, float64, bool) {
	out, _ := exec.Command(ff, "-hide_banner", "-i", file).CombinedOutput()
	s := string(out)
	size := regexp.MustCompile(`Video: h264.*?, (\d+x\d+)`).FindStringSubmatch(s)
	dur := regexp.MustCompile(`Duration: (\d+):(\d+):([\d.]+)`).FindStringSubmatch(s)
	if size == nil || dur == nil {
		t.Fatalf("couldn't read %s:\n%s", file, s)
	}
	var h, m int
	var sec float64
	fmt.Sscan(dur[1], &h)
	fmt.Sscan(dur[2], &m)
	fmt.Sscan(dur[3], &sec)
	return size[1], float64(h*3600+m*60) + sec, strings.Contains(s, "Audio: aac")
}

// writeDumps writes what Dolphin saves for a game: two video files (the
// window was resized), the first with black bars the game drew itself, game
// sound that starts half a second before the picture, and disc music.
func writeDumps(t *testing.T, ff, dir string) {
	os.MkdirAll(filepath.Join(dir, "Frames"), 0o755)
	os.MkdirAll(filepath.Join(dir, "Audio"), 0o755)
	runFF(t, ff, "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=32000:duration=5.5", "-ac", "2", "-c:a", "pcm_s16le",
		filepath.Join(dir, "Audio", "GMSE01_2026-10-14_12-00-00_dspdump.wav"))
	runFF(t, ff, "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000:duration=5.5", "-ac", "2", "-c:a", "pcm_s16le",
		filepath.Join(dir, "Audio", "GMSE01_2026-10-14_12-00-00_dtkdump.wav"))
	runFF(t, ff, "-f", "lavfi", "-i", "testsrc2=size=800x720:rate=60000/1001:duration=3", "-vf", "pad=960:720:80:0:black",
		"-c:v", "mpeg4", "-b:v", "20M", filepath.Join(dir, "Frames", "GMSE01_2026-10-14_12-00-00_0.avi"))
	second := filepath.Join(dir, "Frames", "GMSE01_2026-10-14_12-00-00_1.avi")
	runFF(t, ff, "-f", "lavfi", "-i", "testsrc2=size=1280x960:rate=60000/1001:duration=2", "-c:v", "mpeg4", "-b:v", "20M", second)
	later := time.Now().Add(time.Second)
	os.Chtimes(second, later, later)
}

// The whole thing: a game recorded from its start is saved as one .mp4 of
// just the game, Dolphin's raw files are deleted, and the log says what happened.
func TestRecordingSave(t *testing.T) {
	ff := testFFmpeg(t)
	dumps, recs := t.TempDir(), t.TempDir()
	log := filepath.Join(recs, "logs", "last-recording.log")
	r := NewRecording("Super Mario Sunshine", dumps, log)
	r.Launched = time.Now().Add(-2 * time.Second)
	r.Start([]string{"-b", "-e", "game.rvz"})
	done := make(chan struct{})
	followed := make(chan struct{})
	var warnings []string
	go func() { r.Follow(done, func(m string) { warnings = append(warnings, m) }); close(followed) }()
	writeDumps(t, ff, dumps)
	time.Sleep(1200 * time.Millisecond) // Follow notices the first picture
	close(done)
	<-followed
	if on, _ := r.Active(); on {
		t.Fatal("still recording after the game ended")
	}
	var steps []string
	saved, err := r.Save(SaveOptions{Dir: recs, Quality: "balanced", FPS: 60, FFmpeg: func() string { return ff },
		Progress: func(step string, pc int) {
			if len(steps) == 0 || steps[len(steps)-1] != step {
				steps = append(steps, step)
			}
		}})
	if err != nil || len(saved) != 1 || !strings.HasPrefix(saved[0], "Super Mario Sunshine ") {
		t.Fatalf("saved %v, %v", saved, err)
	}
	if strings.Join(steps, ",") != "ffmpeg,saving" || len(warnings) != 0 {
		t.Errorf("steps %v warnings %v", steps, warnings)
	}
	size, dur, audio := probe(t, ff, filepath.Join(recs, saved[0]))
	t.Logf("%s: %s %.2fs audio=%v", saved[0], size, dur, audio)
	if size != "1280x960" || !audio || dur < 4.8 || dur > 5.3 {
		t.Fatalf("bad recording: %s %.2fs audio=%v", size, dur, audio)
	}
	if left := FindDumps(dumps, time.Time{}).All(); len(left) != 0 {
		t.Fatalf("raw files left behind: %v", left)
	}
	l, _ := os.ReadFile(log)
	if !strings.Contains(string(l), "saved ") || !strings.Contains(string(l), "bars=&{80 0 800 720}") ||
		!strings.Contains(string(l), "Dolphin -b -e game.rvz") || !strings.Contains(string(l), "started saving the picture") {
		t.Errorf("log:\n%s", l)
	}
}

// Stop mid-game keeps only the part before it; a game where Dolphin saved
// nothing gives a clear message.
func TestRecordingStopAndNothing(t *testing.T) {
	ff := testFFmpeg(t)
	dumps, recs := t.TempDir(), t.TempDir()
	r := NewRecording("Sunshine", dumps, filepath.Join(recs, "log.txt"))
	r.Launched = time.Now().Add(-2 * time.Second)
	writeDumps(t, ff, dumps)
	r.Stop()
	r.clips[0].To.Audio = 2 // 2s into the sound = 1.5s into the video
	r.Ended = time.Now()
	saved, err := r.Save(SaveOptions{Dir: recs, FPS: 30, FFmpeg: func() string { return ff }})
	if err != nil || len(saved) != 1 {
		t.Fatalf("saved %v %v", saved, err)
	}
	if _, dur, audio := probe(t, ff, filepath.Join(recs, saved[0])); !audio || dur < 1.3 || dur > 1.8 {
		t.Fatalf("stop not applied: %.2fs", dur)
	}

	empty := NewRecording("Nothing", t.TempDir(), filepath.Join(t.TempDir(), "log.txt"))
	empty.Launched = time.Now().Add(-time.Minute)
	empty.Ended = time.Now()
	if saved, err := empty.Save(SaveOptions{Dir: recs, FFmpeg: func() string { t.Fatal("asked for ffmpeg"); return "" }}); len(saved) != 0 ||
		err == nil || !strings.Contains(err.Error(), "didn't save the game's picture") {
		t.Fatalf("nothing saved: %v %v", saved, err)
	}

	old := NoVideoAfter
	NoVideoAfter = 500 * time.Millisecond
	defer func() { NoVideoAfter = old }()
	done := make(chan struct{})
	warned := make(chan string, 1)
	go empty.Follow(done, func(m string) { warned <- m })
	select {
	case m := <-warned:
		if !strings.Contains(m, "hasn't started saving") {
			t.Errorf("warning %q", m)
		}
	case <-time.After(5 * time.Second):
		t.Error("no warning")
	}
	close(done)
}

// The picture and sound line up: a white flash at 2s in Dolphin's video and a
// beep at 2.5s in its sound (which started half a second earlier) land at the
// same moment in the finished video, also when a clip starts mid-game.
func TestFinalSync(t *testing.T) {
	ff := testFFmpeg(t)
	dir := t.TempDir()
	avi, wav := filepath.Join(dir, "v.avi"), filepath.Join(dir, "dspdump.wav")
	runFF(t, ff, "-f", "lavfi", "-i", "color=black:s=320x240:r=60000/1001:d=6,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='between(t,3,3.2)'",
		"-c:v", "mpeg4", "-q:v", "2", avi)
	runFF(t, ff, "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=32000:duration=6.5,volume=volume=0:enable='not(between(t,3.5,3.7))'",
		"-ac", "2", "-c:a", "pcm_s16le", wav)
	seg, err := probeSegment(ff, avi)
	if err != nil || seg.Dur < 5.9 || seg.Dur > 6.1 || seg.W != 320 {
		t.Fatalf("probe: %+v %v", seg, err)
	}
	for _, from := range []float64{0, 1.25} {
		out := filepath.Join(dir, fmt.Sprintf("out%v.mp4", from))
		j := finalJob{Video: []vidSeg{seg}, DSP: []string{wav}, DSPLen: WavSeconds(wav), From: from, To: 6, FPS: 59.94, Out: out}
		if err := encodeClip(ff, j, "balanced", func(string, ...any) {}, func(float64) {}); err != nil {
			t.Fatal(err)
		}
		res, _ := exec.Command(ff, "-hide_banner", "-i", out, "-vf", "blackdetect=d=0.05:pix_th=0.5", "-af", "silencedetect=n=-40dB:d=0.05", "-f", "null", "-").CombinedOutput()
		v := regexp.MustCompile(`black_end:([\d.]+)`).FindStringSubmatch(string(res))
		a := regexp.MustCompile(`silence_end: ([\d.]+)`).FindStringSubmatch(string(res))
		if v == nil || a == nil {
			t.Fatalf("couldn't find the flash/beep:\n%s", res)
		}
		var vt, at float64
		fmt.Sscan(v[1], &vt)
		fmt.Sscan(a[1], &at)
		if d := vt - at; d > 0.05 || d < -0.05 || vt < 2.9-from || vt > 3.1-from {
			t.Fatalf("out of sync: flash %.3f beep %.3f", vt, at)
		}
	}
}
