package arm

import (
	"compress/gzip"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"testing"
)

// The cases in testdata/arm946.bin.gz are single instructions run on
// Unicorn's ARM946 model (tools/pixelds/armcases.py makes them).

const (
	tCode  = 0x1000
	tData  = 0x10000
	tDSize = 0x400
)

type flatBus struct{ mem []byte }

func (b *flatBus) in(a uint32) bool      { return int(a) < len(b.mem) }
func (b *flatBus) Read8(a uint32) uint32 { return uint32(b.mem[a]) }
func (b *flatBus) Read16(a uint32) uint32 {
	a &^= 1
	return uint32(binary.LittleEndian.Uint16(b.mem[a:]))
}
func (b *flatBus) Read32(a uint32) uint32 { a &^= 3; return binary.LittleEndian.Uint32(b.mem[a:]) }
func (b *flatBus) Write8(a, v uint32)     { b.mem[a] = byte(v) }
func (b *flatBus) Write16(a, v uint32) {
	a &^= 1
	binary.LittleEndian.PutUint16(b.mem[a:], uint16(v))
}
func (b *flatBus) Write32(a, v uint32) {
	a &^= 3
	binary.LittleEndian.PutUint32(b.mem[a:], v)
}
func (b *flatBus) Fetch16(a uint32) uint32 { return b.Read16(a) }
func (b *flatBus) Fetch32(a uint32) uint32 { return b.Read32(a) }

func lcg(seed uint32) []byte {
	out := make([]byte, tDSize)
	for i := range out {
		seed = seed*1664525 + 1013904223
		out[i] = byte(seed >> 24)
	}
	return out
}

func fnv(b []byte) uint32 {
	h := uint32(0x811c9dc5)
	for _, x := range b {
		h = (h ^ uint32(x)) * 0x01000193
	}
	return h
}

func TestARM946(t *testing.T) {
	f, err := os.Open("testdata/arm946.bin.gz")
	if err != nil {
		t.Skip(err)
	}
	defer f.Close()
	z, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	data, err := io.ReadAll(z)
	if err != nil {
		t.Fatal(err)
	}
	const inSize = 1 + 4 + 64 + 4 + 4
	const outSize = 60 + 4 + 4 + 4
	fails := 0
	n := 0
	for p := 0; p+inSize+outSize <= len(data); p += inSize + outSize {
		n++
		in := data[p : p+inSize]
		out := data[p+inSize : p+inSize+outSize]
		thumb := in[0] == 1
		op := binary.LittleEndian.Uint32(in[1:])
		var regs [16]uint32
		for i := range regs {
			regs[i] = binary.LittleEndian.Uint32(in[5+4*i:])
		}
		cpsr := binary.LittleEndian.Uint32(in[69:])
		seed := binary.LittleEndian.Uint32(in[73:])

		bus := &flatBus{mem: make([]byte, 0x20000)}
		copy(bus.mem[tData:], lcg(seed))
		if thumb {
			binary.LittleEndian.PutUint16(bus.mem[tCode:], uint16(op))
		} else {
			binary.LittleEndian.PutUint32(bus.mem[tCode:], op)
		}
		c := New(bus, V5TE)
		c.SetCPSR(cpsr)
		c.R = regs
		c.next = tCode
		c.Step()

		var want [15]uint32
		for i := range want {
			want[i] = binary.LittleEndian.Uint32(out[4*i:])
		}
		wantPC := binary.LittleEndian.Uint32(out[60:])
		wantCPSR := binary.LittleEndian.Uint32(out[64:])
		wantMem := binary.LittleEndian.Uint32(out[68:])

		var bad []string
		for i := 0; i < 15; i++ {
			if c.R[i] != want[i] {
				bad = append(bad, fmt.Sprintf("r%d=%08x want %08x", i, c.R[i], want[i]))
			}
		}
		if c.next != wantPC {
			bad = append(bad, fmt.Sprintf("pc=%08x want %08x", c.next, wantPC))
		}
		if c.CPSR&0xf8000020 != wantCPSR&0xf8000020 {
			bad = append(bad, fmt.Sprintf("cpsr=%08x want %08x", c.CPSR, wantCPSR))
		}
		if fnv(bus.mem[tData:tData+tDSize]) != wantMem {
			bad = append(bad, "memory differs")
		}
		if len(bad) > 0 {
			fails++
			if fails <= 25 {
				kind := "arm"
				if thumb {
					kind = "thumb"
				}
				t.Errorf("%s %08x (cpsr in %08x): %v", kind, op, cpsr, bad)
			}
		}
	}
	if fails > 0 {
		t.Errorf("%d of %d cases differ", fails, n)
	}
}
