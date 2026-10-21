//go:build linux

package main

// Sets the volume of the GameCube / Wii engine's sound the way a Linux
// volume mixer (pavucontrol, the desktop's sound settings) does: through the
// engine's own streams on the sound server ("sink inputs"). PulseAudio and
// PipeWire (through pipewire-pulse, the default on current desktops) both
// have them, and `pactl` (pulseaudio-utils, installed on every desktop that
// uses either) changes them. Only the engine's sound changes, not the rest of
// the computer's, and the engine doesn't need to know about it.

import (
	"bufio"
	"bytes"
	"context"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"retropixels/internal/dolphin"
)

var (
	volMu   sync.Mutex
	volPID  int
	volKick = make(chan struct{}, 1)
)

// followDolphinVolume applies the volume setting to one engine run. The
// engine only opens its sound a moment after it starts, so it keeps trying
// for a while, and again whenever the setting changes.
func followDolphinVolume(pid int, exited <-chan struct{}) {
	volMu.Lock()
	volPID = pid
	volMu.Unlock()
	defer func() {
		volMu.Lock()
		if volPID == pid {
			volPID = 0
		}
		volMu.Unlock()
	}()
	select {
	case <-volKick: // left over from before this game
	default:
	}
	exe := filepath.Base(dolphinExe())
	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	until := time.Now().Add(20 * time.Second)
	for {
		if time.Now().Before(until) {
			v, m := dolphinVolume()
			setProgramVolume(pid, exe, float64(v)/100, m)
		}
		select {
		case <-exited:
			return
		case <-volKick:
			if t := time.Now().Add(3 * time.Second); t.After(until) {
				until = t
			}
		case <-tick.C:
		}
	}
}

// applyDolphinVolumeNow re-applies the setting to the game that's playing.
func applyDolphinVolumeNow() {
	volMu.Lock()
	on := volPID != 0
	volMu.Unlock()
	if !on {
		return
	}
	select {
	case volKick <- struct{}{}:
	default:
	}
}

// paVolumeNorm is the sound server's 100% (PA_VOLUME_NORM).
const paVolumeNorm = 0x10000

// sinkInput is one sound stream as `pactl list sink-inputs` shows it.
type sinkInput struct {
	index  int
	client int // -1: none
	mute   bool
	vols   []int // raw volume of each channel
	props  map[string]string
}

// pactlCmd runs pactl with untranslated output (the parser reads its
// English headings), and gives up if the sound server doesn't answer.
func pactlCmd(args ...string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "pactl", args...)
	cmd.Env = append(os.Environ(), "LC_ALL=C", "LANG=C", "LANGUAGE=")
	return cmd.Output()
}

// setProgramVolume sets the volume (0–1) and mute of every sound stream that
// belongs to process pid or one of its child processes, or, as a fallback,
// to a program called exe (e.g. PixelU-Wii). It returns how many streams
// it changed or found already right.
func setProgramVolume(pid int, exe string, level float64, mute bool) int {
	out, err := pactlCmd("list", "sink-inputs")
	if err != nil {
		return 0 // no pactl, or no sound server: nothing to set
	}
	inputs := parseSinkInputs(out)
	if len(inputs) == 0 {
		return 0
	}
	// A stream without its program's process ID: take it from the stream's client.
	var clients map[int]map[string]string
	for _, in := range inputs {
		if in.props["application.process.id"] == "" && in.client >= 0 {
			if b, err := pactlCmd("list", "clients"); err == nil {
				clients = parseClients(b)
			}
			break
		}
	}
	pids := processTree("/proc", pid)
	want := uint64(math.Round(math.Max(0, math.Min(1, level)) * paVolumeNorm))
	done := 0
	for _, in := range inputs {
		props := in.props
		if c, ok := clients[in.client]; ok && props["application.process.id"] == "" {
			props = c
		}
		if !streamOf(props, pids, exe) {
			continue
		}
		ok := true
		if !volumeIs(in.vols, want) {
			_, err := pactlCmd("set-sink-input-volume", strconv.Itoa(in.index), strconv.FormatUint(want, 10))
			ok = err == nil
		}
		if in.mute != mute {
			m := "0"
			if mute {
				m = "1"
			}
			if _, err := pactlCmd("set-sink-input-mute", strconv.Itoa(in.index), m); err != nil {
				ok = false
			}
		}
		if ok {
			done++
		}
	}
	return done
}

func volumeIs(vols []int, want uint64) bool {
	if len(vols) == 0 {
		return false
	}
	for _, v := range vols {
		if uint64(v) != want {
			return false
		}
	}
	return true
}

// streamOf tells whether a stream with these properties is the engine's:
// made by one of the processes pids, or by a program called exe.
func streamOf(props map[string]string, pids map[int]bool, exe string) bool {
	if p, err := strconv.Atoi(props["application.process.id"]); err == nil && pids[p] {
		return true
	}
	bin := props["application.process.binary"]
	if bin == "" || exe == "" || exe == "." {
		return false
	}
	if dolphin.IsProgram(bin, exe) {
		return true
	}
	// A Linux build has no .exe: "PixelU-Wii" is "PixelU-Wii.exe" too.
	if ext := filepath.Ext(exe); strings.EqualFold(ext, ".exe") && len(exe) > len(ext) {
		return dolphin.IsProgram(bin, strings.TrimSuffix(exe, ext))
	}
	return false
}

// processTree is pid and every process started by it, its children's
// children too (read from proc, normally /proc).
func processTree(proc string, pid int) map[int]bool {
	tree := map[int]bool{}
	if pid <= 0 {
		return tree
	}
	tree[pid] = true
	parent := map[int][]int{}
	ents, _ := os.ReadDir(proc)
	for _, e := range ents {
		p, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		b, err := os.ReadFile(filepath.Join(proc, e.Name(), "stat"))
		if err != nil {
			continue
		}
		if pp, ok := statParent(b); ok {
			parent[pp] = append(parent[pp], p)
		}
	}
	todo := []int{pid}
	for len(todo) > 0 {
		p := todo[len(todo)-1]
		todo = todo[:len(todo)-1]
		for _, c := range parent[p] {
			if !tree[c] {
				tree[c] = true
				todo = append(todo, c)
			}
		}
	}
	return tree
}

// statParent reads the parent's process ID from /proc/<pid>/stat:
// "pid (name) state ppid ...", where the name may itself hold spaces and ')'.
func statParent(stat []byte) (int, bool) {
	i := bytes.LastIndexByte(stat, ')')
	if i < 0 {
		return 0, false
	}
	f := strings.Fields(string(stat[i+1:]))
	if len(f) < 2 {
		return 0, false
	}
	pp, err := strconv.Atoi(f[1])
	return pp, err == nil
}

// parseSinkInputs reads `LC_ALL=C pactl list sink-inputs` (PulseAudio's
// and pipewire-pulse's output are the same layout).
func parseSinkInputs(out []byte) []sinkInput {
	var list []sinkInput
	var cur *sinkInput
	inProps := false
	sc := bufio.NewScanner(bytes.NewReader(out))
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		line := sc.Text()
		if rest, ok := strings.CutPrefix(line, "Sink Input #"); ok {
			n, err := strconv.Atoi(strings.TrimSpace(rest))
			if err != nil {
				cur = nil
				continue
			}
			list = append(list, sinkInput{index: n, client: -1, props: map[string]string{}})
			cur = &list[len(list)-1]
			inProps = false
			continue
		}
		if cur == nil {
			continue
		}
		t := strings.TrimSpace(line)
		// Properties are indented one level deeper than the stream's own fields.
		if inProps && strings.HasPrefix(line, "\t\t") {
			if k, v, ok := parseProp(t); ok {
				cur.props[k] = v
			}
			continue
		}
		inProps = false
		switch {
		case t == "Properties:":
			inProps = true
		case strings.HasPrefix(t, "Client:"):
			if n, err := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(t, "Client:"))); err == nil {
				cur.client = n
			}
		case strings.HasPrefix(t, "Mute:"):
			cur.mute = strings.TrimSpace(strings.TrimPrefix(t, "Mute:")) == "yes"
		case strings.HasPrefix(t, "Volume:"):
			cur.vols = parseVolumes(strings.TrimPrefix(t, "Volume:"))
		}
	}
	return list
}

// parseVolumes reads "front-left: 65536 / 100% / 0.00 dB,   front-right: ..."
// (or "mono: 65536 / 100% / 0.00 dB") into the raw volume of each channel.
func parseVolumes(s string) []int {
	var vols []int
	for _, ch := range strings.Split(s, ",") {
		_, v, ok := strings.Cut(ch, ":")
		if !ok {
			continue
		}
		raw, _, _ := strings.Cut(strings.TrimSpace(v), " ")
		if n, err := strconv.Atoi(raw); err == nil {
			vols = append(vols, n)
		}
	}
	return vols
}

// parseClients reads `LC_ALL=C pactl list clients`: each client's properties by index.
func parseClients(out []byte) map[int]map[string]string {
	clients := map[int]map[string]string{}
	var cur map[string]string
	sc := bufio.NewScanner(bytes.NewReader(out))
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		line := sc.Text()
		if rest, ok := strings.CutPrefix(line, "Client #"); ok {
			cur = nil
			if n, err := strconv.Atoi(strings.TrimSpace(rest)); err == nil {
				cur = map[string]string{}
				clients[n] = cur
			}
			continue
		}
		if cur != nil && strings.HasPrefix(line, "\t\t") {
			if k, v, ok := parseProp(strings.TrimSpace(line)); ok {
				cur[k] = v
			}
		}
	}
	return clients
}

// parseProp reads one property line: key = "value".
func parseProp(t string) (string, string, bool) {
	k, v, ok := strings.Cut(t, " = ")
	if !ok {
		return "", "", false
	}
	v = strings.TrimSpace(v)
	if len(v) >= 2 && v[0] == '"' && v[len(v)-1] == '"' {
		v = v[1 : len(v)-1]
	}
	return strings.TrimSpace(k), v, true
}
