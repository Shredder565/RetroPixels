package main

// The native app's Nintendo 64 core (cores/n64) against the web player's:
// web/emu/n64.js, run in Node, talking to this program's /api/n64/ws exactly
// as it does in RetroPixels.exe, and the native core, on the same games and
// the same controls (buttons, sticks, controllers plugged in and out, a reset,
// a save state fetched and loaded back). Every frame's picture and every
// sound sample must match, and so must the battery save and the save state.
//
// N64_COMPARE_LIST is a file of lines "<rom> [frames]" (default 3000 frames);
// N64_COMPARE_OUT keeps the results (and a PNG of each game's last frame).

import (
	"bufio"
	"bytes"
	"compress/flate"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"math"
	"math/rand"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
	n64core "retropixels/cores/n64"
)

type n64ScriptFrame struct {
	Conn  int       `json:"conn"`
	Reset bool      `json:"reset,omitempty"`
	Pads  [4][5]any `json:"pads"`
	bits  [4]uint32 // (the same, typed)
	axes  [4][4]float32
}

type n64Script struct {
	SRAM    *string          `json:"sram"`
	Frames  []n64ScriptFrame `json:"frames"`
	StateAt int              `json:"stateAt"`
	LoadAt  int              `json:"loadAt"`
}

// n64MakeScript: random controls that change every few frames, Start held
// early on, every stick value that matters to n64.js (the dead zone, the
// C-button threshold, all the way) and some in between, controllers plugged
// in and out, and a reset two thirds of the way.
func n64MakeScript(seed int64, n int) *n64Script {
	r := rand.New(rand.NewSource(seed))
	vals := []float32{0, 0.1, 0.15, 0.149, 0.2, 0.5, 0.501, 0.7, 0.999, 1, 1.2}
	axis := func() float32 {
		var v float32
		if r.Intn(3) == 0 {
			v = float32(r.Float64())
		} else {
			v = vals[r.Intn(len(vals))]
		}
		if r.Intn(2) == 0 {
			v = -v
		}
		return v
	}
	s := &n64Script{StateAt: n / 3, LoadAt: n / 2}
	var cur n64ScriptFrame
	cur.Conn = 1
	for i := 0; i < n; i++ {
		if i%7 == 0 {
			for p := 0; p < 4; p++ {
				var b uint32
				for k := 0; k < 18; k++ {
					if r.Intn(6) == 0 {
						b |= 1 << k
					}
				}
				cur.bits[p] = b
				for a := range cur.axes[p] {
					if r.Intn(2) == 0 {
						cur.axes[p][a] = axis()
					}
				}
			}
			if r.Intn(10) == 0 {
				cur.Conn = r.Intn(5)
			}
		}
		f := cur
		if i >= 20 && i < 30 {
			f.bits[0] |= 1 << 3 // Start
		}
		f.Reset = i == n*2/3
		for p := 0; p < 4; p++ {
			// (float64 of the float32, so Node gets exactly the native value)
			f.Pads[p] = [5]any{f.bits[p], float64(f.axes[p][0]), float64(f.axes[p][1]), float64(f.axes[p][2]), float64(f.axes[p][3])}
		}
		s.Frames = append(s.Frames, f)
	}
	return s
}

func n64Inflate(t *testing.T, b []byte) []byte {
	t.Helper()
	if len(b) < 4 || string(b[:4]) != "RPN1" {
		t.Fatalf("state %q", b[:min(len(b), 4)])
	}
	raw, err := io.ReadAll(flate.NewReader(bytes.NewReader(b[4:])))
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestN64NativeMatchesWebPlayer(t *testing.T) {
	list := os.Getenv("N64_COMPARE_LIST")
	if list == "" {
		t.Skip("N64_COMPARE_LIST not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	lf, err := os.ReadFile(list)
	if err != nil {
		t.Fatal(err)
	}
	token = "test-token"
	srv := httptest.NewServer(newMux())
	defer srv.Close()
	addr := strings.TrimPrefix(srv.URL, "http://")
	hostOK[addr] = true
	outRoot := os.Getenv("N64_COMPARE_OUT")
	if outRoot == "" {
		outRoot = t.TempDir()
	}
	sc := bufio.NewScanner(bytes.NewReader(lf))
	for n := 0; sc.Scan(); n++ {
		fields := strings.Fields(sc.Text())
		if len(fields) == 0 || strings.HasPrefix(fields[0], "#") {
			continue
		}
		romPath, frames := fields[0], 3000
		if len(fields) > 1 {
			frames, _ = strconv.Atoi(fields[1])
		}
		var sram []byte
		if len(fields) > 2 { // a battery save to start with
			if sram, err = os.ReadFile(fields[2]); err != nil {
				t.Fatal(err)
			}
		}
		t.Run(filepath.Base(romPath), func(t *testing.T) {
			n64CompareOne(t, node, addr, romPath, frames, int64(n+1), sram, filepath.Join(outRoot, fmt.Sprintf("%02d-%s", n, filepath.Base(romPath))))
		})
	}
}

func n64CompareOne(t *testing.T, node, addr, romPath string, frames int, seed int64, sram []byte, out string) {
	rom, err := os.ReadFile(romPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(out, 0o755); err != nil {
		t.Fatal(err)
	}
	s := n64MakeScript(seed, frames)
	if sram != nil {
		b := base64.StdEncoding.EncodeToString(sram)
		s.SRAM = &b
	}
	js, _ := json.Marshal(s)
	scriptPath := filepath.Join(out, "script.json")
	os.WriteFile(scriptPath, js, 0o644)
	n64js, _ := filepath.Abs("web/emu/n64.js")
	drive, _ := filepath.Abs("testdata/n64native/drive.js")
	cmd := exec.Command(node, drive, addr, token, n64js, romPath, scriptPath, out)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		t.Fatalf("web player: %v", err)
	}

	// the native core, driven as the native app drives it
	c, err := n64core.New(rom, core.Options{SampleRate: 48000, SRAM: sram})
	if err != nil {
		t.Fatal(err)
	}
	an := c.(core.Analog)
	conn := c.(interface{ SetConnected(int) })
	webState, _ := os.ReadFile(filepath.Join(out, "state.bin"))
	var lines []string
	var audio []float32
	var last []byte
	for i, f := range s.Frames {
		if i == s.StateAt {
			st, err := c.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(n64Inflate(t, st), n64Inflate(t, webState)) {
				t.Errorf("frame %d: the save state differs from the web player's", i)
			}
		}
		if i == s.LoadAt {
			if err := c.LoadState(webState); err != nil {
				t.Fatal(err)
			}
		}
		conn.SetConnected(f.Conn)
		for p := 0; p < 4; p++ {
			c.SetInput(p, f.bits[p])
			for a := 0; a < 4; a++ {
				an.SetAnalog(p, a, f.axes[p][a])
			}
		}
		if f.Reset {
			c.Reset()
		}
		c.RunFrame()
		w, h := c.Width(), c.Height()
		px := make([]byte, w*h*4)
		for k, p := range c.Screen() {
			binary.LittleEndian.PutUint32(px[k*4:], 0xff000000|(p&0xff)<<16|p&0xff00|(p>>16)&0xff)
		}
		sum := sha256.Sum256(px)
		lines = append(lines, fmt.Sprintf("%d %d %s", w, h, hex.EncodeToString(sum[:])))
		audio = append(audio, c.TakeAudio()...)
		last = px
	}

	web, _ := os.ReadFile(filepath.Join(out, "frames.txt"))
	wl := strings.Split(strings.TrimSpace(string(web)), "\n")
	bad := 0
	for i := range lines {
		if i >= len(wl) || wl[i] != lines[i] {
			if bad < 5 {
				t.Errorf("frame %d: native %s, web %s", i, lines[i], wl[min(i, len(wl)-1)])
			}
			bad++
		}
	}
	wa, _ := os.ReadFile(filepath.Join(out, "audio.f32"))
	if len(wa)/4 != len(audio) {
		t.Errorf("sound: native %d samples, web %d", len(audio), len(wa)/4)
	}
	maxDiff, nonzero := 0.0, 0
	for i := 0; i < len(audio) && i*4+4 <= len(wa); i++ {
		v := math.Float32frombits(binary.LittleEndian.Uint32(wa[i*4:]))
		if d := math.Abs(float64(v - audio[i])); d > maxDiff {
			maxDiff = d
		}
		if audio[i] != 0 {
			nonzero++
		}
	}
	if maxDiff >= 1e-4 {
		t.Errorf("sound differs by up to %g", maxDiff)
	}
	ws, _ := os.ReadFile(filepath.Join(out, "sram.bin"))
	if ns := c.SRAM(); !bytes.Equal(ns, ws) {
		t.Errorf("battery save: native %d bytes, web %d, differ", len(ns), len(ws))
	}
	// the last picture, to look at
	if len(lines) > 0 {
		w, h := c.Width(), c.Height()
		img := image.NewRGBA(image.Rect(0, 0, w, h))
		for k := 0; k < w*h; k++ {
			img.Set(k%w, k/w, color.RGBA{last[k*4], last[k*4+1], last[k*4+2], 255})
		}
		if f, err := os.Create(filepath.Join(out, "last.png")); err == nil {
			png.Encode(f, img)
			f.Close()
		}
	}
	t.Logf("%d frames (%d mismatched), %d sound samples (%d not silent, max diff %g), battery save %d bytes, last picture %s",
		len(lines), bad, len(audio), nonzero, maxDiff, len(ws), lines[len(lines)-1][:16])
}
