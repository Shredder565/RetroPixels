package gba

// Sound: four PSG channels and the two DMA sound FIFOs (APU in web/emu/gba.js).

var dutyTable = [4]int{0x01, 0x81, 0x87, 0x7e}
var noiseDiv = [8]int{8, 16, 32, 48, 64, 80, 96, 112}
var ratioTable = [4]float64{0.25, 0.5, 1, 1}

const apuOutMax = 16384

type sndChannel struct {
	on, dac                                   bool
	len                                       int
	lenOn                                     bool
	vol, envDir, envPeriod, envTimer, envInit int
	freq, timer, pos, duty, lfsr              int
	sweepPeriod, sweepTimer, sweepShift       int
	sweepNeg, sweepOn                         bool
	shadow                                    int
}

type apu struct {
	g          *GBA
	sampleRate int
	step       float64
	wave       [32]byte
	fifo       [2][32]int8
	out        []float32

	ch                     [4]sndChannel
	fifoR, fifoW, fifoN    [2]int
	fifoOut                [2]int
	seqTimer, seqStep      int
	last                   int64 // cycle time audio has been generated up to
	acc, sumL, sumR        float64
	n                      int
	hpL, hpR, prevL, prevR float64
}

func newAPU(g *GBA, sampleRate int) *apu {
	a := &apu{g: g, sampleRate: sampleRate, step: cpuHz / float64(sampleRate), out: make([]float32, 0, apuOutMax)}
	a.reset()
	return a
}

func (a *apu) reset() {
	for i := range a.ch {
		a.ch[i] = sndChannel{lfsr: 0x7fff}
	}
	a.wave = [32]byte{}
	a.fifoR, a.fifoW, a.fifoN, a.fifoOut = [2]int{}, [2]int{}, [2]int{}, [2]int{}
	a.seqTimer, a.seqStep = 32768, 0
	a.last = 0
	a.acc, a.sumL, a.sumR, a.n = 0, 0, 0, 0
	a.hpL, a.hpR, a.prevL, a.prevR = 0, 0, 0, 0
}

func (a *apu) io(o int) int { return int(a.g.io16(o)) }

// write16: registers (offsets from 0x60).
func (a *apu) write16(off int, v int) {
	master := a.io(0x84) & 0x80
	if master == 0 && off >= 0x60 && off < 0x82 {
		return
	}
	switch off {
	case 0x60:
		c := &a.ch[0]
		c.sweepPeriod = v >> 4 & 7
		c.sweepNeg = v&8 != 0
		c.sweepShift = v & 7
	case 0x62, 0x68:
		c := &a.ch[1]
		if off == 0x62 {
			c = &a.ch[0]
		}
		c.len = 64 - v&0x3f
		c.duty = v >> 6 & 3
		c.dac = v&0xf800 != 0
		if !c.dac {
			c.on = false
		}
	case 0x64, 0x6c:
		i := 1
		if off == 0x64 {
			i = 0
		}
		c := &a.ch[i]
		c.freq = v & 0x7ff
		c.lenOn = v&0x4000 != 0
		if v&0x8000 != 0 {
			a.trigger(i)
		}
	case 0x70:
		c := &a.ch[2]
		c.dac = v&0x80 != 0
		if !c.dac {
			c.on = false
		}
	case 0x72:
		a.ch[2].len = 256 - v&0xff
	case 0x74:
		c := &a.ch[2]
		c.freq = v & 0x7ff
		c.lenOn = v&0x4000 != 0
		if v&0x8000 != 0 {
			a.trigger(2)
		}
	case 0x78:
		c := &a.ch[3]
		c.len = 64 - v&0x3f
		c.dac = v&0xf800 != 0
		if !c.dac {
			c.on = false
		}
	case 0x7c:
		c := &a.ch[3]
		c.lenOn = v&0x4000 != 0
		if v&0x8000 != 0 {
			a.trigger(3)
		}
	case 0x82:
		if v&0x800 != 0 {
			a.fifoR[0], a.fifoW[0], a.fifoN[0] = 0, 0, 0
		}
		if v&0x8000 != 0 {
			a.fifoR[1], a.fifoW[1], a.fifoN[1] = 0, 0, 0
		}
	case 0x84:
		if v&0x80 == 0 && master != 0 {
			for o := 0x60; o < 0x82; o += 2 {
				a.g.setIO16(o, 0)
			}
			for i := range a.ch {
				a.ch[i].on = false
				a.ch[i].dac = false
			}
		}
		if v&0x80 != 0 && master == 0 {
			a.seqStep = 0
		}
	}
}

func (a *apu) trigger(i int) {
	c := &a.ch[i]
	c.on = c.dac
	if c.len == 0 {
		if i == 2 {
			c.len = 256
		} else {
			c.len = 64
		}
	}
	switch i {
	case 2:
		c.timer = (2048 - c.freq) * 8
		c.pos = 0
	case 3:
		v := a.io(0x7c)
		c.timer = (noiseDiv[v&7] << (v >> 4 & 15)) * 4
		if v&8 != 0 {
			c.lfsr = 0x7f
		} else {
			c.lfsr = 0x7fff
		}
	default:
		c.timer = (2048 - c.freq) * 16
	}
	if i != 2 {
		reg := 0x78
		if i == 0 {
			reg = 0x62
		} else if i == 1 {
			reg = 0x68
		}
		e := a.io(reg) >> 8
		c.vol = e >> 4
		if e&8 != 0 {
			c.envDir = 1
		} else {
			c.envDir = -1
		}
		c.envPeriod = e & 7
		c.envTimer = c.envPeriod
		if c.envTimer == 0 {
			c.envTimer = 8
		}
	}
	if i == 0 {
		c.shadow = c.freq
		c.sweepTimer = c.sweepPeriod
		if c.sweepTimer == 0 {
			c.sweepTimer = 8
		}
		c.sweepOn = c.sweepPeriod != 0 || c.sweepShift != 0
		if c.sweepShift != 0 && sweepCalc(c) > 2047 {
			c.on = false
		}
	}
}

func sweepCalc(c *sndChannel) int {
	d := c.shadow >> c.sweepShift
	if c.sweepNeg {
		return c.shadow - d
	}
	return c.shadow + d
}

func (a *apu) clockSeq() {
	s := a.seqStep
	if s&1 == 0 {
		for i := range a.ch {
			c := &a.ch[i]
			if c.lenOn && c.len > 0 {
				c.len--
				if c.len == 0 {
					c.on = false
				}
			}
		}
	}
	if s == 2 || s == 6 {
		c := &a.ch[0]
		c.sweepTimer--
		if c.sweepTimer <= 0 {
			c.sweepTimer = c.sweepPeriod
			if c.sweepTimer == 0 {
				c.sweepTimer = 8
			}
			if c.sweepOn && c.sweepPeriod != 0 {
				f := sweepCalc(c)
				if f > 2047 {
					c.on = false
				} else if c.sweepShift != 0 {
					c.shadow = f
					c.freq = f
					if sweepCalc(c) > 2047 {
						c.on = false
					}
				}
			}
		}
	}
	if s == 7 {
		for _, i := range [3]int{0, 1, 3} {
			c := &a.ch[i]
			if c.envPeriod != 0 {
				c.envTimer--
				if c.envTimer <= 0 {
					c.envTimer = c.envPeriod
					nv := c.vol + c.envDir
					if nv >= 0 && nv <= 15 {
						c.vol = nv
					}
				}
			}
		}
	}
	a.seqStep = (s + 1) & 7
}

// wave RAM as seen by the CPU (the bank that isn't playing)
func (a *apu) waveRead(i int) int {
	bank := a.io(0x70) >> 6 & 1
	return int(a.wave[(bank^1)*16+i])
}

func (a *apu) waveWrite(i, v int) {
	bank := a.io(0x70) >> 6 & 1
	a.wave[(bank^1)*16+i] = byte(v)
}

// ---- DMA sound ----

func (a *apu) fifoWrite(k int, v32 uint32, n int) {
	for b := 0; b < n; b++ {
		if a.fifoN[k] < 32 {
			a.fifo[k][a.fifoW[k]] = int8(v32 >> (b * 8))
			a.fifoW[k] = (a.fifoW[k] + 1) & 31
			a.fifoN[k]++
		}
	}
}

func (a *apu) timerOverflow(t int) {
	cnt := a.io(0x82)
	for k := 0; k < 2; k++ {
		sel := cnt >> (10 + k*4) & 1
		if sel != t {
			continue
		}
		if a.fifoN[k] > 0 {
			a.fifoOut[k] = int(a.fifo[k][a.fifoR[k]])
			a.fifoR[k] = (a.fifoR[k] + 1) & 31
			a.fifoN[k]--
		}
		if a.fifoN[k] <= 16 {
			a.g.fifoRequest(k)
		}
	}
}

// advance makes sound up to cycle time t.
func (a *apu) advance(t int64) {
	n := t - a.last
	if n <= 0 {
		return
	}
	a.last = t
	g := a.g
	master := a.io(0x84) & 0x80
	// computed in chunks no longer than one output sample so changes are captured
	step := a.step
	for n > 0 {
		k := int(min(n, 64))
		n -= int64(k)
		L, R := 0.0, 0.0
		if master != 0 {
			a.seqTimer -= k
			for a.seqTimer <= 0 {
				a.seqTimer += 32768
				a.clockSeq()
			}
			ch := &a.ch
			for i := 0; i < 4; i++ {
				c := &ch[i]
				if !c.on {
					continue
				}
				c.timer -= k
				for c.timer <= 0 {
					switch i {
					case 2:
						c.timer += (2048 - c.freq) * 8
						if g.io16(0x70)>>5&1 != 0 {
							c.pos = (c.pos + 1) & 63
						} else {
							c.pos = (c.pos + 1) & 31
						}
					case 3:
						v := a.io(0x7c)
						c.timer += (noiseDiv[v&7] << (v >> 4 & 15)) * 4
						x := (c.lfsr ^ c.lfsr>>1) & 1
						c.lfsr = c.lfsr>>1 | x<<14
						if v&8 != 0 {
							c.lfsr = c.lfsr&^0x40 | x<<6
						}
					default:
						c.timer += (2048 - c.freq) * 16
						c.pos = (c.pos + 1) & 7
					}
				}
			}
			var ps [4]int
			if ch[0].on && (dutyTable[ch[0].duty]>>(7-ch[0].pos))&1 != 0 {
				ps[0] = ch[0].vol
			}
			if ch[1].on && (dutyTable[ch[1].duty]>>(7-ch[1].pos))&1 != 0 {
				ps[1] = ch[1].vol
			}
			if ch[2].on {
				c3l := a.io(0x70)
				dim, bank := c3l>>5&1, c3l>>6&1
				p := bank*32 + ch[2].pos
				if dim != 0 {
					p = ch[2].pos
				}
				byt := int(a.wave[(p>>1)&31])
				s := byt >> 4
				if p&1 != 0 {
					s = byt & 15
				}
				vh := a.io(0x72)
				if vh&0x8000 != 0 {
					ps[2] = (s * 3) >> 2
				} else if vc := vh >> 13 & 3; vc != 0 {
					ps[2] = s >> (vc - 1)
				}
			}
			if ch[3].on && ch[3].lfsr&1 == 0 {
				ps[3] = ch[3].vol
			}
			cl, ch2 := a.io(0x80), a.io(0x82)
			ipl, ipr := 0, 0
			for i := 0; i < 4; i++ {
				if cl&(0x1000<<i) != 0 {
					ipl += ps[i]
				}
				if cl&(0x100<<i) != 0 {
					ipr += ps[i]
				}
			}
			ratio := ratioTable[ch2&3]
			pl := float64(ipl) * (float64((cl>>4&7)+1) / 8 * ratio)
			pr := float64(ipr) * (float64((cl&7)+1) / 8 * ratio)
			L = pl / 60 * 0.5
			R = pr / 60 * 0.5
			va, vb := 0.5, 0.5
			if ch2&4 != 0 {
				va = 1
			}
			if ch2&8 != 0 {
				vb = 1
			}
			fa := float64(a.fifoOut[0]) / 128 * va
			fb := float64(a.fifoOut[1]) / 128 * vb
			if ch2&0x200 != 0 {
				L += fa * 0.5
			}
			if ch2&0x100 != 0 {
				R += fa * 0.5
			}
			if ch2&0x2000 != 0 {
				L += fb * 0.5
			}
			if ch2&0x1000 != 0 {
				R += fb * 0.5
			}
		}
		a.sumL += L * float64(k)
		a.sumR += R * float64(k)
		a.n += k
		a.acc += float64(k)
		for a.acc >= step {
			a.acc -= step
			l, r := 0.0, 0.0
			if a.n != 0 {
				l, r = a.sumL/float64(a.n), a.sumR/float64(a.n)
			}
			a.sumL, a.sumR, a.n = 0, 0, 0
			a.hpL = 0.998 * (a.hpL + l - a.prevL)
			a.prevL = l
			a.hpR = 0.998 * (a.hpR + r - a.prevR)
			a.prevR = r
			if len(a.out) < apuOutMax-1 {
				a.out = append(a.out, float32(a.hpL), float32(a.hpR))
			}
		}
	}
}

func (a *apu) state(s *sio) {
	for i := range a.ch {
		c := &a.ch[i]
		s.Bools(&c.on, &c.dac, &c.lenOn, &c.sweepNeg, &c.sweepOn)
		s.Ints(&c.len, &c.vol, &c.envDir, &c.envPeriod, &c.envTimer, &c.envInit, &c.freq, &c.timer, &c.pos, &c.duty, &c.lfsr,
			&c.sweepPeriod, &c.sweepTimer, &c.sweepShift, &c.shadow)
	}
	s.Bytes(a.wave[:])
	for k := 0; k < 2; k++ {
		b := make([]byte, 32)
		for i, v := range a.fifo[k] {
			b[i] = byte(v)
		}
		s.Bytes(b)
		for i := range b {
			a.fifo[k][i] = int8(b[i])
		}
	}
	s.Ints(&a.fifoR[0], &a.fifoR[1], &a.fifoW[0], &a.fifoW[1], &a.fifoN[0], &a.fifoN[1], &a.fifoOut[0], &a.fifoOut[1], &a.seqTimer, &a.seqStep, &a.n)
	s.I64(&a.last)
	for _, f := range []*float64{&a.acc, &a.sumL, &a.sumR, &a.hpL, &a.hpR, &a.prevL, &a.prevR} {
		s.F64(f)
	}
}
