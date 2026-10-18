//go:build !windows

package main

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

func TestDolphinRecordArgs(t *testing.T) {
	args := dolphinRecordArgs("high")
	want := map[string]bool{
		"Dolphin.Movie.DumpFrames=True": false, "Dolphin.DSP.DumpAudio=True": false,
		"Graphics.Settings.DumpFormat=avi": false, "Graphics.Settings.DumpCodec=mpeg4": false,
		"Graphics.Settings.InternalResolutionFrameDumps=False": false, "Graphics.Settings.BitrateKbps=50000": false,
		"Graphics.Settings.DumpPath=": false,
	}
	for i := 0; i < len(args); i += 2 {
		if args[i] != "-C" {
			t.Fatalf("args not in -C pairs: %v", args)
		}
		// Dolphin reads System.Section.Key=Value.
		if !regexp.MustCompile(`^(Dolphin|Graphics)\.[A-Za-z]+\.[A-Za-z0-9]+=[^=]*$`).MatchString(args[i+1]) {
			t.Errorf("bad setting %q", args[i+1])
		}
		if _, ok := want[args[i+1]]; ok {
			want[args[i+1]] = true
		}
	}
	for k, seen := range want {
		if !seen {
			t.Errorf("missing %s", k)
		}
	}
	if !strings.Contains(strings.Join(dolphinRecordArgs("small"), " "), "BitrateKbps=25000") {
		t.Error("quality doesn't set the bitrate")
	}
}

func TestDolphinDumpDir(t *testing.T) {
	dir := t.TempDir()
	os.Setenv("RETRO_DOLPHIN_USER", dir)
	defer os.Unsetenv("RETRO_DOLPHIN_USER")
	if got := dolphinDumpDir(); got != filepath.Join(dir, "Dump") {
		t.Fatalf("default dump dir = %s", got)
	}
	os.MkdirAll(filepath.Join(dir, "Config"), 0o755)
	os.WriteFile(dolphinIni("Dolphin.ini"), []byte("[General]\r\nDumpPath = /data/dumps/\r\n"), 0o644)
	if got := dolphinDumpDir(); got != "/data/dumps" {
		t.Fatalf("custom dump dir = %s", got)
	}
}

func TestWavSeconds(t *testing.T) {
	dir := t.TempDir()
	writeWav(t, filepath.Join(dir, "a.wav"), 32000, 2.5, false)
	writeWav(t, filepath.Join(dir, "b.wav"), 48000, 1.25, true)
	os.WriteFile(filepath.Join(dir, "c.wav"), []byte("not a wav"), 0o644)
	for name, want := range map[string]float64{"a.wav": 2.5, "b.wav": 1.25, "c.wav": 0, "missing.wav": 0} {
		if got := wavSeconds(filepath.Join(dir, name)); got < want-0.001 || got > want+0.001 {
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
	d := findDumps(dir, since)
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
	d.remove()
	if _, err := os.Stat(old); err != nil {
		t.Error("deleted a dump from an earlier game")
	}
	if len(findDumps(dir, since).all()) != 0 {
		t.Error("dumps not deleted")
	}
}

// Stop and Record mid-game mark where the game is by how much sound Dolphin
// has saved; the video keeps only those parts.
func TestStopTrim(t *testing.T) {
	dir := t.TempDir()
	s := newRecSession("Wind Waker", dir)
	s.Launched = time.Now().Add(-time.Minute)
	dsp := filepath.Join(dir, "Audio", "GZLE01_dspdump.wav")
	writeWav(t, dsp, 32000, 12.5, true)
	if !s.stop() {
		t.Fatal("stop did nothing")
	}
	if s.stop() {
		t.Fatal("stopped twice")
	}
	if on, _ := s.recording(); on {
		t.Fatal("still recording after stop")
	}
	writeWav(t, dsp, 32000, 20, true)
	s.resume()
	if on, since := s.recording(); !on || time.Since(since) > time.Second {
		t.Fatalf("resume: %v %v", on, since)
	}
	s.clips[1].Open, s.clips[1].To = false, mark{Audio: -1, Edge: true}

	if c := s.clips[0]; c.To.Audio < 12.49 || c.To.Audio > 12.51 {
		t.Fatalf("stop mark = %+v", c.To)
	}
	// Video 30s, sound 30.5s: the sound started half a second earlier.
	first := time.Now()
	from, to := clipRange(s.clips[0], 30, 30.5, first)
	if from != 0 || to < 11.99 || to > 12.01 {
		t.Errorf("first clip %.2f-%.2f, want 0-12", from, to)
	}
	from, to = clipRange(s.clips[1], 30, 30.5, first)
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
}

// ---------------- with a real ffmpeg ----------------

func testFFmpeg(t *testing.T) string {
	ff := os.Getenv("RETRO_FFMPEG")
	if ff == "" {
		if p, err := exec.LookPath("ffmpeg"); err == nil {
			ff = p
			os.Setenv("RETRO_FFMPEG", p)
		}
	}
	if ff == "" {
		t.Skip("no ffmpeg (set RETRO_FFMPEG)")
	}
	return ff
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

// fakeDolphin is a stand-in PixelU-Wii.exe (Dolphin renamed): it notes its arguments and, when
// asked to dump, writes video and sound files the way Dolphin does - two
// video files (the window was resized), the first with black bars the game
// drew itself, game sound that starts half a second before the picture, and
// disc music.
const fakeDolphin = `#!/bin/sh
echo "$@" >> "$FAKE_ARGS"
case "$*" in *Dolphin.Movie.DumpFrames=True*) ;; *) sleep 1; exit 0 ;; esac
D="$RETRO_DOLPHIN_USER/Dump"
mkdir -p "$D/Frames" "$D/Audio"
FF="$RETRO_FFMPEG -hide_banner -loglevel error -y"
$FF -f lavfi -i "sine=frequency=440:sample_rate=32000:duration=5.5" -ac 2 -c:a pcm_s16le "$D/Audio/GMSE01_2026-10-14_12-00-00_dspdump.wav"
$FF -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=5.5" -ac 2 -c:a pcm_s16le "$D/Audio/GMSE01_2026-10-14_12-00-00_dtkdump.wav"
$FF -f lavfi -i "testsrc2=size=800x720:rate=60000/1001:duration=3" -vf "pad=960:720:80:0:black" -c:v mpeg4 -b:v 20M "$D/Frames/GMSE01_2026-10-14_12-00-00_0.avi"
sleep 1
$FF -f lavfi -i "testsrc2=size=1280x960:rate=60000/1001:duration=2" -c:v mpeg4 -b:v 20M "$D/Frames/GMSE01_2026-10-14_12-00-00_1.avi"
sleep "${FAKE_PLAY:-1}"
`

func setupFakeDolphin(t *testing.T) (ff, argsFile string, launch func() error) {
	ff = testFFmpeg(t)
	root := makeRoms(t)
	user := t.TempDir()
	recs := t.TempDir()
	argsFile = filepath.Join(t.TempDir(), "args")
	for k, v := range map[string]string{"RETRO_DOLPHIN_USER": user, "RETRO_REC_DIR": recs, "FAKE_ARGS": argsFile, "XDG_CONFIG_HOME": t.TempDir()} {
		t.Setenv(k, v)
	}
	cfgMu.Lock()
	cfg = config{DolphinFolders: []string{filepath.Join(root, "- Mario Games")}}
	cfgMu.Unlock()
	fakeEngine(t, fakeDolphin)
	setManualWanted(false)
	recMu.Lock()
	recLastErr, recLastMsg = "", ""
	recMu.Unlock()
	games := dolphinLibrary()
	if len(games) != 1 {
		t.Fatalf("library: %+v", games)
	}
	return ff, argsFile, func() error { return launchDolphinGame(games[0].Path) }
}

// waitIdle waits for the game to end and its recording to be saved.
func waitIdle(t *testing.T) {
	deadline := time.Now().Add(90 * time.Second)
	for time.Now().Before(deadline) {
		time.Sleep(200 * time.Millisecond)
		runMu.Lock()
		on := running != nil
		runMu.Unlock()
		st := recStatus()
		if !on && st["saving"] == nil && !st["dumping"].(bool) && st["queued"].(int) == 0 {
			return
		}
	}
	t.Fatal("game or recording didn't finish")
}

// The whole thing: a game started with recording on is saved as one .mp4 of
// just the game, and Dolphin's raw files are deleted.
func TestRecordGameEndToEnd(t *testing.T) {
	ff, argsFile, launch := setupFakeDolphin(t)
	if err := launch(); err != nil {
		t.Fatal(err)
	}
	if st := recStatus(); !st["dumping"].(bool) || st["active"] == nil {
		t.Fatalf("not recording: %v", st)
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	if !strings.Contains(string(args), "-C Dolphin.Movie.DumpFrames=True") || !strings.Contains(string(args), "-C Dolphin.DSP.DumpAudio=True") {
		t.Fatalf("Dolphin wasn't asked to save the game: %s", args)
	}
	st := recStatus()
	if st["error"] != "" || !strings.HasPrefix(st["message"].(string), "Saved Super Mario Sunshine") {
		t.Fatalf("status: %v", st)
	}
	files, _ := filepath.Glob(filepath.Join(recordingsDir(), "*.mp4"))
	if len(files) != 1 {
		t.Fatalf("want 1 recording, got %v", files)
	}
	size, dur, audio := probe(t, ff, files[0])
	t.Logf("%s: %s %.2fs audio=%v", filepath.Base(files[0]), size, dur, audio)
	if size != "1280x960" || !audio || dur < 4.8 || dur > 5.3 {
		t.Fatalf("bad recording: %s %.2fs audio=%v", size, dur, audio)
	}
	if left := findDumps(dolphinDumpDir(), time.Time{}).all(); len(left) != 0 {
		t.Fatalf("raw files left behind: %v", left)
	}
	log, _ := os.ReadFile(recLogPath())
	if !strings.Contains(string(log), "saved ") || !strings.Contains(string(log), "bars=&{80 0 800 720}") {
		t.Errorf("log:\n%s", log)
	}
	if testing.Verbose() {
		t.Logf("log:\n%s", log)
	}
}

// Stop mid-game keeps only the part before it.
func TestRecordStopMidGame(t *testing.T) {
	ff, _, launch := setupFakeDolphin(t)
	t.Setenv("FAKE_PLAY", "3")
	if err := launch(); err != nil {
		t.Fatal(err)
	}
	// Wait for the fake to have written everything, then make the stop
	// point 2s into the sound (1.5s into the video).
	deadline := time.Now().Add(30 * time.Second)
	for len(findDumps(dolphinDumpDir(), time.Time{}).Video) < 2 && time.Now().Before(deadline) {
		time.Sleep(100 * time.Millisecond)
	}
	recMu.Lock()
	s := recCur
	recMu.Unlock()
	if note, err := recControl("stop"); err != nil || !strings.Contains(note, "stopped") {
		t.Fatalf("stop: %q %v", note, err)
	}
	s.mu.Lock()
	s.clips[0].To.Audio = 2
	s.mu.Unlock()
	if st := recStatus(); st["active"] != nil {
		t.Fatal("still shows REC after stop")
	}
	waitIdle(t)
	files, _ := filepath.Glob(filepath.Join(recordingsDir(), "*.mp4"))
	if len(files) != 1 {
		t.Fatalf("want 1 recording, got %v", files)
	}
	size, dur, audio := probe(t, ff, files[0])
	t.Logf("%s %.2fs audio=%v", size, dur, audio)
	if !audio || dur < 1.3 || dur > 1.8 {
		t.Fatalf("stop not applied: %.2fs", dur)
	}
}

// With automatic recording off, Record during a game means the next game.
func TestRecordNextGame(t *testing.T) {
	_, argsFile, launch := setupFakeDolphin(t)
	off := false
	cfgMu.Lock()
	cfg.AutoRecord = &off
	cfgMu.Unlock()
	if err := launch(); err != nil {
		t.Fatal(err)
	}
	if st := recStatus(); st["dumping"].(bool) {
		t.Fatal("recording with automatic recording off")
	}
	note, _ := recControl("start")
	if !strings.Contains(note, "next game") || !recStatus()["next"].(bool) {
		t.Fatalf("note %q", note)
	}
	waitIdle(t)
	if err := launch(); err != nil {
		t.Fatal(err)
	}
	if recStatus()["next"].(bool) {
		t.Error("still waiting for the next game")
	}
	waitIdle(t)
	args, _ := os.ReadFile(argsFile)
	lines := strings.Split(strings.TrimSpace(string(args)), "\n")
	if len(lines) != 2 || strings.Contains(lines[0], "DumpFrames") || !strings.Contains(lines[1], "DumpFrames=True") {
		t.Fatalf("args:\n%s", args)
	}
	if files, _ := filepath.Glob(filepath.Join(recordingsDir(), "*.mp4")); len(files) != 1 {
		t.Fatalf("recordings: %v", files)
	}
}

// Dolphin that saves nothing: a clear message, no crash.
func TestRecordNoDumps(t *testing.T) {
	_, _, launch := setupFakeDolphin(t)
	fakeEngine(t, "#!/bin/sh\nsleep 2\n")
	old := recNoVideoAfter
	recNoVideoAfter = time.Second
	defer func() { recNoVideoAfter = old }()
	if err := launch(); err != nil {
		t.Fatal(err)
	}
	time.Sleep(1500 * time.Millisecond)
	if e := recStatus()["error"].(string); !strings.Contains(e, "hasn't started saving") {
		t.Fatalf("error = %q", e)
	}
	waitIdle(t)
	if files, _ := filepath.Glob(filepath.Join(recordingsDir(), "*.mp4")); len(files) != 0 {
		t.Fatalf("recordings: %v", files)
	}
}

// The picture and sound line up: a white flash at 2s in Dolphin's video and a
// beep at 2.5s in its sound (which started half a second earlier) land at the
// same moment in the finished video, also when a clip starts mid-game.
func TestFinalSync(t *testing.T) {
	ff := testFFmpeg(t)
	dir := t.TempDir()
	avi, wav := filepath.Join(dir, "v.avi"), filepath.Join(dir, "dspdump.wav")
	run := func(args ...string) {
		if out, err := exec.Command(ff, append([]string{"-hide_banner", "-loglevel", "error", "-y"}, args...)...).CombinedOutput(); err != nil {
			t.Fatalf("%v\n%s", err, out)
		}
	}
	run("-f", "lavfi", "-i", "color=black:s=320x240:r=60000/1001:d=6,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='between(t,3,3.2)'",
		"-c:v", "mpeg4", "-q:v", "2", avi)
	run("-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=32000:duration=6.5,volume=volume=0:enable='not(between(t,3.5,3.7))'",
		"-ac", "2", "-c:a", "pcm_s16le", wav)
	seg, err := probeSegment(ff, avi)
	if err != nil || seg.Dur < 5.9 || seg.Dur > 6.1 || seg.W != 320 {
		t.Fatalf("probe: %+v %v", seg, err)
	}
	for _, from := range []float64{0, 1.25} {
		out := filepath.Join(dir, fmt.Sprintf("out%v.mp4", from))
		j := finalJob{Video: []vidSeg{seg}, DSP: []string{wav}, DSPLen: wavSeconds(wav), From: from, To: 6, FPS: 59.94, Out: out}
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
		t.Logf("from %.2f: flash at %.3fs, beep at %.3fs", from, vt, at)
		if d := vt - at; d > 0.05 || d < -0.05 || vt < 2.9-from || vt > 3.1-from {
			t.Fatalf("out of sync: flash %.3f beep %.3f", vt, at)
		}
	}
}
