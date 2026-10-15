package snes

import (
	"fmt"
	"math"
)

// NEC uPD77C25 (DSP-3, DSP-4, and DSP-1 / DSP-2 when their program is
// supplied) and uPD96050 (ST010, ST011) signal processors, instruction by
// instruction (web/emu/snes_necdsp.js). The programs inside these chips are
// the player's own dumps (core.Options.Firmware), or appended to the ROM.

const (
	ndOV0 = 1
	ndOV1 = 2
	ndZ   = 4
	ndC   = 8
	ndS0  = 16
	ndS1  = 32

	ndRQM        = 0x8000
	ndDRS        = 0x1000
	ndDRC        = 0x0400
	ndSRWritable = 0x6f83
)

type necModel struct {
	prog, data, ram, stack int
	dpMask, rpMask, klm    int
	pcMask                 int
	big                    bool
}

var (
	upd7725  = &necModel{prog: 2048, data: 1024, ram: 256, stack: 4, dpMask: 0xff, rpMask: 0x3ff, klm: 0x40, pcMask: 0x7ff}
	upd96050 = &necModel{prog: 16384, data: 2048, ram: 2048, stack: 16, dpMask: 0x7ff, rpMask: 0x7ff, klm: 0x400, pcMask: 0x3fff, big: true}
)

func (m *necModel) fwSize() int { return m.prog*3 + m.data*2 }

func validBranch(c int) bool {
	if c == 0x100 || c == 0x140 || c == 0x0b1 || c == 0x0b3 {
		return true
	}
	return c >= 0x080 && c <= 0x0be && c&1 == 0
}

type necFirmware struct {
	prog []uint32
	data []uint16
}

// parseFirmware splits a dump into program and data words, choosing the byte
// order that makes more of its jump instructions valid.
func parseFirmware(b []byte, m *necModel) *necFirmware {
	if len(b) != m.fwSize() {
		return nil
	}
	read := func(big bool) *necFirmware {
		fw := &necFirmware{prog: make([]uint32, m.prog), data: make([]uint16, m.data)}
		for i := range fw.prog {
			a, b1, c := uint32(b[i*3]), uint32(b[i*3+1]), uint32(b[i*3+2])
			if big {
				fw.prog[i] = a<<16 | b1<<8 | c
			} else {
				fw.prog[i] = c<<16 | b1<<8 | a
			}
		}
		d0 := m.prog * 3
		for i := range fw.data {
			a, b1 := uint16(b[d0+i*2]), uint16(b[d0+i*2+1])
			if big {
				fw.data[i] = a<<8 | b1
			} else {
				fw.data[i] = b1<<8 | a
			}
		}
		return fw
	}
	score := func(prog []uint32) int {
		s := 0
		for _, w := range prog {
			if w>>22 != 2 {
				continue
			}
			if validBranch(int(w>>13)&0x1ff) && (m.big || w&3 == 0) {
				s++
			} else {
				s--
			}
		}
		return s
	}
	le, be := read(false), read(true)
	if score(be.prog) > score(le.prog) {
		return be
	}
	return le
}

// ---------- the processor ----------

type necDSP struct {
	M     *necModel
	prog  []uint32
	drom  []uint16
	ram   []uint16
	stack []uint16

	pc, sp, dp, rp      int
	a, b, fa, fb        int
	tr, trb, k, l, m, n int
	dr, sr, si, so      int
	budget              int
}

func newNECDSP(m *necModel, fw *necFirmware) *necDSP {
	d := &necDSP{M: m, ram: make([]uint16, m.ram), stack: make([]uint16, m.stack)}
	if fw != nil {
		d.prog, d.drom = fw.prog, fw.data
	} else {
		d.prog, d.drom = make([]uint32, m.prog), make([]uint16, m.data)
	}
	d.reset()
	return d
}

func (d *necDSP) reset() {
	d.pc, d.sp, d.dp, d.rp = 0, 0, 0, 0
	d.a, d.b, d.fa, d.fb = 0, 0, 0, 0
	d.tr, d.trb, d.k, d.l, d.m, d.n = 0, 0, 0, 0, 0, 0
	d.dr, d.sr, d.si, d.so = 0, 0, 0, 0
	d.budget = 0
	for i := range d.stack {
		d.stack[i] = 0
	}
}

func (d *necDSP) hostReadSR() int { return d.sr >> 8 }

func (d *necDSP) hostReadDR() int {
	if d.sr&ndDRC != 0 {
		d.sr &^= ndRQM
		return d.dr & 0xff
	}
	if d.sr&ndDRS == 0 {
		d.sr |= ndDRS
		return d.dr & 0xff
	}
	d.sr &^= ndDRS | ndRQM
	return d.dr >> 8
}

func (d *necDSP) hostWriteDR(v int) {
	if d.sr&ndDRC != 0 {
		d.dr = d.dr&0xff00 | v
		d.sr &^= ndRQM
		return
	}
	if d.sr&ndDRS == 0 {
		d.dr = d.dr&0xff00 | v
		d.sr |= ndDRS
		return
	}
	d.dr = d.dr&0x00ff | v<<8
	d.sr &^= ndDRS | ndRQM
}

func (d *necDSP) run(n int) {
	d.budget += n
	for d.budget > 0 {
		d.budget--
		d.step()
	}
}

func (d *necDSP) step() {
	at := d.pc
	op := int(d.prog[at])
	d.pc = (at + 1) & d.M.pcMask
	switch op >> 22 {
	case 0:
		d.opALU(op)
	case 1:
		d.opALU(op)
		d.sp = (d.sp - 1) & (d.M.stack - 1)
		d.pc = int(d.stack[d.sp])
	case 2:
		d.opJump(op, at)
	default:
		d.moveTo((op>>6)&0xffff, op&15)
		d.multiply()
	}
}

func (d *necDSP) multiply() {
	p := s16(d.k) * s16(d.l) * 2
	d.m, d.n = (p>>16)&0xffff, p&0xffff
}

func (d *necDSP) source(s int) int {
	switch s {
	case 0:
		return d.trb
	case 1:
		return d.a
	case 2:
		return d.b
	case 3:
		return d.tr
	case 4:
		return d.dp
	case 5:
		return d.rp
	case 6:
		return int(d.drom[d.rp])
	case 7:
		if d.fa&ndS1 != 0 {
			return 0x8000
		}
		return 0x7fff
	case 8:
		d.sr |= ndRQM
		return d.dr
	case 9:
		return d.dr
	case 10:
		return d.sr
	case 11, 12:
		return d.si
	case 13:
		return d.k
	case 14:
		return d.l
	}
	return int(d.ram[d.dp])
}

func (d *necDSP) moveTo(v, dst int) {
	switch dst {
	case 0:
	case 1:
		d.a = v
	case 2:
		d.b = v
	case 3:
		d.tr = v
	case 4:
		d.dp = v & d.M.dpMask
	case 5:
		d.rp = v & d.M.rpMask
	case 6:
		d.dr = v
		d.sr |= ndRQM
	case 7:
		d.sr = d.sr&^ndSRWritable | v&ndSRWritable
	case 8, 9:
		d.so = v
	case 10:
		d.k = v
	case 11:
		d.k = v
		d.l = int(d.drom[d.rp])
	case 12:
		d.l = v
		d.k = int(d.ram[d.dp|d.M.klm])
	case 13:
		d.l = v
	case 14:
		d.trb = v
	default:
		d.ram[d.dp] = uint16(v)
	}
}

func (d *necDSP) opALU(op int) {
	pselect, fn, asl := (op>>20)&3, (op>>16)&15, (op>>15)&1
	idb := d.source((op >> 4) & 15)
	if fn != 0 {
		var p int
		switch pselect {
		case 0:
			p = int(d.ram[d.dp])
		case 1:
			p = idb
		case 2:
			p = d.m
		default:
			p = d.n
		}
		d.alu(fn, p, asl)
	}
	d.moveTo(idb, op&15)
	dpl := (op >> 13) & 3
	dp := d.dp
	switch dpl {
	case 1:
		dp = dp&^15 | (dp+1)&15
	case 2:
		dp = dp&^15 | (dp-1)&15
	case 3:
		dp &^= 15
	}
	dp ^= ((op >> 9) & 15) << 4
	d.dp = dp & d.M.dpMask
	if op&0x100 != 0 {
		d.rp = (d.rp - 1) & d.M.rpMask
	}
	d.multiply()
}

func (d *necDSP) alu(fn, p, asl int) {
	q := d.a
	of := d.fb
	if asl != 0 {
		q, of = d.b, d.fa
	}
	otherC := 0
	if of&ndC != 0 {
		otherC = 1
	}
	var r, carry, ov int
	arith := false
	switch fn {
	case 1:
		r = q | p
	case 2:
		r = q & p
	case 3:
		r = q ^ p
	case 4, 6, 8:
		s := p
		if fn == 8 {
			s = 1
		}
		full := q - s
		if fn == 6 {
			full -= otherC
		}
		r = full & 0xffff
		if full < 0 {
			carry = 1
		}
		ov = (q ^ s) & (q ^ r) & 0x8000
		arith = true
	case 5, 7, 9:
		s := p
		if fn == 9 {
			s = 1
		}
		full := q + s
		if fn == 7 {
			full += otherC
		}
		r = full & 0xffff
		carry = full >> 16
		ov = (q ^ r) & (s ^ r) & 0x8000
		arith = true
	case 10:
		r = ^q & 0xffff
	case 11:
		r = q>>1 | q&0x8000
		carry = q & 1
	case 12:
		r = (q<<1 | otherC) & 0xffff
		carry = q >> 15
	case 13:
		r = (q<<2 | 3) & 0xffff
	case 14:
		r = (q<<4 | 15) & 0xffff
	default:
		r = (q>>8 | q<<8) & 0xffff
	}
	f := d.fa
	if asl != 0 {
		f = d.fb
	}
	s0 := 0
	if r&0x8000 != 0 {
		s0 = ndS0
	}
	z := 0
	if r == 0 {
		z = ndZ
	}
	c := 0
	if carry != 0 {
		c = ndC
	}
	if arith {
		ov1 := f & ndOV1
		if ov != 0 {
			ov1 ^= ndOV1
		}
		f = s0 | z | c | ov1
		if ov != 0 {
			f |= ndOV0
		}
		if (ov1 != 0) != (s0 != 0) {
			f |= ndS1
		}
	} else {
		f = s0 | z | c
		if s0 != 0 {
			f |= ndS1
		}
	}
	if asl != 0 {
		d.b, d.fb = r, f
	} else {
		d.a, d.fa = r, f
	}
}

func (d *necDSP) opJump(op, at int) {
	brch, M := (op>>13)&0x1ff, d.M
	target := (op >> 2) & 0x7ff
	if M.big {
		target = at&0x2000 | (op&3)<<11 | (op>>2)&0x7ff
	}
	var take bool
	switch {
	case brch == 0x100:
		take = true
	case brch == 0x140:
		d.stack[d.sp] = uint16(d.pc)
		d.sp = (d.sp + 1) & (M.stack - 1)
		d.pc = target
		return
	case M.big && brch == 0x000:
		d.pc = d.so & M.pcMask
		return
	case M.big && brch == 0x101:
		d.pc = target | 0x2000
		return
	case M.big && brch == 0x141:
		d.stack[d.sp] = uint16(d.pc)
		d.sp = (d.sp + 1) & (M.stack - 1)
		d.pc = target | 0x2000
		return
	default:
		fa, fb, dpl := d.fa, d.fb, d.dp&15
		if brch >= 0x080 && brch <= 0x0af && brch&1 == 0 {
			k := (brch - 0x080) >> 1 // 0..23
			bit := []int{ndC, ndZ, ndOV0, ndOV1, ndS0, ndS1}[k>>2]
			f := fa
			if k&2 != 0 {
				f = fb
			}
			take = (f&bit != 0) == (k&1 != 0)
		} else {
			switch brch {
			case 0x0b0:
				take = dpl == 0
			case 0x0b1:
				take = dpl != 0
			case 0x0b2:
				take = dpl == 15
			case 0x0b3:
				take = dpl != 15
			case 0x0b4, 0x0b8:
				take = true
			case 0x0bc:
				take = d.sr&ndRQM == 0
			case 0x0be:
				take = d.sr&ndRQM != 0
			}
		}
	}
	if !take {
		return
	}
	d.pc = target
	if target == at {
		d.budget = 0
	}
}

func (d *necDSP) state(s *sio) {
	s.U16s(d.ram)
	s.U16s(d.stack)
	s.Ints(&d.pc, &d.sp, &d.dp, &d.rp, &d.a, &d.b, &d.fa, &d.fb, &d.tr, &d.trb, &d.k, &d.l, &d.m, &d.n,
		&d.dr, &d.sr, &d.si, &d.so, &d.budget)
}

// ---------- finding the firmware ----------

var necFiles = map[string]struct {
	file  string
	model *necModel
}{
	"DSP-1": {"dsp1.rom", upd7725},
	"DSP-2": {"dsp2.rom", upd7725},
	"DSP-3": {"dsp3.rom", upd7725},
	"DSP-4": {"dsp4.rom", upd7725},
	"ST010": {"st010.rom", upd96050},
	"ST011": {"st011.rom", upd96050},
}

func needFirmware(chip string) *FirmwareError {
	spec := necFiles[chip]
	size := spec.model.fwSize()
	return &FirmwareError{Chip: chip, File: spec.file, Size: size,
		Msg: fmt.Sprintf("This game needs the %s chip's program (%s, %d KB). Choose the file to continue.", chip, spec.file, size/1024)}
}

// findFirmware returns the parsed program for chip, nil when the player
// hasn't given one, or an error when the file given is the wrong size.
func findFirmware(s *SNES, o *chipOpts, chip string) (*necFirmware, error) {
	spec := necFiles[chip]
	size := spec.model.fwSize()
	c := s.cart
	if o != nil {
		if given := o.firmware[chip]; len(given) > 0 {
			fw := parseFirmware(given, spec.model)
			if fw == nil {
				e := needFirmware(chip)
				e.Msg = fmt.Sprintf("The %s chip's program (%s) must be %d KB (%d bytes), but the file given is %d bytes. Choose the right file.",
					chip, spec.file, size/1024, size, len(given))
				return nil, e
			}
			return fw, nil
		}
	}
	// a dump appended to the game ROM
	romSize := 0x400 << (c.hdr(0x17) & 15)
	if c.size == romSize+size {
		fw := parseFirmware(c.rom[romSize:romSize+size], spec.model)
		c.rom = c.rom[:romSize:romSize]
		c.size = romSize
		return fw, nil
	}
	return nil, nil
}

// ---------- cartridge glue ----------

// necClocked keeps a processor in step with the SNES clock.
type necClocked struct {
	snes         *SNES
	dsp          *necDSP
	rate, maxGap float64
	last         int
	frac         float64
}

func newNECClocked(s *SNES, d *necDSP, hz float64) *necClocked {
	master := float64(ntscHz)
	if s.cart.pal {
		master = palHz
	}
	return &necClocked{snes: s, dsp: d, rate: hz / master, maxGap: master / 15}
}

func (c *necClocked) sync() {
	now := c.snes.clock
	d := now - c.last
	c.last = now
	if d <= 0 {
		return
	}
	df := float64(d)
	if df > c.maxGap {
		df = c.maxGap
	}
	c.frac += df * c.rate
	n := int(math.Floor(c.frac))
	c.frac -= float64(n)
	if n > 0 {
		c.dsp.run(n)
	}
}

func (c *necClocked) reset() {
	c.dsp.reset()
	c.last = c.snes.clock
	c.frac = 0
}

func (c *necClocked) readDR() int   { c.sync(); return c.dsp.hostReadDR() }
func (c *necClocked) readSR() int   { c.sync(); return c.dsp.hostReadSR() }
func (c *necClocked) writeDR(v int) { c.sync(); c.dsp.hostWriteDR(v) }

func (c *necClocked) state(s *sio) {
	c.dsp.state(s)
	s.Int(&c.last)
	s.F64(&c.frac)
}

// dspN: DSP-3 / DSP-4, DR at $8000-$BFFF and SR at $C000-$FFFF of banks b0..b1 (and mirrors).
type dspN struct {
	b0, b1 int
	port   *necClocked
}

func newDSPn(s *SNES, o *chipOpts, chip string, b0, b1 int) (coproc, error) {
	fw, err := findFirmware(s, o, chip)
	if err != nil {
		return nil, err
	}
	if fw == nil {
		return nil, needFirmware(chip)
	}
	c := &dspN{b0: b0, b1: b1, port: newNECClocked(s, newNECDSP(upd7725, fw), 7600000)}
	s.claim(b0, b1, 0x8000, 0xffff, true)
	return c, nil
}

func (c *dspN) hit(bank int) bool { b := bank & 0x7f; return b >= c.b0 && b <= c.b1 }

func (c *dspN) read(bank, off int) int {
	if !c.hit(bank) || off < 0x8000 {
		return -1
	}
	if off < 0xc000 {
		return c.port.readDR()
	}
	return c.port.readSR()
}

func (c *dspN) write(bank, off int, v uint8) bool {
	if !c.hit(bank) || off < 0x8000 {
		return false
	}
	if off < 0xc000 {
		c.port.writeDR(int(v))
	}
	return true
}

func (c *dspN) reset()       { c.port.reset() }
func (c *dspN) state(s *sio) { c.port.state(s) }

// st01x: ST010 / ST011, DR / SR at $60-$67:0000-0FFF, the data RAM at $68-$6F.
type st01x struct {
	dsp     *necDSP
	port    *necClocked
	battery bool
}

func newST01x(s *SNES, o *chipOpts, chip string) (coproc, error) {
	fw, err := findFirmware(s, o, chip)
	if err != nil {
		return nil, err
	}
	if fw == nil {
		return nil, needFirmware(chip)
	}
	c := &st01x{dsp: newNECDSP(upd96050, fw)}
	hz := 15000000.0
	if chip == "ST010" {
		hz = 11000000
	}
	c.port = newNECClocked(s, c.dsp, hz)
	c.battery = chip == "ST010"
	if c.battery && o != nil && len(o.sram) >= 0x1000 {
		for i := 0; i < 0x800; i++ {
			c.dsp.ram[i] = uint16(o.sram[i*2]) | uint16(o.sram[i*2+1])<<8
		}
	}
	s.claim(0x60, 0x6f, 0x0000, 0x0fff, true)
	return c, nil
}

func (c *st01x) read(bank, off int) int {
	b := bank & 0x7f
	if b < 0x60 || b > 0x6f || off > 0x0fff {
		return -1
	}
	if b < 0x68 {
		if off&1 != 0 {
			return c.port.readSR()
		}
		return c.port.readDR()
	}
	c.port.sync()
	w := int(c.dsp.ram[off>>1])
	if off&1 != 0 {
		return w >> 8
	}
	return w & 0xff
}

func (c *st01x) write(bank, off int, v uint8) bool {
	b := bank & 0x7f
	if b < 0x60 || b > 0x6f || off > 0x0fff {
		return false
	}
	if b < 0x68 {
		if off&1 == 0 {
			c.port.writeDR(int(v))
		}
		return true
	}
	c.port.sync()
	r, i := c.dsp.ram, off>>1
	if off&1 != 0 {
		r[i] = r[i]&0x00ff | uint16(v)<<8
	} else {
		r[i] = r[i]&0xff00 | uint16(v)
	}
	return true
}

func (c *st01x) reset() { c.port.reset() }

func (c *st01x) getSRAM() []byte {
	if !c.battery {
		return nil
	}
	o := make([]byte, 0x1000)
	for i := 0; i < 0x800; i++ {
		o[i*2], o[i*2+1] = byte(c.dsp.ram[i]), byte(c.dsp.ram[i]>>8)
	}
	return o
}

func (c *st01x) state(s *sio) { c.port.state(s) }
