package snes

import (
	"bytes"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"testing"

	"retropixels/core"
)

var traceROMs = []string{"bgtest-direct.sfc", "bgtest-table.sfc", "bgtest-wram.sfc"}

func traceROM(t *testing.T, name string) []byte {
	b, err := os.ReadFile(filepath.Join("..", "..", "tools", "snes-debugger", "testroms", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func runTraced(t *testing.T, rom []byte, frames int) *Capture {
	c, err := New(rom, core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	s := c.(*SNES)
	s.EnableTrace()
	for i := 0; i < frames; i++ {
		s.RunFrame()
		s.TakeAudio()
	}
	cp, err := s.DebugCapture()
	if err != nil {
		t.Fatal(err)
	}
	return cp
}

// TestTraceRecords: the test programs' transfers are found with their ROM
// sources, and each VRAM / CGRAM / WRAM byte knows which transfer wrote it
// and from where.
func TestTraceRecords(t *testing.T) {
	cp := runTraced(t, traceROM(t, "bgtest-wram.sfc"), 5)
	recs := cp.Meta["records"].([]DMARecord)
	if len(recs) != 5 {
		t.Fatalf("%d records, want 5: %+v", len(recs), recs)
	}
	// 0: ROM $01:8000 -> WRAM $7F:0000; 2: WRAM $7F:0000 -> VRAM $0000
	if recs[0].Src != 0x018000 || recs[0].WMAdd != 0x10000 || recs[0].BBAD != 0x80 {
		t.Errorf("record 0 = %+v", recs[0])
	}
	if recs[2].Src != 0x7f0000 || recs[2].VAddr != 0 || recs[2].BBAD != 0x18 {
		t.Errorf("record 2 = %+v", recs[2])
	}
	i32 := func(b []byte, i int) int32 {
		return int32(uint32(b[i*4]) | uint32(b[i*4+1])<<8 | uint32(b[i*4+2])<<16 | uint32(b[i*4+3])<<24)
	}
	vs, vf := cp.Files["vram_src.bin"], cp.Files["vram_from.bin"]
	if i32(vs, 1) != 2 || i32(vf, 1) != 0x7f0001 {
		t.Errorf("VRAM byte 1: writer %d from %#x", i32(vs, 1), i32(vf, 1))
	}
	ws, wf := cp.Files["wram_src.bin"], cp.Files["wram_from.bin"]
	if i32(ws, 0x10005) != 0 || i32(wf, 0x10005) != 0x018005 {
		t.Errorf("WRAM $7F:0005: writer %d from %#x", i32(ws, 0x10005), i32(wf, 0x10005))
	}
	cs, cf := cp.Files["cgram_src.bin"], cp.Files["cgram_from.bin"]
	if i32(cs, 3) != 4 || i32(cf, 3) != 0x00f003 {
		t.Errorf("CGRAM byte 3: writer %d from %#x", i32(cs, 3), i32(cf, 3))
	}
	// the CPU's own writes: its address (-16 - PC)
	if w := i32(ws, 0x0100); w > -16 && w != -1 {
		t.Errorf("WRAM $0100 writer %d", w)
	}
}

// TestTraceMatchesJS: the web core's tracer (web/emu/snes.js) makes the same
// capture as this one.
func TestTraceMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	emu, _ := filepath.Abs(filepath.Join("..", "..", "web", "emu"))
	for _, name := range traceROMs {
		t.Run(name, func(t *testing.T) {
			rom := filepath.Join("..", "..", "tools", "snes-debugger", "testroms", name)
			dir := t.TempDir()
			cmd := exec.Command(node, filepath.Join("testdata", "snescap.js"), emu, rom, "8", dir)
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("node: %v\n%s", err, out)
			}
			cp := runTraced(t, traceROM(t, name), 8)
			for n, b := range cp.Files {
				js, err := os.ReadFile(filepath.Join(dir, n))
				if err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(js, b) {
					t.Errorf("%s differs", n)
				}
			}
			goMeta, _ := json.Marshal(cp.Meta)
			var a, b any
			json.Unmarshal(goMeta, &a)
			jsMeta, _ := os.ReadFile(filepath.Join(dir, "capture.json"))
			json.Unmarshal(jsMeta, &b)
			if !reflect.DeepEqual(a, b) {
				t.Errorf("capture.json differs:\ngo %s\njs %s", goMeta, jsMeta)
			}
		})
	}
}
