package mcs48

// Compares the Go MCS-48 with the web one (web/emu/mcs48.js, run in Node)
// step by step on random code: every register, the cycles, the RAM and every
// port / external memory / expander access. Skipped without node.

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
	rom         []byte
	step, count int
	sum         uint32
}

func (b *fuzzBus) mix(v int)           { b.sum = b.sum*33 + uint32(v) }
func (b *fuzzBus) ROM(a int) uint8     { return b.rom[a] }
func (b *fuzzBus) ReadX(a uint8) uint8 { v := int(a)*7 + b.count; b.count++; return uint8(v) }
func (b *fuzzBus) WriteX(a, v uint8)   { b.mix(0x10000 | int(a)<<8 | int(v)) }
func (b *fuzzBus) OutP(p int, v uint8) { b.mix(0x20000 | p<<8 | int(v)) }
func (b *fuzzBus) InP(p int) uint8     { v := p*13 + b.count*3; b.count++; return uint8(v) }
func (b *fuzzBus) T0() bool            { return (b.step>>3)&1 == 1 }
func (b *fuzzBus) T1() bool            { return (b.step*7>>2)&1 == 1 }
func (b *fuzzBus) IntLine() bool       { return b.step%97 < 5 }

type expBus struct{ *fuzzBus }

func (b expBus) Expander(op, port int, v uint8) uint8 {
	b.mix(0x30000 | op<<12 | port<<8 | int(v))
	r := op*5 + port + int(v) + b.count
	b.count++
	return uint8(r)
}

func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	steps := 200000
	emu, _ := filepath.Abs("../../web/emu")
	script, _ := filepath.Abs("testdata/fuzz.js")
	for seed := uint64(1); seed <= 8; seed++ {
		s := seed*0x9e3779b97f4a7c15 | 1
		rom := make([]byte, 0x1000)
		for i := range rom {
			s ^= s << 13
			s ^= s >> 7
			s ^= s << 17
			rom[i] = byte(s >> 24)
		}
		p := filepath.Join(t.TempDir(), "rom.bin")
		os.WriteFile(p, rom, 0o644)
		exp := seed%2 == 0
		cmd := exec.Command(node, script, emu, p, strconv.Itoa(steps), map[bool]string{false: "0", true: "1"}[exp])
		cmd.Stderr = os.Stderr
		stdout, _ := cmd.StdoutPipe()
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		r := bufio.NewReaderSize(stdout, 1<<20)
		fb := &fuzzBus{rom: rom}
		var c *CPU
		if exp {
			c = New(expBus{fb}, 64)
		} else {
			c = New(fb, 64)
		}
		rec := make([]byte, 21*4+64)
		for fb.step = 0; fb.step < steps; fb.step++ {
			cy := c.Step()
			if _, err := io.ReadFull(r, rec); err != nil {
				t.Fatalf("seed %d step %d: %v", seed, fb.step, err)
			}
			got := append(c.regs(), cy, int(int32(fb.sum)))
			for k, v := range got {
				js := int(int32(binary.LittleEndian.Uint32(rec[k*4:])))
				if v != js {
					t.Fatalf("seed %d step %d: value %d is %#x, JS %#x (pc %03x)", seed, fb.step, k, v, js, c.PC)
				}
			}
			if string(c.RAM) != string(rec[21*4:]) {
				t.Fatalf("seed %d step %d: RAM differs", seed, fb.step)
			}
		}
		stdout.Close()
		cmd.Wait()
		st := c.AppendState(nil)
		c2 := New(fb, 64)
		if rest, err := c2.LoadState(st); err != nil || len(rest) != 0 || string(c2.AppendState(nil)) != string(st) {
			t.Fatal("state round trip")
		}
	}
	t.Logf("8 programs x %d steps identical", steps)
}
