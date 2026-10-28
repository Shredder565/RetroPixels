package model3

import (
	"sync"

	"retropixels/cores/m68k"
)

// The sound board: a 68000 at 11.2896 MHz with two SCSP sound chips, each
// with 1 MB of RAM, and banked sample ROM. The main board talks to it only
// through MIDI bytes (which the MPEG music board hears too). It makes 44.1
// kHz stereo; the master chip feeds the front speakers and the slave the
// rear ones, mixed together here as Supermodel mixes them for two speakers.

const (
	soundRate         = 44100
	soundCyclesPerSmp = 11289600 / 44100 // 256
)

type soundBoard struct {
	cpu       *m68k.CPU
	prog      []byte // 512 KB
	samples   []byte // 16 MB
	bank      uint32
	ctrl      uint8
	ram1      []byte
	ram2      []byte
	scsp      *scsp
	dsb       *dsb2
	cycleGoal int64

	// samples made by the last frame, interleaved left, right
	out     []float32
	rear    []float32
	outFrac float64

	// The board runs a frame behind the main board, on its own goroutine:
	// the MIDI bytes the main board writes in a frame are queued, and handed
	// over (as Supermodel hands them over after the main board's frame) when
	// the frame ends.
	midiQ   []uint8
	running []uint8
	ready   []float32 // the finished frame's sound
	busy    sync.WaitGroup
}

func newSoundBoard(r *ROMs, g *GameDef) *soundBoard {
	s := &soundBoard{prog: r.SoundProg, samples: r.Samples}
	if len(s.prog) < 0x80000 {
		p := make([]byte, 0x80000)
		copy(p, s.prog)
		s.prog = p
	}
	if len(s.samples) < 0x1000000 {
		p := make([]byte, 0x1000000)
		copy(p, s.samples)
		s.samples = p
	}
	s.ram1 = make([]byte, 0x100000)
	s.ram2 = make([]byte, 0x100000)
	s.cpu = m68k.New(s)
	s.scsp = newSCSP(s.ram1, s.ram2, s.cpu.SetIRQ)
	if g.DSB == "DSB2" && r.DSBProg != nil {
		s.dsb = newDSB2(r.DSBProg, r.MPEG)
	}
	return s
}

func (s *soundBoard) reset() {
	s.busy.Wait()
	s.midiQ = s.midiQ[:0]
	s.ready = s.ready[:0]
	for i := range s.ram1 {
		s.ram1[i] = 0
		s.ram2[i] = 0
	}
	copy(s.ram1, s.prog[:16]) // the 68000's vectors
	s.ctrl = 0
	s.bank = 0
	s.scsp.reset()
	s.cpu.SetIRQ(0)
	s.cpu.Cycles = 0
	s.cycleGoal = 0
	s.cpu.Reset()
	if s.dsb != nil {
		s.dsb.reset()
	}
	s.out = s.out[:0]
	s.outFrac = 0
}

func (s *soundBoard) writeMIDI(v uint8) { s.midiQ = append(s.midiQ, v) }

// endFrame hands the frame's MIDI bytes to the board and starts its next
// frame running; the sound of the frame before is then in ready.
func (s *soundBoard) endFrame() {
	s.busy.Wait()
	s.ready = append(s.ready[:0], s.out...)
	s.running, s.midiQ = s.midiQ, s.running[:0]
	s.busy.Add(1)
	go func() {
		defer s.busy.Done()
		for _, v := range s.running {
			s.scsp.midiIn(v)
			if s.dsb != nil {
				s.dsb.sendCommand(v)
			}
		}
		s.runFrame()
	}()
}

// wait waits for the frame running on the board's goroutine.
func (s *soundBoard) wait() { s.busy.Wait() }

// runFrame makes one video frame's worth of sound (the board runs at its
// own 44.1 kHz; the frame is 1/57.524 s).
func (s *soundBoard) runFrame() {
	s.outFrac += soundRate / 57.52416
	n := int(s.outFrac)
	s.outFrac -= float64(n)
	if cap(s.out) < 2*n {
		s.out = make([]float32, 0, 2*n+16)
	}
	s.out = s.out[:2*n]
	if cap(s.rear) < 2*n {
		s.rear = make([]float32, 0, 2*n+16)
	}
	s.rear = s.rear[:2*n]
	for i := 0; i < n; i++ {
		fl, fr, rl, rr := s.scsp.sample()
		s.out[2*i], s.out[2*i+1] = float32(fl), float32(fr)
		s.rear[2*i], s.rear[2*i+1] = float32(rl), float32(rr)
		s.cycleGoal += soundCyclesPerSmp
		s.cpu.Run(s.cycleGoal)
	}
	if s.dsb != nil {
		s.dsb.runFrame(s.out, n) // the music goes to the front speakers
	}
	// Supermodel mixes front and rear for two speakers at half volume each
	for i, v := range s.out {
		v = float32(int32((v + s.rear[i]) * 0.5))
		if v > 32767 {
			v = 32767
		} else if v < -32768 {
			v = -32768
		}
		s.out[i] = v / 32768
	}
}

// ---------------------------------------------------------------- 68000 bus

func (s *soundBoard) Read8(a uint32) uint8 {
	switch a >> 20 & 0xF {
	case 0x0:
		return s.ram1[a&0xFFFFF]
	case 0x1:
		return s.scsp.read8(0, a)
	case 0x2:
		return s.ram2[a&0xFFFFF]
	case 0x3:
		return s.scsp.read8(1, a)
	case 0x6:
		return s.prog[a&0x7FFFF]
	case 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF:
		return s.samples[s.bank+a&0x7FFFFF]
	}
	return 0
}

func be16(b []byte, i uint32) uint16 { return uint16(b[i])<<8 | uint16(b[i+1]) }

func (s *soundBoard) Read16(a uint32) uint16 {
	switch a >> 20 & 0xF {
	case 0x0:
		return be16(s.ram1, a&0xFFFFE)
	case 0x1:
		return s.scsp.read16(0, a)
	case 0x2:
		return be16(s.ram2, a&0xFFFFE)
	case 0x3:
		return s.scsp.read16(1, a)
	case 0x6:
		return be16(s.prog, a&0x7FFFE)
	case 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF:
		return be16(s.samples, s.bank+a&0x7FFFFE)
	}
	return 0
}

// LongRead: long reads of the sound chips give all ones, as in Supermodel.
func (s *soundBoard) LongRead(a uint32) uint32 {
	switch a >> 20 & 0xF {
	case 0x1, 0x3:
		return 0xFFFFFFFF
	}
	return uint32(s.Read16(a))<<16 | uint32(s.Read16((a+2)&0xFFFFFF))
}

func (s *soundBoard) Write8(a uint32, v uint8) {
	switch a >> 20 & 0xF {
	case 0x0:
		s.ram1[a&0xFFFFF] = v
	case 0x1:
		s.scsp.write8(0, a, v)
	case 0x2:
		s.ram2[a&0xFFFFF] = v
	case 0x3:
		s.scsp.write8(1, a, v)
	default:
		if a == 0x400001 {
			s.ctrl = v
			if v&0x10 != 0 {
				s.bank = 0x800000
			} else {
				s.bank = 0
			}
		}
	}
}

func (s *soundBoard) Write16(a uint32, v uint16) {
	switch a >> 20 & 0xF {
	case 0x0:
		s.ram1[a&0xFFFFE] = uint8(v >> 8)
		s.ram1[a&0xFFFFE+1] = uint8(v)
	case 0x1:
		s.scsp.write16(0, a, v)
	case 0x2:
		s.ram2[a&0xFFFFE] = uint8(v >> 8)
		s.ram2[a&0xFFFFE+1] = uint8(v)
	case 0x3:
		s.scsp.write16(1, a, v)
	}
}

func (s *soundBoard) LongWrite(a uint32, v uint32) {
	switch a >> 20 & 0xF {
	case 0x1:
		s.scsp.write32(0, a, v)
	case 0x3:
		s.scsp.write32(1, a, v)
	default:
		s.Write16(a, uint16(v>>16))
		s.Write16((a+2)&0xFFFFFF, uint16(v))
	}
}

// Ack: the sound chips' interrupts use the autovectors and stay raised until
// the chips drop them.
func (s *soundBoard) Ack(level int) int { return -1 }
