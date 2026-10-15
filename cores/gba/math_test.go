package gba

import (
	"encoding/binary"
	"math"
	"os/exec"
	"path/filepath"
	"testing"
)

// TestMathMatchesJS checks the floating-point maths of the BIOS calls against
// the web core's (V8's) results: the sine table, ArcTan for every input, and
// ArcTan2 and MidiKey2Freq for pseudo-random inputs, and Math.pow over the
// range of MidiKey2Freq's exponents.
func TestMathMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	const n = 200000
	script, _ := filepath.Abs("testdata/mathref.js")
	b, err := exec.Command(node, script, "200000").Output()
	if err != nil {
		t.Fatal(err)
	}
	at := 0
	next := func() int32 { v := int32(binary.LittleEndian.Uint32(b[at:])); at += 4; return v }
	for i := 0; i < 256; i++ {
		if w := next(); w != sinTable[i] {
			t.Fatalf("sin[%d]: js %d go %d", i, w, sinTable[i])
		}
	}
	for v := uint32(0); v < 0x10000; v++ {
		if w := next(); w != arcTan(v) {
			t.Fatalf("ArcTan(%#x): js %d go %d", v, w, arcTan(v))
		}
	}
	s := uint32(0x2545f491)
	rnd := func() uint32 { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s }
	for i := 0; i < n; i++ {
		r0, r1 := rnd(), rnd()
		x, y := float64(int16(r0)), float64(int16(r1))
		a := jsAtan2(y, x)
		if a < 0 {
			a += 2 * math.Pi
		}
		got := int32(int64(jsRound(a/(2*math.Pi)*0x10000)) & 0xffff)
		if w := next(); w != got {
			t.Fatalf("ArcTan2(%d, %d): js %d go %d", int16(r0), int16(r1), w, got)
		}
	}
	for i := 0; i < n; i++ {
		f := rnd()
		f >>= rnd() & 15
		m1, m2 := uint32(0xffffffff), uint32(0xffffffff)
		if i&1 != 0 {
			m1 = 0xff
		}
		if i&2 != 0 {
			m2 = 0xff
		}
		r1 := rnd() & m1
		r2 := rnd() & m2
		e := (180 - float64(int32(r1)) - float64(int32(r2))/256) / 12
		got := toInt32(math.Floor(float64(f) / jsPow(2, e)))
		if w := next(); w != got {
			t.Fatalf("MidiKey2Freq(%d, %d, %d): js %d go %d", f, int32(r1), int32(r2), w, got)
		}
	}
	for r1 := -1024; r1 < 2048; r1++ {
		for r2 := 0; r2 < 256; r2++ {
			w := binary.LittleEndian.Uint64(b[at:])
			at += 8
			e := (180 - float64(r1) - float64(r2)/256) / 12
			if got := math.Float64bits(jsPow(2, e)); got != w {
				t.Fatalf("Math.pow(2, %v): js %v go %v", e, math.Float64frombits(w), math.Float64frombits(got))
			}
		}
	}
}
