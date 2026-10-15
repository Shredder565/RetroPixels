package i8080

// Compares the Go 8080 with the web one (web/emu/i8080.js, run in Node) step
// by step on random code: every register, the cycles, every port write and
// every memory write. Skipped without node.

import (
	"bufio"
	"encoding/binary"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
)

type fuzzBus struct {
	mem            [0x10000]uint8
	inCount        int
	outSum, memSum uint32
}

func (b *fuzzBus) Read(a uint16) uint8 { return b.mem[a] }
func (b *fuzzBus) Write(a uint16, v uint8) {
	b.mem[a] = v
	b.memSum = b.memSum*31 + uint32(a)*256 + uint32(v)
}
func (b *fuzzBus) In(p uint8) uint8 {
	v := (int(p)*31 + b.inCount) & 0xff
	b.inCount++
	return uint8(v)
}
func (b *fuzzBus) Out(p, v uint8) { b.outSum = b.outSum*33 + uint32(p)*256 + uint32(v) }

func randomMemory(seed uint64) []byte {
	s := seed | 1
	m := make([]byte, 0x10000)
	for i := range m {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		m[i] = byte(s >> 24)
		if m[i] == 0x76 && s&0x300 != 0 { // fewer HLTs
			m[i] = 0
		}
	}
	return m
}

func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	steps := 300000
	emu, _ := filepath.Abs("../../web/emu")
	script, _ := filepath.Abs("testdata/fuzz.js")
	for seed := uint64(1); seed <= 8; seed++ {
		mem := randomMemory(seed * 0x9e3779b97f4a7c15)
		p := filepath.Join(t.TempDir(), "mem.bin")
		os.WriteFile(p, mem, 0o644)
		cmd := exec.Command(node, script, emu, p, strconv.Itoa(steps))
		cmd.Stderr = os.Stderr
		stdout, _ := cmd.StdoutPipe()
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		r := bufio.NewReaderSize(stdout, 1<<20)
		bus := &fuzzBus{}
		copy(bus.mem[:], mem)
		c := New(bus)
		rec := make([]byte, 16*4)
		for i := 0; i < steps; i++ {
			var cy int
			if i%50 == 49 {
				cy = c.Interrupt(uint8(0xc7 | ((i/50)&7)<<3))
			} else {
				cy = c.Step()
			}
			if i%1000 == 999 && c.Halted && !c.Inte {
				c.Halted = false
				c.PC = (c.PC + 0x1234) & 0xffff
			}
			if _, err := io.ReadFull(r, rec); err != nil {
				t.Fatalf("seed %d step %d: %v", seed, i, err)
			}
			got := []int{c.A, c.F, c.B, c.C, c.D, c.E, c.H, c.L, c.SP, c.PC, b2(c.Inte), b2(c.Halted), b2(c.EIDelay), cy, int(bus.outSum), int(bus.memSum)}
			for k, v := range got {
				js := int(binary.LittleEndian.Uint32(rec[k*4:]))
				if uint32(v) != uint32(js) {
					t.Fatalf("seed %d step %d: value %d is %#x, JS %#x (pc %04x)", seed, i, k, v, js, c.PC)
				}
			}
		}
		stdout.Close()
		cmd.Wait()
		// a save state round trip
		st := c.AppendState(nil)
		var c2 CPU
		if rest, err := c2.LoadState(st); err != nil || len(rest) != 0 || c2.AppendState(nil) == nil || string(c2.AppendState(nil)) != string(st) {
			t.Fatal("state round trip")
		}
	}
	t.Logf("8 programs x %d steps identical", steps)
}

func b2(b bool) int {
	if b {
		return 1
	}
	return 0
}
