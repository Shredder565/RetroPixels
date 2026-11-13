package arm

import (
	"compress/gzip"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"testing"
)

// The cases in testdata/arm11.bin.gz are ARMv6 media and VFP instructions
// run on Unicorn's ARM11 MPCore model (tools/pixelds/arm11cases.py).
func TestARM11(t *testing.T) {
	f, err := os.Open("testdata/arm11.bin.gz")
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
	const inSize = 4 + 64 + 4 + 128 + 4 + 4
	const outSize = 60 + 4 + 4 + 128 + 4 + 4
	u32 := func(b []byte, i int) uint32 { return binary.LittleEndian.Uint32(b[4*i:]) }
	fails, n := 0, 0
	for p := 0; p+inSize+outSize <= len(data); p += inSize + outSize {
		n++
		in, out := data[p:p+inSize], data[p+inSize:p+inSize+outSize]
		op := u32(in, 0)
		bus := &flatBus{mem: make([]byte, 0x20000)}
		binary.LittleEndian.PutUint32(bus.mem[tCode:], op)
		c := New(bus, V6K)
		c.SetCPSR(u32(in, 17))
		for i := 0; i < 16; i++ {
			c.R[i] = u32(in, 1+i)
		}
		for i := 0; i < 32; i++ {
			c.fp.S[i] = u32(in, 18+i)
		}
		c.fp.FPSCR = binary.LittleEndian.Uint32(in[200:])
		copy(bus.mem[tData:], lcg(binary.LittleEndian.Uint32(in[204:])))
		c.next = tCode
		c.Step()

		var bad []string
		for i := 0; i < 15; i++ {
			if w := u32(out, i); c.R[i] != w {
				bad = append(bad, fmt.Sprintf("r%d=%08x want %08x", i, c.R[i], w))
			}
		}
		if w := u32(out, 15); c.next != w {
			bad = append(bad, fmt.Sprintf("pc=%08x want %08x", c.next, w))
		}
		if w := u32(out, 16); c.CPSR&0xf80f0000 != w&0xf80f0000 {
			bad = append(bad, fmt.Sprintf("cpsr=%08x want %08x", c.CPSR, w))
		}
		for i := 0; i < 32; i++ {
			if w := u32(out, 17+i); c.fp.S[i] != w {
				bad = append(bad, fmt.Sprintf("s%d=%08x want %08x", i, c.fp.S[i], w))
			}
		}
		if w := u32(out, 49); c.fp.FPSCR&0xf0000000 != w&0xf0000000 {
			bad = append(bad, fmt.Sprintf("fpscr=%08x want %08x", c.fp.FPSCR, w))
		}
		if fnv(bus.mem[tData:tData+tDSize]) != u32(out, 50) {
			bad = append(bad, "memory differs")
		}
		if len(bad) > 0 {
			fails++
			if fails <= 30 {
				t.Errorf("%08x (fpscr %08x): %v", op, binary.LittleEndian.Uint32(in[200:]), bad)
			}
		}
	}
	if fails > 0 {
		t.Errorf("%d of %d cases differ", fails, n)
	}
}
