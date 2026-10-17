package cpu6502

import "testing"

// irqBus raises the IRQ line on cycle at.
type irqBus struct {
	mem [65536]uint8
	cpu *CPU
	at  int64
}

func (b *irqBus) Read(a uint16) uint8 {
	if b.cpu.Cycles >= b.at {
		b.cpu.IRQLine = true
	}
	return b.mem[a]
}
func (b *irqBus) Write(a uint16, v uint8) { b.mem[a] = v }

// steps runs prog at $0200 with the IRQ raised from the bus access that
// makes cycle count at, and returns the PCs before each step until the IRQ
// handler ($0300) is reached (the last step is the interrupt).
func steps(t *testing.T, opts Options, prog []uint8, at int64) []uint16 {
	b := &irqBus{at: 1 << 62}
	b.mem[0xfffc], b.mem[0xfffd] = 0x00, 0x02
	b.mem[0xfffe], b.mem[0xffff] = 0x00, 0x03
	copy(b.mem[0x200:], prog)
	c := New(b, opts)
	b.cpu = c
	c.Reset()
	c.Cycles, b.at = 0, at
	var pcs []uint16
	for i := 0; i < 20 && c.PC != 0x300; i++ {
		pcs = append(pcs, c.PC)
		c.Step()
	}
	if c.PC != 0x300 {
		t.Fatalf("no interrupt: %x", pcs)
	}
	return pcs
}

func TestPollAtBoundary(t *testing.T) {
	cli := []uint8{0x58, 0xea, 0xea, 0xea, 0xea}
	// CLI with the line already up: the interrupt waits an instruction in
	// both modes.
	for _, o := range []Options{{}, {PollAtBoundary: true}} {
		if pcs := steps(t, o, cli, 0); len(pcs) != 3 {
			t.Errorf("%+v: interrupt after %x", o, pcs)
		}
	}
	// CLI; CLC; BCC +0 (taken, same page: accesses 5-7); the line goes up
	// on the branch's last access. The chip looked before it, so its
	// interrupt waits for the next instruction; at a boundary it doesn't.
	br := []uint8{0x58, 0x18, 0x90, 0x00, 0xea, 0xea, 0xea}
	if pcs := steps(t, Options{}, br, 7); len(pcs) != 5 {
		t.Errorf("chip: interrupt after %x", pcs)
	}
	if pcs := steps(t, Options{PollAtBoundary: true}, br, 7); len(pcs) != 4 {
		t.Errorf("boundary: interrupt after %x", pcs)
	}
}
