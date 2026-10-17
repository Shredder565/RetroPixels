package konami

import (
	"math"

	"retropixels/cores/arcade"
	"retropixels/cores/upd7759"
)

// ---------------------------------------------------------------- K007232

// K007232 is the two-channel PCM chip (k007232.js).
type K007232 struct {
	clock  float64
	rom    []byte
	OnPort func(v uint8)
	regs   [16]uint8
	addr   [2]int
	cnt    [2]int
	on     [2]uint8
	vol    [4]uint8
	Bank   [2]int
	Out    [2]float64
	Acc    [2]float64
	AccN   int
	clk    int
}

// NewK007232 makes the chip.
func NewK007232(clock float64, rom []byte, onPort func(v uint8)) *K007232 {
	if clock == 0 {
		clock = 3579545
	}
	if len(rom) == 0 {
		rom = make([]byte, 1)
	}
	k := &K007232{clock: clock, rom: rom, OnPort: onPort}
	k.Reset()
	return k
}

func (k *K007232) Reset() {
	k.regs = [16]uint8{}
	k.addr, k.cnt, k.on = [2]int{}, [2]int{}, [2]uint8{}
	k.clk = 0
	k.vol = [4]uint8{255, 255, 255, 255}
}

// SetVolume sets a channel's left / right volume (0-255; JS stores & 255).
func (k *K007232) SetVolume(ch, l, r int) { k.vol[ch*2], k.vol[ch*2+1] = uint8(l), uint8(r) }

func (k *K007232) pitch(ch int) int { b := ch * 6; return int(k.regs[b]) | int(k.regs[b+1]&15)<<8 }

func (k *K007232) startAddr(ch int) int {
	b := ch * 6
	return int(k.regs[b+2]) | int(k.regs[b+3])<<8 | int(k.regs[b+4]&1)<<16
}

func (k *K007232) start(ch int) {
	k.addr[ch] = k.startAddr(ch)
	k.cnt[ch] = k.pitch(ch)
	k.on[ch] = 1
}

func (k *K007232) Write(r int, v uint8) {
	r &= 15
	k.regs[r] = v
	if r == 5 || r == 11 {
		k.start(r / 6)
	} else if r == 12 && k.OnPort != nil {
		k.OnPort(v)
	}
}

func (k *K007232) Read(r int) uint8 {
	r &= 15
	if r == 5 || r == 11 {
		k.start(r / 6)
	}
	return 0
}

func (k *K007232) sampleAt(ch int) uint8 { return k.rom[(k.addr[ch]+k.Bank[ch])%len(k.rom)] }

// Tick is one native step (clock / 128).
func (k *K007232) Tick() {
	L, R := 0.0, 0.0
	for ch := 0; ch < 2; ch++ {
		if k.on[ch] == 0 {
			continue
		}
		k.cnt[ch] += 32
		span := 0x1000 - k.pitch(ch)
		for k.cnt[ch] >= 0x1000 {
			k.cnt[ch] -= span
			k.addr[ch] = (k.addr[ch] + 1) & 0x1ffff
			if k.sampleAt(ch)&0x80 != 0 {
				break
			}
		}
		s := k.sampleAt(ch)
		if s&0x80 != 0 {
			if k.regs[13]&(1<<ch) != 0 {
				k.addr[ch] = k.startAddr(ch)
				s = k.sampleAt(ch)
				if s&0x80 != 0 {
					k.on[ch] = 0
					continue
				}
			} else {
				k.on[ch] = 0
				continue
			}
		}
		v := float64(int(s&0x7f)-0x40) / 64
		L += v * float64(k.vol[ch*2]) / 255
		R += v * float64(k.vol[ch*2+1]) / 255
	}
	k.Out[0], k.Out[1] = L, R
}

// Clocks runs n input clocks, summing samples into Acc / AccN.
func (k *K007232) Clocks(n int) {
	k.clk += n
	for k.clk >= 128 {
		k.clk -= 128
		k.Tick()
		k.Acc[0] += k.Out[0]
		k.Acc[1] += k.Out[1]
		k.AccN++
	}
}

func (k *K007232) AppendState(w *arcade.StateW) {
	w.Bytes(k.regs[:])
	w.Ints(k.addr[:])
	w.Ints(k.cnt[:])
	w.Bytes(k.on[:])
	w.Bytes(k.vol[:])
	w.Ints(k.Bank[:])
	w.Int(k.clk)
	w.F64s(k.Out[:])
	w.F64s(k.Acc[:])
	w.Int(k.AccN)
}

// LoadState reads what AppendState wrote (which, unlike the JS state, also
// keeps the last output, so a loaded state carries on exactly).
func (k *K007232) LoadState(r *arcade.StateR) {
	r.Bytes(k.regs[:])
	r.Ints(k.addr[:])
	r.Ints(k.cnt[:])
	r.Bytes(k.on[:])
	r.Bytes(k.vol[:])
	r.Ints(k.Bank[:])
	k.clk = r.Int()
	r.F64s(k.Out[:])
	r.F64s(k.Acc[:])
	k.AccN = r.Int()
}

// ---------------------------------------------------------------- K053260

var adpcmStep = [16]int{0, 1, 2, 4, 8, 16, 32, 64, -128, -64, -32, -16, -8, -4, -2, -1}

// K053260 is the four-channel PCM chip (k053260.js).
type K053260 struct {
	ROM     []byte
	clock   float64
	regs    [0x30]uint8
	ports   [4]uint8
	pos     [4]float64
	frac    [4]float64
	acc     [4]int32
	playing [4]uint8
	romRead int
}

// NewK053260 makes the chip.
func NewK053260(rom []byte, clock float64) *K053260 {
	if len(rom) == 0 {
		rom = make([]byte, 1)
	}
	if clock == 0 {
		clock = 3579545
	}
	return &K053260{ROM: rom, clock: clock}
}

func (k *K053260) pitch(n int) int {
	b := 8 + n*8
	return int(k.regs[b]) | int(k.regs[b+1]&15)<<8
}
func (k *K053260) length(n int) int {
	b := 8 + n*8
	return int(k.regs[b+2]) | int(k.regs[b+3])<<8
}
func (k *K053260) start(n int) int {
	b := 8 + n*8
	return int(k.regs[b+4]) | int(k.regs[b+5])<<8 | int(k.regs[b+6]&0x1f)<<16
}
func (k *K053260) volume(n int) int { return int(k.regs[8+n*8+7] & 0x7f) }
func (k *K053260) pan(n int) int {
	r := int(k.regs[0x2c+(n>>1)])
	if n&1 != 0 {
		return (r >> 3) & 7
	}
	return r & 7
}

// Write: the sound processor's side.
func (k *K053260) Write(reg int, v uint8) {
	reg &= 0x3f
	if reg >= 0x30 {
		return
	}
	if reg == 2 || reg == 3 {
		k.ports[reg] = v
		return
	}
	if reg < 2 {
		return
	}
	if reg == 0x28 {
		old := k.regs[0x28]
		for n := 0; n < 4; n++ {
			bit := uint8(1 << n)
			if v&bit != 0 && old&bit == 0 {
				k.pos[n], k.frac[n], k.acc[n], k.playing[n] = 0, 0, -1, 1
			} else if v&bit == 0 {
				k.playing[n] = 0
			}
		}
	}
	if reg >= 0x08 && reg < 0x10 {
		k.romRead = 0
	}
	k.regs[reg] = v
}

// Read: the sound processor's side.
func (k *K053260) Read(reg int) uint8 {
	reg &= 0x3f
	if reg < 2 {
		return k.ports[reg]
	}
	if reg == 0x29 {
		s := uint8(0)
		for n := 0; n < 4; n++ {
			if k.playing[n] != 0 {
				s |= 1 << n
			}
		}
		return s
	}
	if reg == 0x2e {
		if k.regs[0x2f]&1 == 0 {
			return 0
		}
		a := (k.start(0) + k.romRead) % len(k.ROM)
		k.romRead++
		return k.ROM[a]
	}
	return 0
}

func (k *K053260) MainWrite(port int, v uint8) { k.ports[port&1] = v }
func (k *K053260) MainRead(port int) uint8     { return k.ports[2+(port&1)] }

func (k *K053260) nib(start, p int, a int32) int32 {
	b := k.ROM[(start+(p>>1))%len(k.ROM)]
	n := b & 15
	if p&1 != 0 {
		n = b >> 4
	}
	return (a + int32(adpcmStep[n])) & 0xff
}

// Mix adds n mono samples at outRate into out.
func (k *K053260) Mix(out []float32, offset, n int, outRate float64) {
	if k.regs[0x2f]&2 == 0 {
		return
	}
	rom, size := k.ROM, len(k.ROM)
	for c := 0; c < 4; c++ {
		if k.playing[c] == 0 {
			continue
		}
		gain := float64(k.volume(c)) / 127 * math.Float64frombits(k053260PanBits[k.pan(c)]) / 128 * 0.35
		if gain == 0 {
			k.advanceSilent(c, n, outRate)
			continue
		}
		step := k.clock / float64(4096-k.pitch(c)) / outRate
		adpcm, loop := (k.regs[0x2a]>>(4+c))&1 != 0, (k.regs[0x2a]>>c)&1 != 0
		start, ln := k.start(c), k.length(c)
		end := ln
		if adpcm {
			end = ln * 2
		}
		pos, frac, acc := int(k.pos[c]), k.frac[c], k.acc[c]
		if adpcm && acc < 0 {
			acc = k.nib(start, pos, 0)
		}
		var cur int
		if adpcm {
			cur = int(int8(uint8(acc)))
		} else {
			cur = int(int8(rom[(start+pos)%size]))
		}
		for i := 0; i < n; i++ {
			out[offset+i] = float32(float64(out[offset+i]) + float64(cur)*gain)
			frac += step
			for frac >= 1 {
				frac--
				pos++
				if pos >= end {
					if !loop {
						k.playing[c] = 0
						break
					}
					pos, acc = 0, 0
				}
				if adpcm {
					acc = k.nib(start, pos, acc)
					cur = int(int8(uint8(acc)))
				} else {
					cur = int(int8(rom[(start+pos)%size]))
				}
			}
			if k.playing[c] == 0 {
				break
			}
		}
		k.pos[c], k.frac[c], k.acc[c] = float64(pos), frac, acc
	}
}

func (k *K053260) advanceSilent(c, n int, outRate float64) {
	adpcm, loop := (k.regs[0x2a]>>(4+c))&1 != 0, (k.regs[0x2a]>>c)&1 != 0
	end := k.length(c)
	if adpcm {
		end *= 2
	}
	t := k.frac[c] + float64(n)*k.clock/float64(4096-k.pitch(c))/outRate
	pos := k.pos[c] + math.Floor(t)
	k.frac[c] = t - math.Floor(t)
	if pos >= float64(end) {
		if loop && end != 0 {
			pos = math.Mod(pos, float64(end))
		} else {
			k.playing[c] = 0
		}
	}
	k.pos[c] = pos
}

// Reset: as making the chip again with the same ROM.
func (k *K053260) Reset() { *k = K053260{ROM: k.ROM, clock: k.clock} }

func (k *K053260) AppendState(w *arcade.StateW) {
	w.Bytes(k.regs[:])
	w.Bytes(k.ports[:])
	w.F64s(k.pos[:])
	w.F64s(k.frac[:])
	for _, v := range k.acc {
		w.Int(int(v))
	}
	w.Bytes(k.playing[:])
	w.Int(k.romRead)
}

func (k *K053260) LoadState(r *arcade.StateR) {
	r.Bytes(k.regs[:])
	r.Bytes(k.ports[:])
	r.F64s(k.pos[:])
	r.F64s(k.frac[:])
	for i := range k.acc {
		k.acc[i] = int32(r.Int())
	}
	r.Bytes(k.playing[:])
	k.romRead = r.Int()
}

// ---------------------------------------------------------------- uPD7759

// UPD7759 is the NEC ADPCM speech chip in stand-alone mode (upd7759.js); it
// now lives in its own package, shared with the Sega boards.
type UPD7759 = upd7759.UPD7759

// NewUPD7759 makes the chip.
func NewUPD7759(clock float64, rom []byte) *UPD7759 { return upd7759.New(clock, rom) }

// ---------------------------------------------------------------- 93C46

// Eeprom93C46 is the serial EEPROM (eeprom93c46.js): 64 words of 16 bits,
// or 128 bytes.
type Eeprom93C46 struct {
	wordBits, words, addrBits int
	write11                   bool
	Bytes                     [128]uint8
	cs, clk, di, dout         int
	shift, bits, state        int
	addr, out, outBits        int
	enabled, writeAll         bool
	Dirty                     bool
}

// NewEeprom93C46 makes the chip with initial contents (nil: blank). wordBits
// 16 or 8; addrBits 0 for the organisation's own.
func NewEeprom93C46(initial []byte, wordBits, addrBits int, write11 bool) *Eeprom93C46 {
	e := &Eeprom93C46{wordBits: 16, words: 64, write11: write11, dout: 1}
	if wordBits == 8 {
		e.wordBits, e.words = 8, 128
	}
	e.addrBits = addrBits
	if e.addrBits == 0 {
		e.addrBits = 6
		if e.wordBits == 8 {
			e.addrBits = 7
		}
	}
	for i := range e.Bytes {
		e.Bytes[i] = 0xff
	}
	copy(e.Bytes[:], initial)
	return e
}

func (e *Eeprom93C46) word(a int) int {
	a &= e.words - 1
	if e.wordBits == 8 {
		return int(e.Bytes[a])
	}
	return int(e.Bytes[a*2])<<8 | int(e.Bytes[a*2+1])
}

func (e *Eeprom93C46) setWord(a, v int) {
	a &= e.words - 1
	if e.wordBits == 8 {
		v &= 0xff
	} else {
		v &= 0xffff
	}
	if e.word(a) != v {
		e.Dirty = true
	}
	if e.wordBits == 8 {
		e.Bytes[a] = uint8(v)
	} else {
		e.Bytes[a*2], e.Bytes[a*2+1] = uint8(v>>8), uint8(v)
	}
}

func bit(b bool) int {
	if b {
		return 1
	}
	return 0
}

// Write sets the three input lines.
func (e *Eeprom93C46) Write(csB, clkB, diB bool) {
	cs, clk, di := bit(csB), bit(clkB), bit(diB)
	if cs == 0 {
		if e.cs != 0 {
			e.state, e.bits, e.shift = 0, 0, 0
		}
		e.cs, e.clk, e.di, e.dout = 0, clk, di, 1
		return
	}
	if e.cs == 0 {
		e.state, e.bits, e.shift, e.dout = 0, 0, 0, 1
	}
	rising := clk != 0 && e.clk == 0
	e.cs, e.clk, e.di = 1, clk, di
	if rising {
		e.clock(di)
	}
}

func (e *Eeprom93C46) clock(di int) {
	switch e.state {
	case 0:
		if di != 0 {
			e.state, e.bits, e.shift = 1, 0, 0
		}
	case 1:
		e.shift = e.shift<<1 | di
		e.bits++
		if e.bits == 2+e.addrBits {
			e.command(e.shift>>e.addrBits, e.shift&((1<<e.addrBits)-1))
		}
	case 2:
		if e.outBits == 0 {
			e.addr = (e.addr + 1) & (e.words - 1)
			e.out = e.word(e.addr)
			e.outBits = e.wordBits
		}
		e.dout = (e.out >> (e.wordBits - 1)) & 1
		e.out = (e.out << 1) & ((1 << e.wordBits) - 1)
		e.outBits--
	case 3:
		e.shift = (e.shift<<1 | di) & 0xffff
		e.bits++
		if e.bits == e.wordBits {
			if e.writeAll {
				for a := 0; a < e.words; a++ {
					e.setWord(a, e.shift)
				}
			} else {
				e.setWord(e.addr, e.shift)
			}
			e.state, e.dout = 4, 1
		}
	}
}

func (e *Eeprom93C46) command(op, a int) {
	top := a >> (e.addrBits - 2)
	a &= e.words - 1
	e.addr = a
	switch op {
	case 2:
		e.state, e.dout, e.out, e.outBits = 2, 0, e.word(a), e.wordBits
		return
	case 1:
		if !e.enabled {
			e.state = 4
			return
		}
		e.state, e.bits, e.shift, e.writeAll = 3, 0, 0, false
		return
	case 3:
		if e.write11 {
			if !e.enabled {
				e.state = 4
				return
			}
			e.state, e.bits, e.shift, e.writeAll = 3, 0, 0, false
			return
		}
		if e.enabled {
			e.setWord(a, 0xffff)
		}
		e.state, e.dout = 4, 1
		return
	}
	switch top {
	case 0:
		e.enabled = false
	case 1:
		if e.enabled {
			e.state, e.bits, e.shift, e.writeAll = 3, 0, 0, true
			return
		}
	case 2:
		if e.enabled {
			for i := 0; i < e.words; i++ {
				e.setWord(i, 0xffff)
			}
		}
	case 3:
		e.enabled = true
	}
	e.state, e.dout = 4, 1
}

// Read is the data-out line.
func (e *Eeprom93C46) Read() int { return e.dout }

func (e *Eeprom93C46) AppendState(w *arcade.StateW) {
	w.Bytes(e.Bytes[:])
	w.Ints([]int{e.cs, e.clk, e.di, e.dout, e.shift, e.bits, e.state, e.addr, e.out, e.outBits, bit(e.enabled), bit(e.writeAll)})
}

func (e *Eeprom93C46) LoadState(r *arcade.StateR) {
	r.Bytes(e.Bytes[:])
	v := make([]int, 12)
	r.Ints(v)
	e.cs, e.clk, e.di, e.dout, e.shift, e.bits, e.state, e.addr, e.out, e.outBits = v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]
	e.enabled, e.writeAll = v[10] != 0, v[11] != 0
}
