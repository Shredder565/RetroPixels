package snes

// The sound unit: SPC700 + DSP + timers + RetroPixels' own upload program in
// place of the IPL ROM (APU in web/emu/snes.js).

type apu struct {
	ram     [0x10000]uint8
	dsp     *sdsp
	spc     spc700
	portIn  [4]uint8
	portOut [4]uint8
	tTarget [3]int
	tStage  [3]int
	tOut    [3]int
	tDiv    [3]int
	samples [4096]int16
	nSamp   int

	control, dspAddr                            int
	cycles, sampleDiv                           int
	ipl                                         bool
	iplState, iplAddr, iplIdx, iplSeen, iplWait int
}

func newAPU() *apu {
	a := &apu{}
	a.dsp = newSDSP(a.ram[:])
	a.spc.apu = a
	a.reset()
	return a
}

func (a *apu) reset() {
	a.ram = [0x10000]uint8{}
	a.dsp.reset()
	a.spc.reset()
	a.portIn, a.portOut = [4]uint8{}, [4]uint8{}
	a.tTarget, a.tStage, a.tOut, a.tDiv = [3]int{}, [3]int{}, [3]int{}, [3]int{}
	a.control, a.dspAddr = 0x80, 0
	a.cycles, a.sampleDiv = 0, 0
	a.enterIpl()
}

func (a *apu) enterIpl() {
	a.ipl = true
	a.iplState, a.iplAddr, a.iplIdx, a.iplSeen, a.iplWait = 0, 0, 0, -1, 0
}

func (a *apu) iplStep() {
	pi, po := &a.portIn, &a.portOut
	if int(pi[0]) != a.iplSeen {
		a.iplSeen = int(pi[0])
		a.iplWait = 3
		return
	}
	if a.iplWait > 0 {
		a.iplWait--
		return
	}
	switch a.iplState {
	case 0:
		for i := 0; i < 0xf0; i++ {
			a.ram[i] = 0
		}
		po[0], po[1] = 0xaa, 0xbb
		a.iplState = 1
		return
	case 1:
		if pi[0] == 0xcc {
			a.iplCommand()
		}
		return
	case 2:
		if pi[0] != 0 {
			return
		}
		a.iplIdx = 0
		a.iplState = 3
		fallthrough
	case 3:
		p := int(pi[0])
		if p == a.iplIdx&0xff {
			a.ram[(a.iplAddr+a.iplIdx)&0xffff] = pi[1]
			po[0] = uint8(p)
			a.iplIdx++
		} else if (a.iplIdx-p)&0x80 != 0 {
			a.iplCommand()
		}
	}
}

func (a *apu) iplCommand() {
	pi := &a.portIn
	a.iplAddr = int(pi[2]) | int(pi[3])<<8
	a.portOut[0] = pi[0]
	if pi[1] != 0 {
		a.iplState = 2
	} else { // run the uploaded program
		a.ipl = false
		s := &a.spc
		s.pc, s.sp, s.a, s.x, s.y, s.psw = a.iplAddr, 0xef, 0, 0, 0, 0x02
		s.stopped = false
	}
}

func (a *apu) read(ad int) uint8 {
	if ad >= 0xf0 && ad < 0x100 {
		switch ad {
		case 0xf2:
			return uint8(a.dspAddr)
		case 0xf3:
			return a.dsp.read(a.dspAddr & 0x7f)
		case 0xf4, 0xf5, 0xf6, 0xf7:
			return a.portIn[ad-0xf4]
		case 0xfd, 0xfe, 0xff:
			v := a.tOut[ad-0xfd]
			a.tOut[ad-0xfd] = 0
			return uint8(v)
		case 0xf0, 0xf1, 0xfa, 0xfb, 0xfc:
			return 0
		}
	}
	return a.ram[ad]
}

func (a *apu) write(ad int, v uint8) {
	if ad >= 0xf0 && ad < 0x100 {
		switch ad {
		case 0xf1:
			for t := 0; t < 3; t++ {
				if int(v)&(1<<t) != 0 && a.control&(1<<t) == 0 {
					a.tStage[t] = 0
					a.tOut[t] = 0
				}
			}
			if v&0x10 != 0 {
				a.portIn[0], a.portIn[1] = 0, 0
			}
			if v&0x20 != 0 {
				a.portIn[2], a.portIn[3] = 0, 0
			}
			a.control = int(v)
			return
		case 0xf2:
			a.dspAddr = int(v)
			return
		case 0xf3:
			if a.dspAddr < 0x80 {
				a.dsp.write(a.dspAddr, v)
			}
			return
		case 0xf4, 0xf5, 0xf6, 0xf7:
			a.portOut[ad-0xf4] = v
			return
		case 0xfa, 0xfb, 0xfc:
			t := int(v)
			if t == 0 {
				t = 256
			}
			a.tTarget[ad-0xfa] = t
			return
		}
	}
	a.ram[ad] = v
}

// run runs the sound unit to SPC cycle target.
func (a *apu) run(target int) {
	s := &a.spc
	for a.cycles < target {
		var n int
		if a.ipl {
			a.iplStep()
			n = 4
		} else {
			if a.control&0x80 != 0 && s.pc >= 0xffc0 {
				a.enterIpl()
				continue
			}
			n = s.step()
		}
		a.cycles += n
		for t := 0; t < 3; t++ {
			a.tDiv[t] += n
			per := 128
			if t == 2 {
				per = 16
			}
			for a.tDiv[t] >= per {
				a.tDiv[t] -= per
				if a.control&(1<<t) != 0 {
					st := a.tStage[t] + 1 // (++ on a Uint16Array compares the unwrapped value)
					a.tStage[t] = st & 0xffff
					tg := a.tTarget[t]
					if tg == 0 {
						tg = 256
					}
					if st >= tg {
						a.tStage[t] = 0
						a.tOut[t] = (a.tOut[t] + 1) & 15
					}
				}
			}
		}
		a.sampleDiv += n
		for a.sampleDiv >= 32 {
			a.sampleDiv -= 32
			a.dsp.sample()
			if a.nSamp < len(a.samples)-2 {
				a.samples[a.nSamp] = a.dsp.out[0]
				a.samples[a.nSamp+1] = a.dsp.out[1]
				a.nSamp += 2
			}
		}
	}
}

func (a *apu) state(s *sio) {
	s.Bytes(a.ram[:])
	a.dsp.state(s)
	st := a.spc.state()
	for i := range st {
		s.Int(&st[i])
	}
	if !s.save {
		a.spc.setState(st)
	}
	s.Bytes(a.portIn[:])
	s.Bytes(a.portOut[:])
	s.IntSlice(a.tTarget[:])
	s.IntSlice(a.tStage[:])
	s.IntSlice(a.tOut[:])
	s.IntSlice(a.tDiv[:])
	s.Ints(&a.control, &a.dspAddr, &a.cycles, &a.sampleDiv)
	s.Bool(&a.ipl)
	s.Ints(&a.iplState, &a.iplAddr, &a.iplIdx, &a.iplSeen, &a.iplWait)
	if !s.save {
		a.nSamp = 0
	}
}
