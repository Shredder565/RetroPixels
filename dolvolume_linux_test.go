//go:build linux

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// What `LC_ALL=C pactl list sink-inputs` prints (PulseAudio 16; pipewire-pulse
// prints the same layout). The second stream has no process ID of its own.
const sinkInputsSample = `Sink Input #0
	Driver: protocol-native.c
	Owner Module: 7
	Client: 2
	Sink: 1
	Sample Specification: s16le 2ch 44100Hz
	Channel Map: front-left,front-right
	Format: pcm, format.sample_format = "\"s16le\""  format.rate = "44100"  format.channels = "2"  format.channel_map = "\"front-left,front-right\""
	Corked: no
	Mute: no
	Volume: front-left: 65536 / 100% / 0.00 dB,   front-right: 65536 / 100% / 0.00 dB
	        balance 0.00
	Buffer Latency: 2000000 usec
	Sink Latency: 958341 usec
	Resample method: n/a
	Properties:
		application.name = "pacat"
		media.name = "/dev/zero"
		application.process.id = "22417"
		application.process.binary = "pacat"
		module-stream-restore.id = "sink-input-by-application-name:pacat"

Sink Input #57
	Driver: PipeWire
	Owner Module: n/a
	Client: 91
	Sink: 48
	Sample Specification: float32le 1ch 48000Hz
	Channel Map: mono
	Format: pcm, format.sample_format = "\"float32le\""  format.rate = "48000"  format.channels = "1"  format.channel_map = "\"mono\""
	Corked: no
	Mute: yes
	Volume: mono: 22938 /  35% / -27.36 dB
	        balance 0.00
	Buffer Latency: 0 usec
	Sink Latency: 0 usec
	Resample method: PipeWire
	Properties:
		media.name = "Playback"
		application.name = "PixelU-Wii"
`

const clientsSample = `Client #91
	Driver: PipeWire
	Owner Module: n/a
	Properties:
		application.name = "PixelU-Wii"
		application.process.id = "4242"
		application.process.binary = "PixelU-Wii"
`

func TestParseSinkInputs(t *testing.T) {
	in := parseSinkInputs([]byte(sinkInputsSample))
	if len(in) != 2 {
		t.Fatalf("%d streams: %+v", len(in), in)
	}
	a, b := in[0], in[1]
	if a.index != 0 || a.client != 2 || a.mute || len(a.vols) != 2 || a.vols[0] != 65536 || a.vols[1] != 65536 {
		t.Fatalf("first: %+v", a)
	}
	if a.props["application.process.id"] != "22417" || a.props["application.process.binary"] != "pacat" ||
		a.props["module-stream-restore.id"] != "sink-input-by-application-name:pacat" {
		t.Fatalf("first props: %v", a.props)
	}
	if _, ok := a.props["format.rate"]; ok {
		t.Fatal("format line read as a property")
	}
	if b.index != 57 || b.client != 91 || !b.mute || len(b.vols) != 1 || b.vols[0] != 22938 || b.props["application.name"] != "PixelU-Wii" {
		t.Fatalf("second: %+v", b)
	}
	c := parseClients([]byte(clientsSample))
	if c[91]["application.process.id"] != "4242" {
		t.Fatalf("clients: %v", c)
	}
}

func TestStreamOf(t *testing.T) {
	pids := map[int]bool{100: true, 101: true}
	cases := []struct {
		props map[string]string
		exe   string
		want  bool
	}{
		{map[string]string{"application.process.id": "101"}, "", true},
		{map[string]string{"application.process.id": "7", "application.process.binary": "pacat"}, "PixelU-Wii", false},
		{map[string]string{"application.process.id": "7", "application.process.binary": "PixelU-Wii"}, "PixelU-Wii", true},
		{map[string]string{"application.process.binary": "PixelU-Wii"}, "PixelU-Wii.exe", true},
		{map[string]string{"application.process.binary": "PixelU-Wii.exe"}, "PixelU-Wii.exe", true},
		{map[string]string{"application.process.binary": "PixelU"}, "PixelU-Wii", false},
		{map[string]string{"application.process.binary": ""}, ".", false},
	}
	for i, c := range cases {
		if got := streamOf(c.props, pids, c.exe); got != c.want {
			t.Errorf("%d: %v %q = %v", i, c.props, c.exe, got)
		}
	}
}

func TestProcessTree(t *testing.T) {
	proc := t.TempDir()
	write := func(pid, ppid int, name string) {
		d := filepath.Join(proc, strconv.Itoa(pid))
		os.MkdirAll(d, 0o755)
		os.WriteFile(filepath.Join(d, "stat"), []byte(strconv.Itoa(pid)+" ("+name+") S "+strconv.Itoa(ppid)+" 1 1 0 -1\n"), 0o644)
	}
	write(1, 0, "init")
	write(10, 1, "sh")
	write(11, 10, "PixelU-Wii")
	write(12, 11, "a) b (c") // a name with spaces and parentheses
	write(13, 12, "worker")
	write(20, 1, "other")
	os.MkdirAll(filepath.Join(proc, "self"), 0o755)
	got := processTree(proc, 10)
	if len(got) != 4 || !got[10] || !got[11] || !got[12] || !got[13] || got[20] || got[1] {
		t.Fatalf("tree: %v", got)
	}
	if len(processTree(proc, 0)) != 0 {
		t.Fatal("pid 0 has a tree")
	}
}

// Against a real sound server, when one is running (PulseAudio with a null
// sink, or PipeWire): a pacat started by a shell plays silence, and the
// shell's process ID is what RetroPixels knows, as with an engine.
func TestProgramVolumeOnServer(t *testing.T) {
	if _, err := exec.LookPath("pactl"); err != nil {
		t.Skip("no pactl")
	}
	if _, err := exec.LookPath("pacat"); err != nil {
		t.Skip("no pacat")
	}
	if _, err := pactlCmd("info"); err != nil {
		t.Skip("no sound server")
	}
	// The shell stays the parent: it runs pacat, then waits.
	cmd := exec.Command("sh", "-c", "pacat --raw /dev/zero; wait")
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		cmd.Wait()
	}()
	pid := cmd.Process.Pid
	find := func() *sinkInput {
		t.Helper()
		out, err := pactlCmd("list", "sink-inputs")
		if err != nil {
			if e, ok := err.(*exec.ExitError); ok {
				t.Fatalf("%v: %s", err, e.Stderr)
			}
			t.Fatal(err)
		}
		tree := processTree("/proc", pid)
		for _, in := range parseSinkInputs(out) {
			if streamOf(in.props, tree, "") {
				return &in
			}
		}
		return nil
	}
	var in *sinkInput
	for i := 0; i < 50 && in == nil; i++ {
		time.Sleep(100 * time.Millisecond)
		in = find()
	}
	if in == nil {
		t.Fatal("pacat's stream never showed up")
	}
	if n := setProgramVolume(pid, "", 0.35, false); n != 1 {
		t.Fatalf("set %d streams", n)
	}
	if in = find(); in == nil || in.mute || !volumeIs(in.vols, 22938) {
		t.Fatalf("after 35%%: %+v", in)
	}
	if n := setProgramVolume(pid, "", 0.8, true); n != 1 {
		t.Fatalf("set %d streams", n)
	}
	if in = find(); in == nil || !in.mute || !volumeIs(in.vols, 52429) {
		t.Fatalf("after 80%% muted: %+v", in)
	}
	// Another program's stream is left alone.
	if n := setProgramVolume(1<<22, "no-such-engine", 0, false); n != 0 {
		t.Fatalf("set %d streams of nobody", n)
	}
	if in = find(); in == nil || !in.mute || !volumeIs(in.vols, 52429) {
		t.Fatalf("changed by another program's setting: %+v", in)
	}

	// The follower: the saved setting, and again when it changes. The
	// setting is saved in a test folder, so the sound server is named
	// directly (libpulse looks for it under XDG_CONFIG_HOME too).
	if info, err := pactlCmd("info"); err == nil {
		for _, l := range strings.Split(string(info), "\n") {
			if s, ok := strings.CutPrefix(l, "Server String: "); ok && strings.HasPrefix(s, "/") {
				t.Setenv("PULSE_SERVER", "unix:"+s)
			}
		}
	}
	if os.Getenv("PULSE_COOKIE") == "" {
		if d, err := os.UserConfigDir(); err == nil {
			if c := filepath.Join(d, "pulse", "cookie"); volTestExists(c) {
				t.Setenv("PULSE_COOKIE", c)
			}
		}
	}
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	cfgMu.Lock()
	cfg = config{}
	cfgMu.Unlock()
	exited := make(chan struct{})
	done := make(chan struct{})
	go func() { followDolphinVolume(pid, exited); close(done) }()
	waitFor := func(what string, ok func(*sinkInput) bool) {
		t.Helper()
		for i := 0; i < 60; i++ {
			if in := find(); in != nil && ok(in) {
				return
			}
			time.Sleep(100 * time.Millisecond)
		}
		t.Fatalf("never %s: %+v", what, find())
	}
	waitFor("100% unmuted", func(in *sinkInput) bool { return !in.mute && volumeIs(in.vols, paVolumeNorm) })
	v := 20.0
	if err := setDolphinVolume(&v, nil); err != nil {
		t.Fatal(err)
	}
	waitFor("20%", func(in *sinkInput) bool { return volumeIs(in.vols, 13107) })
	close(exited)
	<-done
	volMu.Lock()
	left := volPID
	volMu.Unlock()
	if left != 0 {
		t.Fatalf("volPID %d after exit", left)
	}
}

func volTestExists(p string) bool { _, err := os.Stat(p); return err == nil }

func TestVolumeParseOdd(t *testing.T) {
	if v := parseVolumes(" front-left: 1 / 0% / -inf dB,   front-right: 65536 / 100% / 0.00 dB"); len(v) != 2 || v[0] != 1 {
		t.Fatalf("%v", v)
	}
	if v := parseVolumes(" n/a"); len(v) != 0 {
		t.Fatalf("%v", v)
	}
	if !strings.Contains(sinkInputsSample, "Sink Input #57") {
		t.Fatal("sample")
	}
}
