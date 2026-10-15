package snes

// The Sony S-DSP sound generator: a port of web/emu/sdsp.js. One call to
// sample() makes one 32 kHz stereo sample.

var (
	dspRate, dspOffset [32]int
	dspKern            [256 * 4]int
)

func init() {
	for r := 1; r < 32; r++ {
		k := (r - 1) % 3
		if r == 31 {
			dspRate[r] = 1
		} else {
			dspRate[r] = [3]int{2048, 1536, 1280}[k] >> ((r - 1) / 3)
		}
		dspOffset[r] = [3]int{0, 1040, 536}[k]
	}
	// 4-tap interpolation weights (a smooth bell-shaped kernel)
	sigma := 0.62 // (a variable: 2*0.62*0.62 is worked out at run time, in doubles, as JavaScript does)
	for f := 0; f < 256; f++ {
		t := float64(f) / 256
		d := [4]float64{1 + t, t, 1 - t, 2 - t}
		var w [4]float64
		for k := range d {
			w[k] = jsExp(-(d[k] * d[k]) / (2 * sigma * sigma))
		}
		sum := w[0] + w[1] + w[2] + w[3]
		acc := 0
		for k := 0; k < 4; k++ {
			dspKern[f*4+k] = int(jsRound(w[k] / sum * 2048))
			acc += dspKern[f*4+k]
		}
		dspKern[f*4+1] += 2048 - acc
	}
}

func clamp16(v int) int {
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return v
}

const (
	envAttack = iota
	envDecay
	envSustain
	envRelease
)

type sdsp struct {
	ram      []uint8
	regs     [128]uint8
	buf      [8 * 19]int16 // per voice: 3 previous + 16 current decoded samples
	v        [8 * 10]int32 // per voice: brr address, position, envelope, mode, key-on delay, output, active, block header
	echoHist [16]int16
	out      [2]int16

	counter, noise, echoPos, echoLen, firPos, konPending, everyOther int
}

func newSDSP(ram []uint8) *sdsp {
	d := &sdsp{ram: ram}
	d.reset()
	return d
}

func (d *sdsp) reset() {
	d.regs = [128]uint8{}
	d.buf = [8 * 19]int16{}
	d.v = [8 * 10]int32{}
	d.echoHist = [16]int16{}
	d.regs[0x6c] = 0xe0
	for i := 0; i < 8; i++ {
		d.v[i*10+3] = envRelease
	}
	d.counter, d.noise, d.echoPos, d.echoLen, d.firPos = 0, 0x4000, 0, 0, 0
	d.konPending, d.everyOther = 0, 0
}

func (d *sdsp) read(r int) uint8 { return d.regs[r&0x7f] }

func (d *sdsp) write(r int, val uint8) {
	r &= 0x7f
	if r == 0x7c {
		d.regs[r] = 0
		return
	}
	d.regs[r] = val
	if r == 0x4c {
		d.konPending |= int(val)
	}
}

func (d *sdsp) tick(r int) bool {
	return r != 0 && (d.counter+dspOffset[r])%dspRate[r] == 0
}

func (d *sdsp) decodeBlock(i int) {
	V, o, ram, b, bo := &d.v, i*10, d.ram, &d.buf, i*19
	addr := int(V[o])
	hdr := int(ram[addr&0xffff])
	shift, filter := hdr>>4, (hdr>>2)&3
	b[bo], b[bo+1], b[bo+2] = b[bo+16], b[bo+17], b[bo+18]
	p1, p2 := int(b[bo+2])>>1, int(b[bo+1])>>1
	for k := 0; k < 16; k++ {
		byt := int(ram[(addr+1+(k>>1))&0xffff])
		var n int
		if k&1 != 0 {
			n = byt & 15
		} else {
			n = byt >> 4
		}
		if n >= 8 {
			n -= 16
		}
		var s int
		if shift <= 12 {
			s = (n << shift) >> 1
		} else if n < 0 {
			s = -2048
		}
		switch filter {
		case 1:
			s += p1 + ((-p1) >> 4)
		case 2:
			s += 2*p1 + ((-3 * p1) >> 5) - p2 + (p2 >> 4)
		case 3:
			s += 2*p1 + ((-13 * p1) >> 6) - p2 + ((3 * p2) >> 4)
		}
		s = clamp16(s)
		w := int16(s << 1)
		b[bo+3+k] = w
		p2 = p1
		p1 = int(w) >> 1
	}
	V[o+7] = int32(hdr)
}

func (d *sdsp) keyOn(i int) {
	V, o, R := &d.v, i*10, &d.regs
	dir, src := int(R[0x5d])<<8, int(R[i*16+4])
	ent := (dir + src*4) & 0xffff
	V[o] = int32(int(d.ram[ent]) | int(d.ram[(ent+1)&0xffff])<<8)
	V[o+1], V[o+2], V[o+3], V[o+4], V[o+6] = 0, 0, envAttack, 5, 1
	for k := i * 19; k < i*19+19; k++ {
		d.buf[k] = 0
	}
	d.decodeBlock(i)
	R[0x7c] &^= 1 << i
}

func (d *sdsp) envelope(i int) {
	V, o, R, base := &d.v, i*10, &d.regs, i*16
	env, mode := int(V[o+2]), int(V[o+3])
	if mode == envRelease {
		env -= 8
		if env < 0 {
			env = 0
		}
		V[o+2] = int32(env)
		return
	}
	adsr1, adsr2 := int(R[base+5]), int(R[base+6])
	if adsr1&0x80 != 0 {
		switch mode {
		case envAttack:
			r := (adsr1&15)*2 + 1
			if d.tick(r) {
				if r == 31 {
					env += 1024
				} else {
					env += 32
				}
			}
			if env >= 0x7ff {
				env = 0x7ff
				mode = envDecay
			}
		case envDecay:
			r := ((adsr1>>4)&7)*2 + 16
			if d.tick(r) {
				env -= ((env - 1) >> 8) + 1
			}
			if env>>8 == adsr2>>5 {
				mode = envSustain
			}
		default:
			r := adsr2 & 31
			if d.tick(r) {
				env -= ((env - 1) >> 8) + 1
			}
		}
	} else {
		g := int(R[base+7])
		if g&0x80 == 0 {
			env = (g & 0x7f) << 4
		} else {
			r := g & 31
			if d.tick(r) {
				switch (g >> 5) & 3 {
				case 0:
					env -= 32
				case 1:
					env -= ((env - 1) >> 8) + 1
				case 2:
					env += 32
				default:
					if env < 0x600 {
						env += 32
					} else {
						env += 8
					}
				}
			}
		}
	}
	if env < 0 {
		env = 0
	} else if env > 0x7ff {
		env = 0x7ff
	}
	V[o+2], V[o+3] = int32(env), int32(mode)
}

// sample produces one stereo sample into d.out.
func (d *sdsp) sample() {
	R, V, b := &d.regs, &d.v, &d.buf
	if d.counter == 0 {
		d.counter = 0x77ff
	} else {
		d.counter--
	}
	d.everyOther ^= 1
	if d.everyOther != 0 {
		kon := d.konPending
		d.konPending = 0
		R[0x4c] = 0
		koff := int(R[0x5c])
		for i := 0; i < 8; i++ {
			if koff&(1<<i) != 0 {
				V[i*10+3] = envRelease
			}
			if kon&(1<<i) != 0 {
				d.keyOn(i)
			}
		}
	}
	if R[0x6c]&0x80 != 0 {
		for i := 0; i < 8; i++ {
			V[i*10+3] = envRelease
			V[i*10+2] = 0
		}
	}
	if d.tick(int(R[0x6c] & 31)) {
		fb := (d.noise ^ (d.noise >> 1)) & 1
		d.noise = (d.noise >> 1) | (fb << 14)
	}
	pmon, non, eon := int(R[0x2d]), int(R[0x3d]), int(R[0x4d])
	L, Rr, eL, eR, prevOut := 0, 0, 0, 0, 0
	for i := 0; i < 8; i++ {
		o, base := i*10, i*16
		if V[o+6] == 0 {
			R[base+8], R[base+9] = 0, 0
			prevOut = 0
			continue
		}
		if V[o+4] > 0 {
			V[o+4]--
			R[base+8], R[base+9] = 0, 0
			prevOut = 0
			continue
		}
		pitch := (int(R[base+2]) | int(R[base+3])<<8) & 0x3fff
		if pmon&(1<<i) != 0 && i > 0 {
			pitch += ((prevOut >> 5) * pitch) >> 10
			if pitch < 0 {
				pitch = 0
			} else if pitch > 0x3fff {
				pitch = 0x3fff
			}
		}
		pos := int(V[o+1])
		idx, f := pos>>12, (pos>>4)&0xff
		bo := i*19 + idx
		var s int
		if non&(1<<i) != 0 {
			s = int(int16(d.noise << 1))
		} else {
			s = (dspKern[f*4]*int(b[bo]) + dspKern[f*4+1]*int(b[bo+1]) + dspKern[f*4+2]*int(b[bo+2]) + dspKern[f*4+3]*int(b[bo+3])) >> 11
			s = clamp16(s) &^ 1
		}
		d.envelope(i)
		env := int(V[o+2])
		out := (s * env) >> 11
		V[o+5] = int32(out)
		prevOut = out
		R[base+8] = uint8(env >> 4)
		R[base+9] = uint8((out >> 8) & 0xff)
		vl, vr := int(int8(R[base])), int(int8(R[base+1]))
		ol, or := (out*vl)>>7, (out*vr)>>7
		L += ol
		Rr += or
		if eon&(1<<i) != 0 {
			eL += ol
			eR += or
		}
		np := pos + pitch
		for np>>12 >= 16 {
			np -= 16 << 12
			hdr := V[o+7]
			if hdr&1 != 0 {
				R[0x7c] |= 1 << i
				if hdr&2 != 0 {
					dir := int(R[0x5d]) << 8
					ent := (dir + int(R[base+4])*4 + 2) & 0xffff
					V[o] = int32(int(d.ram[ent]) | int(d.ram[(ent+1)&0xffff])<<8)
				} else {
					V[o+3], V[o+2], V[o+6] = envRelease, 0, 0
				}
			} else {
				V[o] = (V[o] + 9) & 0xffff
			}
			if V[o+6] != 0 {
				d.decodeBlock(i)
			}
		}
		V[o+1] = int32(np)
	}
	L, Rr = clamp16(L), clamp16(Rr)
	// echo
	ram, esa := d.ram, int(R[0x6d])<<8
	ea := (esa + d.echoPos) & 0xffff
	inL := int(int16(int(ram[ea])|int(ram[(ea+1)&0xffff])<<8)) >> 1
	inR := int(int16(int(ram[(ea+2)&0xffff])|int(ram[(ea+3)&0xffff])<<8)) >> 1
	H := &d.echoHist
	d.firPos = (d.firPos + 1) & 7
	H[d.firPos], H[8+d.firPos] = int16(inL), int16(inR)
	fl, fr := 0, 0
	for k := 0; k < 8; k++ {
		c, hp := int(int8(R[k*16+0x0f])), (d.firPos+k+1)&7
		fl += (int(H[hp]) * c) >> 6
		fr += (int(H[8+hp]) * c) >> 6
	}
	fl, fr = clamp16(fl)&^1, clamp16(fr)&^1
	mvl, mvr, evl, evr := int(int8(R[0x0c])), int(int8(R[0x1c])), int(int8(R[0x2c])), int(int8(R[0x3c]))
	outL := clamp16(((L * mvl) >> 7) + ((fl * evl) >> 7))
	outR := clamp16(((Rr * mvr) >> 7) + ((fr * evr) >> 7))
	if R[0x6c]&0x40 != 0 {
		outL, outR = 0, 0
	}
	d.out[0], d.out[1] = int16(outL), int16(outR)
	if R[0x6c]&0x20 == 0 {
		efb := int(int8(R[0x0d]))
		wl, wr := clamp16(eL+((fl*efb)>>7))&^1, clamp16(eR+((fr*efb)>>7))&^1
		ram[ea] = uint8(wl)
		ram[(ea+1)&0xffff] = uint8(wl >> 8)
		ram[(ea+2)&0xffff] = uint8(wr)
		ram[(ea+3)&0xffff] = uint8(wr >> 8)
	}
	if d.echoPos == 0 {
		d.echoLen = int(R[0x7d]&15) * 2048
		if d.echoLen == 0 {
			d.echoLen = 4
		}
	}
	d.echoPos += 4
	if d.echoPos >= d.echoLen {
		d.echoPos = 0
	}
}

func (d *sdsp) state(s *sio) {
	s.Bytes(d.regs[:])
	for i := range d.buf {
		x := int(d.buf[i])
		s.Int(&x)
		d.buf[i] = int16(x)
	}
	for i := range d.v {
		x := int(d.v[i])
		s.Int(&x)
		d.v[i] = int32(x)
	}
	for i := range d.echoHist {
		x := int(d.echoHist[i])
		s.Int(&x)
		d.echoHist[i] = int16(x)
	}
	s.Ints(&d.counter, &d.noise, &d.echoPos, &d.echoLen, &d.firPos, &d.konPending, &d.everyOther)
}
