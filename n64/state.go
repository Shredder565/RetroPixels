package n64

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
)

// Save states: everything needed to carry on exactly where the machine was,
// in a versioned binary format. The layout is a magic header, a version, a
// fingerprint of the game, then each part of the machine in a fixed order.

const (
	stateMagic   = "RPN64SS\x00"
	stateVersion = 1
)

// stateWriter appends big-endian values to a buffer.
type stateWriter struct{ b []byte }

func (w *stateWriter) u8(v uint8)   { w.b = append(w.b, v) }
func (w *stateWriter) u16(v uint16) { w.b = binary.BigEndian.AppendUint16(w.b, v) }
func (w *stateWriter) u32(v uint32) { w.b = binary.BigEndian.AppendUint32(w.b, v) }
func (w *stateWriter) u64(v uint64) { w.b = binary.BigEndian.AppendUint64(w.b, v) }
func (w *stateWriter) i64(v int64)  { w.u64(uint64(v)) }
func (w *stateWriter) flag(v bool) {
	if v {
		w.u8(1)
	} else {
		w.u8(0)
	}
}

// blob writes a length-prefixed byte string.
func (w *stateWriter) blob(b []byte) {
	w.u32(uint32(len(b)))
	w.b = append(w.b, b...)
}

// stateReader reads what stateWriter wrote; the first problem sticks in err
// and makes every later read return zero.
type stateReader struct {
	b   []byte
	err error
}

func (r *stateReader) take(n int) []byte {
	if r.err != nil {
		return nil
	}
	if n < 0 || n > len(r.b) {
		r.err = errors.New("the save state is cut short or damaged")
		return nil
	}
	v := r.b[:n]
	r.b = r.b[n:]
	return v
}

func (r *stateReader) u8() uint8 {
	if b := r.take(1); b != nil {
		return b[0]
	}
	return 0
}

func (r *stateReader) u16() uint16 {
	if b := r.take(2); b != nil {
		return binary.BigEndian.Uint16(b)
	}
	return 0
}

func (r *stateReader) u32() uint32 {
	if b := r.take(4); b != nil {
		return binary.BigEndian.Uint32(b)
	}
	return 0
}

func (r *stateReader) u64() uint64 {
	if b := r.take(8); b != nil {
		return binary.BigEndian.Uint64(b)
	}
	return 0
}

func (r *stateReader) i64() int64 { return int64(r.u64()) }
func (r *stateReader) flag() bool { return r.u8() != 0 }

// blob reads a length-prefixed byte string.
func (r *stateReader) blob() []byte { return r.take(int(r.u32())) }

// fill reads a blob that must be exactly len(dst) bytes into dst.
func (r *stateReader) fill(dst []byte, what string) {
	b := r.blob()
	if r.err == nil && len(b) != len(dst) {
		r.err = fmt.Errorf("the save state's %s has the wrong size", what)
		return
	}
	copy(dst, b)
}

// fingerprint identifies the game a state belongs to.
func (m *Machine) fingerprint() uint32 {
	return crc32.ChecksumIEEE(m.rom[:0x1000]) ^ uint32(len(m.rom))
}

// SaveState captures the whole machine.
func (m *Machine) SaveState() []byte {
	w := &stateWriter{b: make([]byte, 0, rdramSize+0x60000)}
	w.b = append(w.b, stateMagic...)
	w.u32(stateVersion)
	w.u32(m.fingerprint())

	// CPU
	c := &m.CPU
	for _, v := range c.gpr {
		w.u64(v)
	}
	w.u64(c.hi)
	w.u64(c.lo)
	w.u64(c.pc)
	w.u64(c.next)
	w.u64(c.curPC)
	w.flag(c.delay)
	w.flag(c.nDelay)
	w.flag(c.llbit)
	for _, v := range c.cop0 {
		w.u64(v)
	}
	for _, v := range c.fpr {
		w.u64(v)
	}
	w.u32(c.fcr31)
	for _, e := range c.tlb {
		w.u64(e.mask)
		w.u64(e.hi)
		w.u64(e.lo0)
		w.u64(e.lo1)
	}
	w.u64(c.Cycles)
	w.u64(c.countBase)
	w.u64(c.compareAt)
	w.flag(c.irqPending)

	// RSP
	r := &m.RSP
	for _, v := range r.r {
		w.u32(v)
	}
	w.u32(r.pc)
	w.u32(r.next)
	w.flag(r.nDelay)
	w.flag(r.delay)
	for i := range r.vr {
		for _, v := range r.vr[i] {
			w.u16(v)
		}
	}
	for _, v := range r.acc {
		w.i64(v)
	}
	w.u8(r.vcoLo)
	w.u8(r.vcoHi)
	w.u8(r.vccLo)
	w.u8(r.vccHi)
	w.u8(r.vce)
	w.u32(uint32(r.divIn))
	w.u32(uint32(r.divOut))
	w.flag(r.divLoad)
	w.u64(r.Cycles)

	// memories
	w.blob(m.rdram)
	w.blob(m.spmem[:])
	w.blob(m.pif[:])

	// register blocks
	w.u32(m.miMode)
	w.u32(m.miIntr)
	w.u32(m.miMask)
	for _, v := range m.vi {
		w.u32(v)
	}
	w.u32(m.viLine)
	w.u32(m.viField)
	a := &m.ai
	w.u32(a.dramNext)
	for _, q := range a.queue {
		w.u32(q.addr)
		w.u32(q.length)
	}
	w.u32(uint32(a.queued))
	w.u32(a.control)
	w.u32(a.dacrate)
	w.u32(a.bitrate)
	w.u64(a.playStart)
	w.u64(a.playEnd)
	w.u32(a.sent)
	for _, v := range m.pi {
		w.u32(v)
	}
	w.u64(m.piBusyUntil)
	w.u32(m.siDram)
	w.u32(m.siStatus)
	w.u64(m.siBusyUntil)
	sp := &m.sp
	for _, v := range []uint32{sp.memAddr, sp.dramAddr, sp.curMem, sp.curDram, sp.length, sp.status, sp.semaphore} {
		w.u32(v)
	}
	dp := &m.dp
	for _, v := range []uint32{dp.start, dp.end, dp.current, dp.status, dp.clock} {
		w.u32(v)
	}
	for _, v := range m.ri {
		w.u32(v)
	}

	// timing
	w.u32(uint32(len(m.pending)))
	for _, e := range m.pending {
		w.u64(e.at)
		w.u32(uint32(e.kind))
	}
	w.u64(m.rspTime)
	w.u64(m.rspFrac)
	w.u32(uint32(m.AudioRate))

	// save memory and controllers
	s := m.Save
	w.blob(s.eeprom)
	w.u32(uint32(s.eepromSize))
	w.blob(s.sram)
	w.flag(s.flash)
	for _, p := range s.paks {
		w.blob(p)
	}
	w.u32(uint32(s.fMode))
	w.u32(s.fStatus)
	w.u32(s.fOffset)
	w.blob(s.fBuf[:])
	for _, p := range m.Pad {
		w.u16(p.Buttons)
		w.u8(uint8(p.X))
		w.u8(uint8(p.Y))
		w.flag(p.Present)
		w.flag(p.Pak)
	}

	// the rasterizer keeps its own state
	w.blob(m.RDP.marshalState())
	w.u32(crc32.ChecksumIEEE(w.b))
	return w.b
}

// LoadState restores a state made by SaveState for the same game. On error
// the machine is left as it was.
func (m *Machine) LoadState(b []byte) error {
	if len(b) < len(stateMagic)+12 || string(b[:len(stateMagic)]) != stateMagic {
		return errors.New("that isn't a Nintendo 64 save state")
	}
	body, sum := b[:len(b)-4], binary.BigEndian.Uint32(b[len(b)-4:])
	if crc32.ChecksumIEEE(body) != sum {
		return errors.New("the save state is damaged")
	}
	r := &stateReader{b: body[len(stateMagic):]}
	if v := r.u32(); v != stateVersion {
		return fmt.Errorf("the save state is from an unsupported version (%d)", v)
	}
	if r.u32() != m.fingerprint() {
		return errors.New("the save state is for a different game")
	}
	// read into a staging copy so a damaged state can't leave the machine
	// half loaded
	var n struct {
		CPU                    CPU
		RSP                    RSP
		rdram                  []byte
		spmem                  [0x2000]byte
		pif                    [64]byte
		miMode, miIntr, miMask uint32
		vi                     [14]uint32
		viLine, viField        uint32
		ai                     aiState
		pi                     [13]uint32
		piBusyUntil            uint64
		siDram, siStatus       uint32
		siBusyUntil            uint64
		sp                     spRegs
		dp                     dpRegs
		ri                     [8]uint32
		pending                []event
		rspTime, rspFrac       uint64
		AudioRate              int
		Save                   *Saves
		Pad                    [4]PadState
	}
	n.CPU = m.CPU
	n.RSP = m.RSP
	n.rdram = make([]byte, rdramSize)
	n.Save = &Saves{}
	*n.Save = *m.Save

	c := &n.CPU
	for i := range c.gpr {
		c.gpr[i] = r.u64()
	}
	c.hi = r.u64()
	c.lo = r.u64()
	c.pc = r.u64()
	c.next = r.u64()
	c.curPC = r.u64()
	c.delay = r.flag()
	c.nDelay = r.flag()
	c.llbit = r.flag()
	for i := range c.cop0 {
		c.cop0[i] = r.u64()
	}
	for i := range c.fpr {
		c.fpr[i] = r.u64()
	}
	c.fcr31 = r.u32()
	for i := range c.tlb {
		e := &c.tlb[i]
		e.mask = r.u64()
		e.hi = r.u64()
		e.lo0 = r.u64()
		e.lo1 = r.u64()
	}
	c.Cycles = r.u64()
	c.countBase = r.u64()
	c.compareAt = r.u64()
	c.irqPending = r.flag()

	rs := &n.RSP
	for i := range rs.r {
		rs.r[i] = r.u32()
	}
	rs.pc = r.u32() & 0xffc
	rs.next = r.u32() & 0xffc
	rs.nDelay = r.flag()
	rs.delay = r.flag()
	for i := range rs.vr {
		for j := range rs.vr[i] {
			rs.vr[i][j] = r.u16()
		}
	}
	for i := range rs.acc {
		rs.acc[i] = sext48(r.i64())
	}
	rs.vcoLo = r.u8()
	rs.vcoHi = r.u8()
	rs.vccLo = r.u8()
	rs.vccHi = r.u8()
	rs.vce = r.u8()
	rs.divIn = int32(r.u32())
	rs.divOut = int32(r.u32())
	rs.divLoad = r.flag()
	rs.Cycles = r.u64()

	r.fill(n.rdram, "RDRAM")
	r.fill(n.spmem[:], "RSP memory")
	r.fill(n.pif[:], "PIF memory")

	n.miMode = r.u32()
	n.miIntr = r.u32()
	n.miMask = r.u32()
	for i := range n.vi {
		n.vi[i] = r.u32()
	}
	n.viLine = r.u32()
	n.viField = r.u32()
	a := &n.ai
	a.dramNext = r.u32()
	for i := range a.queue {
		a.queue[i].addr = r.u32()
		a.queue[i].length = r.u32()
	}
	a.queued = int(r.u32() % 3)
	a.control = r.u32()
	a.dacrate = r.u32()
	a.bitrate = r.u32()
	a.playStart = r.u64()
	a.playEnd = r.u64()
	a.sent = r.u32()
	for i := range n.pi {
		n.pi[i] = r.u32()
	}
	n.piBusyUntil = r.u64()
	n.siDram = r.u32()
	n.siStatus = r.u32()
	n.siBusyUntil = r.u64()
	sp := &n.sp
	for _, p := range []*uint32{&sp.memAddr, &sp.dramAddr, &sp.curMem, &sp.curDram, &sp.length, &sp.status, &sp.semaphore} {
		*p = r.u32()
	}
	dp := &n.dp
	for _, p := range []*uint32{&dp.start, &dp.end, &dp.current, &dp.status, &dp.clock} {
		*p = r.u32()
	}
	for i := range n.ri {
		n.ri[i] = r.u32()
	}

	np := int(r.u32())
	if np > 64 {
		return errors.New("the save state is damaged")
	}
	n.pending = make([]event, np)
	for i := range n.pending {
		n.pending[i].at = r.u64()
		n.pending[i].kind = int(r.u32())
	}
	n.rspTime = r.u64()
	n.rspFrac = r.u64() % 3
	n.AudioRate = int(r.u32())

	s := n.Save
	s.eeprom = append([]byte(nil), r.blob()...)
	s.eepromSize = int(r.u32())
	s.sram = append([]byte(nil), r.blob()...)
	s.flash = r.flag()
	for i := range s.paks {
		s.paks[i] = append([]byte(nil), r.blob()...)
	}
	s.fMode = int(r.u32())
	s.fStatus = r.u32()
	s.fOffset = r.u32()
	r.fill(s.fBuf[:], "FlashRAM buffer")
	for i := range n.Pad {
		p := &n.Pad[i]
		p.Buttons = r.u16()
		p.X = int8(r.u8())
		p.Y = int8(r.u8())
		p.Present = r.flag()
		p.Pak = r.flag()
	}
	rdp := r.blob()
	if r.err != nil {
		return r.err
	}
	if len(r.b) != 0 {
		return errors.New("the save state has unexpected extra data")
	}
	if s.eepromSize != len(s.eeprom) || len(s.sram) != 0x20000 || (s.eepromSize != 512 && s.eepromSize != 2048) {
		return errors.New("the save state's save memory is damaged")
	}
	for _, p := range s.paks {
		if len(p) != 0x8000 {
			return errors.New("the save state's Controller Pak data is damaged")
		}
	}
	if err := m.RDP.unmarshalState(rdp); err != nil {
		return err
	}

	// commit
	copy(m.rdram, n.rdram)
	m.CPU = n.CPU
	m.RSP = n.RSP
	m.spmem = n.spmem
	m.pif = n.pif
	m.miMode, m.miIntr, m.miMask = n.miMode, n.miIntr, n.miMask
	m.vi, m.viLine, m.viField = n.vi, n.viLine, n.viField
	m.ai = n.ai
	m.pi, m.piBusyUntil = n.pi, n.piBusyUntil
	m.siDram, m.siStatus, m.siBusyUntil = n.siDram, n.siStatus, n.siBusyUntil
	m.sp, m.dp, m.ri = n.sp, n.dp, n.ri
	m.pending = n.pending
	m.rspTime, m.rspFrac = n.rspTime, n.rspFrac
	m.AudioRate = n.AudioRate
	*m.Save = *n.Save
	m.Pad = n.Pad
	m.CPU.flushTLBCache()
	m.idle, m.stop = false, false
	return nil
}
