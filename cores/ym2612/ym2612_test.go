package ym2612

import (
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

type rng struct{ s uint64 }

func (r *rng) next() uint32 {
	r.s ^= r.s << 13
	r.s ^= r.s >> 7
	r.s ^= r.s << 17
	return uint32(r.s >> 16)
}
func (r *rng) n(k int) int { return int(r.next() % uint32(k)) }

// script makes a deterministic stream of register writes and ticks that
// covers the whole chip: every operator and channel register, key on / off,
// channel 3 special and CSM modes, the LFO, SSG-EG, the timers and the DAC.
func script(seed uint64, segments int) []uint32 {
	r := &rng{s: seed | 1}
	var s []uint32
	w := func(part, reg, v int) {
		s = append(s, 0, uint32(part*2), uint32(reg), 0, uint32(part*2+1), uint32(v&0xff))
	}
	for seg := 0; seg < segments; seg++ {
		if seg%97 == 50 {
			s = append(s, 3)
		}
		nw := 1 + r.n(30)
		for i := 0; i < nw; i++ {
			part := r.n(2)
			switch k := r.n(20); {
			case k < 8: // operator registers
				reg := 0x30 + r.n(0x70)
				v := int(r.next())
				if reg >= 0x40 && reg < 0x50 && r.n(2) == 0 {
					v &= 0x1f // audible levels
				}
				if reg >= 0x90 && reg < 0xa0 && r.n(3) != 0 {
					v = 0
				}
				w(part, reg, v)
			case k < 11: // frequency
				ch := r.n(3)
				w(part, 0xa4+ch, r.n(0x40))
				w(part, 0xa0+ch, int(r.next()))
			case k < 12: // channel 3 special frequencies
				w(0, 0xac+r.n(4), r.n(0x40))
				w(0, 0xa8+r.n(4), int(r.next()))
			case k < 14:
				w(part, 0xb0+r.n(4), int(r.next()))
				w(part, 0xb4+r.n(4), int(r.next()))
			case k < 17: // key on / off
				w(0, 0x28, int(r.next()))
			case k < 18:
				w(0, 0x22, int(r.next()&0xf))
			case k < 19: // timers and mode
				w(0, 0x24+r.n(3), int(r.next()))
				w(0, 0x27, int(r.next()))
			default: // DAC, odd globals
				reg := 0x20 + r.n(16)
				w(part, reg, int(r.next()))
				s = append(s, 0, uint32(r.n(4)), uint32(r.next()&0xff)) // a bare write to any port
			}
		}
		s = append(s, 2, 1, uint32(1+r.n(1500)), 2)
	}
	return s
}

func runGo(y *YM2612, s []uint32) []int32 {
	var out []int32
	for k := 0; k < len(s); {
		switch s[k] {
		case 0:
			y.Write(int(s[k+1]), uint8(s[k+2]))
			k += 3
		case 1:
			for i := uint32(0); i < s[k+1]; i++ {
				y.Tick()
				out = append(out, int32(y.Out[0]), int32(y.Out[1]))
			}
			k += 2
		case 2:
			out = append(out, int32(y.ReadStatus()))
			k++
		case 3:
			y.Reset()
			k++
		}
	}
	return out
}

func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	segs := 3000
	if testing.Short() {
		segs = 300
	}
	for _, seed := range []uint64{1, 2612, 0xfeed} {
		s := script(seed, segs)
		b := make([]byte, 0, len(s)*4)
		for _, v := range s {
			b = binary.LittleEndian.AppendUint32(b, v)
		}
		file := filepath.Join(t.TempDir(), "script.bin")
		os.WriteFile(file, b, 0o644)
		emu, _ := filepath.Abs("../../web/emu")
		js, err := exec.Command(node, "testdata/ymref.js", emu, file).Output()
		if err != nil {
			t.Fatal(err)
		}
		gov := runGo(New(), s)
		if len(js) != len(gov)*4 {
			t.Fatalf("seed %d: %d values, JS %d", seed, len(gov), len(js)/4)
		}
		nonzero := 0
		for i, v := range gov {
			jv := int32(binary.LittleEndian.Uint32(js[i*4:]))
			if v != jv {
				t.Fatalf("seed %d: value %d: go %d js %d", seed, i, v, jv)
			}
			if v != 0 {
				nonzero++
			}
		}
		t.Logf("seed %d: %d values identical (%d not silent)", seed, len(gov), nonzero)
	}
}

func TestStateRoundTrip(t *testing.T) {
	s := script(7, 400)
	half := 0 // cut at a tick command half way
	for k := 0; k < len(s); {
		if k >= len(s)/2 && s[k] == 1 {
			half = k
			break
		}
		k += map[uint32]int{0: 3, 1: 2, 2: 1, 3: 1}[s[k]]
	}
	a := New()
	runGo(a, s[:half])
	st := a.AppendState(nil)
	want := runGo(a, s[half:])
	b := New()
	rest, err := b.LoadState(append(st, 9))
	if err != nil || len(rest) != 1 {
		t.Fatalf("LoadState: %v (%d left)", err, len(rest))
	}
	got := runGo(b, s[half:])
	for i := range want {
		if want[i] != got[i] {
			t.Fatalf("value %d after loading: %d, want %d", i, got[i], want[i])
		}
	}
	if _, err := b.LoadState(st[:len(st)-1]); err == nil {
		t.Fatal("a short state loaded")
	}
}

func BenchmarkTick(b *testing.B) {
	y := New()
	runGo(y, script(3, 50))
	for i := 0; i < b.N; i++ {
		y.Tick()
	}
}
