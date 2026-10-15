package arm7

import (
	"bytes"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
)

type flatBus struct{ mem []byte }

func (b *flatBus) Read8(a uint32) uint32 { return uint32(b.mem[a&0xffff]) }
func (b *flatBus) Read16(a uint32) uint32 {
	a &^= 1
	return b.Read8(a) | b.Read8(a+1)<<8
}
func (b *flatBus) Read32(a uint32) uint32 {
	a &^= 3
	return b.Read8(a) | b.Read8(a+1)<<8 | b.Read8(a+2)<<16 | b.Read8(a+3)<<24
}
func (b *flatBus) Fetch16(a uint32) uint32 { return b.Read16(a) }
func (b *flatBus) Fetch32(a uint32) uint32 { return b.Read32(a) }
func (b *flatBus) Write8(a, v uint32) {
	if a &= 0xffff; a >= 0x100 { // the vectors stay put
		b.mem[a] = byte(v)
	}
}
func (b *flatBus) Write16(a, v uint32) {
	a &^= 1
	b.Write8(a, v)
	b.Write8(a+1, v>>8)
}
func (b *flatBus) Write32(a, v uint32) {
	a &^= 3
	b.Write8(a, v)
	b.Write8(a+1, v>>8)
	b.Write8(a+2, v>>16)
	b.Write8(a+3, v>>24)
}

// program makes 64K of mostly-valid random instructions: ARM words with the
// condition mostly "always", or Thumb halfwords; data registers point into
// the memory so loads and stores land somewhere.
func program(seed uint64, thumb bool) []byte {
	s := seed | 1
	rnd := func() uint32 {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		return uint32(s)
	}
	mem := make([]byte, 65536)
	for i := 0; i < len(mem); i += 4 {
		w := rnd()
		if !thumb {
			if rnd()%4 != 0 {
				w = w&0x0fffffff | 0xe0000000
			}
			// keep most branches short so the program stays in its area
			if w>>25&7 == 5 {
				w = w&0xff000000 | rnd()&0x3f
			}
		}
		binary.LittleEndian.PutUint32(mem[i:], w)
	}
	// exception vectors: return to the interrupted code (MOVS pc, lr / SUBS pc, lr, #4)
	for v := 0; v < 0x20; v += 4 {
		binary.LittleEndian.PutUint32(mem[v:], 0xe1b0f00e)
	}
	binary.LittleEndian.PutUint32(mem[0x18:], 0xe25ef004)
	binary.LittleEndian.PutUint32(mem[0x1c:], 0xe25ef004)
	binary.LittleEndian.PutUint32(mem[0x00:], 0xea00003e) // reset: b 0x100
	return mem
}

// TestMatchesJS compares every instruction's effect with web/emu/arm7.js
// (registers, status, program counter after each step, and memory at the end).
func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	emu, _ := filepath.Abs("../../web/emu")
	ref, _ := filepath.Abs("testdata/armref.js")
	steps, runs := 20000, 60
	if testing.Short() {
		runs = 10
	}
	total := 0
	for run := 0; run < runs; run++ {
		thumb := run%2 == 1
		mem := program(uint64(run)*0x9e3779b97f4a7c15+7, thumb)
		img := filepath.Join(t.TempDir(), "mem.bin")
		os.WriteFile(img, mem, 0o644)
		th := "0"
		if thumb {
			th = "1"
		}
		cmd := exec.Command(node, ref, emu, img, strconv.Itoa(steps), th)
		cmd.Stderr = os.Stderr
		out, err := cmd.Output()
		if err != nil {
			t.Fatal(err)
		}
		bus := &flatBus{mem: append([]byte(nil), mem...)}
		c := New(bus)
		c.Reset()
		for i := 0; i < 13; i++ {
			c.R[i] = uint32(int32(i) * int32(-1640531527))
		}
		c.R[13] = 0x8000
		if thumb {
			c.T = 1
		}
		c.Branch(0x100)
		done := int(binary.LittleEndian.Uint32(out))
		out = out[4:]
		for st := 0; st < done; st++ {
			c.Step()
			o := out[st*72:]
			for i := 0; i < 16; i++ {
				if w := binary.LittleEndian.Uint32(o[i*4:]); c.R[i] != w {
					t.Fatalf("run %d (thumb %v) step %d: r%d = %08x, JS %08x (pc %08x)", run, thumb, st, i, c.R[i], w, c.PC)
				}
			}
			if w := binary.LittleEndian.Uint32(o[64:]); c.CPSR() != w {
				t.Fatalf("run %d step %d: CPSR %08x, JS %08x", run, st, c.CPSR(), w)
			}
			if w := binary.LittleEndian.Uint32(o[68:]); c.PC != w {
				t.Fatalf("run %d step %d: pc %08x, JS %08x", run, st, c.PC, w)
			}
		}
		if !bytes.Equal(bus.mem, out[done*72:]) {
			t.Fatalf("run %d: memory differs", run)
		}
		total += done
	}
	t.Logf("%d instructions identical (%d ARM and %d Thumb runs)", total, (runs+1)/2, runs/2)
}
