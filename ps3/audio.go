package ps3

// cellAudio: the game's audio ports. Every 256 samples (48 kHz) the audio
// system reads the next block of each open port, mixes them, and moves the
// port's read index on.

import (
	"encoding/binary"
	"math"
	"os"
)

const audioBlock = 256

type audioPort struct {
	open, started bool
	channels      uint32
	blocks        uint32
	addr          uint32
	index         uint32 // read index address
	level         float32
	peak          float32
	chPeak        [8]float32
}

var audioDebug = os.Getenv("PS3_AUDIODBG") != ""

// audioRaw (PS3_AUDIORAW=file): port 0's front channels, as they are
var audioRaw = func() *os.File {
	if p := os.Getenv("PS3_AUDIORAW"); p != "" {
		f, _ := os.Create(p)
		return f
	}
	return nil
}()

func abs32(x float32) float32 { return float32(math.Abs(float64(x))) }

type audioState struct {
	inited    bool
	ports     [8]audioPort
	indices   uint32 // 8 x u64 read indices
	tbNext    uint64
	out       []float32 // stereo samples for the shell
	dbgBlocks int
	gain      float32 // the limiter's
}

const tbPerBlock = TBFreq * audioBlock / 48000

func (m *Machine) audio0() *audioState {
	if m.audio == nil {
		m.audio = &audioState{gain: 1}
	}
	return m.audio
}

func init() {
	reg("cellAudioInit", func(m *Machine, t *Thread) {
		a := m.audio0()
		a.inited = true
		a.indices = m.sysAlloc(8*8, 128)
		a.tbNext = m.tb + tbPerBlock
		t.ret(0)
	})
	reg("cellAudioQuit", func(m *Machine, t *Thread) { m.audio0().inited = false; t.ret(0) })
	reg("cellAudioPortOpen", func(m *Machine, t *Thread) { // (param*, portNum*)
		a := m.audio0()
		p := t.arg(0)
		ch := uint32(m.Mem.Read64(p))
		nb := uint32(m.Mem.Read64(p + 8))
		level := float32(1)
		if attr := m.Mem.Read64(p + 16); attr&0x1000 != 0 {
			level = m.Mem.ReadF32(p + 24)
		}
		for i := range a.ports {
			if !a.ports[i].open {
				size := ch * nb * audioBlock * 4
				addr := m.user.alloc(size, 0x10000)
				m.Mem.Fill(addr, 0, size)
				a.ports[i] = audioPort{open: true, channels: ch, blocks: nb, addr: addr, index: a.indices + 8*uint32(i), level: level}
				m.Mem.Write64(a.ports[i].index, 0)
				m.Mem.Write32(t.arg(1), uint32(i))
				m.Logf("audio port %d: %d channels, %d blocks at %08X", i, ch, nb, addr)
				t.ret(0)
				return
			}
		}
		t.ret(0x80310705)
	})
	reg("cellAudioGetPortConfig", func(m *Machine, t *Thread) { // (portNum, config*)
		a := m.audio0()
		i := t.arg(0)
		if i >= 8 || !a.ports[i].open {
			t.ret(0x80310704)
			return
		}
		p := &a.ports[i]
		c := t.arg(1)
		m.Mem.Write32(c, p.index)
		st := uint32(1) // ready
		if p.started {
			st = 2
		}
		m.Mem.Write32(c+4, st)
		m.Mem.Write64(c+8, uint64(p.channels))
		m.Mem.Write64(c+16, uint64(p.blocks))
		m.Mem.Write32(c+24, p.channels*p.blocks*audioBlock*4)
		m.Mem.Write32(c+28, p.addr)
		t.ret(0)
	})
	reg("cellAudioPortStart", func(m *Machine, t *Thread) {
		if i := t.arg(0); i < 8 {
			m.audio0().ports[i].started = true
		}
		t.ret(0)
	})
	reg("cellAudioPortStop", func(m *Machine, t *Thread) {
		if i := t.arg(0); i < 8 {
			m.audio0().ports[i].started = false
		}
		t.ret(0)
	})
	reg("cellAudioPortClose", func(m *Machine, t *Thread) {
		if i := t.arg(0); i < 8 {
			m.audio0().ports[i].open = false
		}
		t.ret(0)
	})
}

// audioTick mixes the blocks due by now.
func (m *Machine) audioTick() {
	a := m.audio
	if a == nil || !a.inited {
		return
	}
	for m.tb >= a.tbNext {
		a.tbNext += tbPerBlock
		mix := make([]float32, 2*audioBlock)
		for i := range a.ports {
			p := &a.ports[i]
			if !p.open || !p.started {
				continue
			}
			idx := m.Mem.Read64(p.index)
			blk := p.addr + uint32(idx%uint64(p.blocks))*p.channels*audioBlock*4
			for s := uint32(0); s < audioBlock; s++ {
				base := blk + s*p.channels*4
				l := m.Mem.ReadF32(base)
				r := l
				if p.channels >= 2 {
					r = m.Mem.ReadF32(base + 4)
				}
				if p.channels >= 6 { // fold the centre and surrounds in
					c := m.Mem.ReadF32(base + 8)
					l += 0.7*c + 0.5*m.Mem.ReadF32(base+16)
					r += 0.7*c + 0.5*m.Mem.ReadF32(base+20)
				}
				if audioRaw != nil && i == 0 {
					var b [8]byte
					binary.LittleEndian.PutUint32(b[:], math.Float32bits(m.Mem.ReadF32(base)))
					binary.LittleEndian.PutUint32(b[4:], math.Float32bits(m.Mem.ReadF32(base+4)))
					audioRaw.Write(b[:])
				}
				if audioDebug {
					p.peak = max(p.peak, abs32(l), abs32(r))
					for k := uint32(0); k < p.channels && k < 8; k++ {
						p.chPeak[k] = max(p.chPeak[k], abs32(m.Mem.ReadF32(base+4*k)))
					}
				}
				mix[2*s] += l * p.level
				mix[2*s+1] += r * p.level
			}
			// the block has been played: clear it, move on
			m.Mem.Fill(blk, 0, p.channels*audioBlock*4)
			m.Mem.Write64(p.index, idx+1)
		}
		if audioDebug {
			if a.dbgBlocks++; a.dbgBlocks%188 == 0 {
				for i := range a.ports {
					if a.ports[i].open {
						m.Logf("audio port %d (%d ch, level %.2f): peak %.3f %.2f", i, a.ports[i].channels, a.ports[i].level, a.ports[i].peak, a.ports[i].chPeak)
						a.ports[i].peak = 0
						a.ports[i].chPeak = [8]float32{}
					}
				}
			}
		}
		// a limiter: the game's mix can go well past full scale (the music
		// engine puts both music channels in both speakers)
		for s := 0; s < len(mix); s += 2 {
			pk := max(abs32(mix[s]), abs32(mix[s+1]))
			if pk*a.gain > 0.95 {
				a.gain = 0.95 / pk
			} else {
				a.gain += (1 - a.gain) * (1.0 / 24000)
			}
			mix[s] *= a.gain
			mix[s+1] *= a.gain
		}
		if len(a.out) < 48000*2 {
			a.out = append(a.out, mix...)
		}
	}
}

func (m *Machine) audioVBlank() {}

// TakeAudio returns the stereo samples (48 kHz) made since the last call.
func (m *Machine) TakeAudio() []float32 {
	if m.audio == nil {
		return nil
	}
	o := m.audio.out
	m.audio.out = nil
	return o
}
