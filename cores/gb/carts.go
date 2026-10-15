package gb

import (
	"encoding/binary"
	"fmt"
	"math"
	"time"
)

// The cartridge chips (a port of web/emu/gb_carts.js): the memory bank
// controllers and the extra hardware some cartridges carry: MBC1 (and MBC1
// multi-game carts), MBC2, MBC3 with its clock, MBC30, MBC5 (and rumble),
// MBC6 (with its flash memory), MBC7 (tilt sensor + EEPROM), HuC1 (infrared
// port left dark), HuC3 (with its clock), MMM01, Bandai TAMA5 and the Pocket
// Camera (no real camera: it photographs a built-in test card).

var ramSizes = [...]int{0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000}

// wallClock is the computer's clock in milliseconds (Date.now() in the web
// core), for the cartridge clocks.
func wallClock() float64 { return float64(time.Now().UnixMilli()) }

func (c *cartBase) dateNow() float64 { return c.gb.dateNow() }

// emuFrameMs: one frame of emulated time, in milliseconds.
const emuFrameMs = 70224 * 1000 / float64(clockHz)

type cart interface {
	base() *cartBase
	romRead(a int) int
	romWrite(a, v int)
	ramRead(a int) int
	ramWrite(a, v int)
	frame()             // once per frame
	extraSave() []byte  // what the battery save keeps besides the RAM (clock, flash...), or nil
	loadExtra(x []byte) // the reverse
	state(s *sio)
}

// cartBase is the common part (Cart in gb_carts.js).
type cartBase struct {
	rom         []byte
	gb          *GB
	typ         int
	kind        string
	ramSize     int
	romBanks    int
	ram         []byte
	ramOn       bool
	rom0, romX  int
	ramOff      int
	battery     bool
	cgb         bool
	hasRumble   bool
	rumbleLevel float64
}

func newCartBase(rom []byte, gb *GB, kind string) cartBase {
	c := cartBase{rom: rom, gb: gb, typ: int(rom[0x147]), kind: kind}
	if n := int(rom[0x149]); n < len(ramSizes) {
		c.ramSize = ramSizes[n]
	}
	c.romBanks = max(2, (len(rom)+0x3fff)/0x4000)
	c.ram = make([]byte, c.ramSize)
	c.romX = 0x4000
	switch c.typ {
	case 3, 6, 9, 0x0d, 0x0f, 0x10, 0x13, 0x1b, 0x1e, 0x22, 0xfc, 0xfd, 0xfe, 0xff:
		c.battery = true
	}
	c.cgb = rom[0x143]&0x80 != 0
	return c
}

func (c *cartBase) base() *cartBase { return c }

// romAt: a ROM byte (0 past the end of a short file).
func (c *cartBase) romAt(i int) int {
	if i < len(c.rom) {
		return int(c.rom[i])
	}
	return 0
}

func (c *cartBase) bankOffset(bank int) int { return (bank % c.romBanks) * 0x4000 }

func (c *cartBase) romRead(a int) int {
	if a < 0x4000 {
		return c.romAt(c.rom0 + a)
	}
	return c.romAt(c.romX + (a & 0x3fff))
}

func (c *cartBase) romWrite(a, v int) {}

func (c *cartBase) ramRead(a int) int {
	if !c.ramOn || len(c.ram) == 0 {
		return 0xff
	}
	return int(c.ram[(c.ramOff+(a&0x1fff))%len(c.ram)])
}

func (c *cartBase) ramWrite(a, v int) {
	if !c.ramOn || len(c.ram) == 0 {
		return
	}
	c.ram[(c.ramOff+(a&0x1fff))%len(c.ram)] = byte(v)
}

func (c *cartBase) frame()             {}
func (c *cartBase) extraSave() []byte  { return nil }
func (c *cartBase) loadExtra(x []byte) {}

// now: real time in CPU clocks (the Game Boy counts it; double speed doesn't change it).
func (c *cartBase) now() int64 {
	if c.gb == nil {
		return 0
	}
	return c.gb.clock
}

func (c *cartBase) state(s *sio) {
	s.Bytes(c.ram)
	s.Bool(&c.ramOn)
	s.Ints(&c.rom0, &c.romX, &c.ramOff)
	s.F64(&c.rumbleLevel)
}

// cartSram: the battery save: the RAM, plus whatever extra the chip keeps.
func cartSram(c cart) []byte {
	b := c.base()
	if !b.battery {
		return nil
	}
	out := append([]byte{}, b.ram...)
	if extra := c.extraSave(); extra != nil {
		out = append(out, extra...)
	}
	return out
}

func cartLoadSram(c cart, s []byte) {
	b := c.base()
	copy(b.ram, s)
	if len(s) > len(b.ram) {
		c.loadExtra(s[len(b.ram):])
	}
}

// createCart picks the chip from the cartridge header.
func createCart(rom []byte, gb *GB) (cart, error) {
	t := int(rom[0x147])
	switch {
	case t == 0 || t == 8 || t == 9:
		c := &plainCart{newCartBase(rom, gb, "Cart")}
		c.ramOn = true
		return c, nil
	case t >= 1 && t <= 3:
		return newMBC1(rom, gb), nil
	case t == 5 || t == 6:
		return newMBC2(rom, gb), nil
	case t >= 0x0b && t <= 0x0d:
		return newMMM01(rom, gb), nil
	case t >= 0x0f && t <= 0x13:
		return newMBC3(rom, gb), nil
	case t >= 0x19 && t <= 0x1e:
		return newMBC5(rom, gb), nil
	case t == 0x20:
		return newMBC6(rom, gb), nil
	case t == 0x22:
		return newMBC7(rom, gb), nil
	case t == 0xfc:
		return newCamera(rom, gb), nil
	case t == 0xfd:
		return newTAMA5(rom, gb), nil
	case t == 0xfe:
		return newHuC3(rom, gb), nil
	case t == 0xff:
		return newHuC1(rom, gb), nil
	}
	return nil, fmt.Errorf("This Game Boy game uses a cartridge chip RetroPixels doesn't support yet (type $%x).", t)
}

// plainCart: ROM (and RAM) with no bank controller.
type plainCart struct{ cartBase }

// ===================================================================
// MBC1 (and MBC1M: multi-game carts wired with a 4-bit low bank)
// ===================================================================

type mbc1 struct {
	cartBase
	multi              bool
	bank1, bank2, mode int
}

func newMBC1(rom []byte, gb *GB) *mbc1 {
	c := &mbc1{cartBase: newCartBase(rom, gb, "MBC1")}
	// A 1 MB MBC1 cart with Nintendo's logo at the start of a 256 KB quarter is a multi-game cart.
	if len(rom) == 0x100000 {
		logos := 0
		for q := 1; q < 4; q++ {
			same := true
			for i := 0; i < 48; i++ {
				if rom[q*0x40000+0x104+i] != rom[0x104+i] {
					same = false
					break
				}
			}
			if same {
				logos++
			}
		}
		c.multi = logos >= 2
	}
	c.bank1 = 1
	c.update()
	return c
}

func (c *mbc1) update() {
	s, low := 5, c.bank1
	if c.multi {
		s, low = 4, c.bank1&15
	}
	c.romX = c.bankOffset((c.bank2 << s) | low)
	c.rom0, c.ramOff = 0, 0
	if c.mode != 0 {
		c.rom0 = c.bankOffset(c.bank2 << s)
		c.ramOff = c.bank2 * 0x2000
	}
}

func (c *mbc1) romWrite(a, v int) {
	switch {
	case a < 0x2000:
		c.ramOn = v&0x0f == 0x0a
	case a < 0x4000:
		c.bank1 = v & 0x1f
		if c.bank1 == 0 {
			c.bank1 = 1
		}
	case a < 0x6000:
		c.bank2 = v & 3
	default:
		c.mode = v & 1
	}
	c.update()
}

func (c *mbc1) state(s *sio) {
	c.cartBase.state(s)
	s.Bool(&c.multi)
	s.Ints(&c.bank1, &c.bank2, &c.mode)
}

// ===================================================================
// MBC2: 16 ROM banks and 512 half-bytes of built-in RAM
// ===================================================================

type mbc2 struct{ cartBase }

func newMBC2(rom []byte, gb *GB) *mbc2 {
	c := &mbc2{newCartBase(rom, gb, "MBC2")}
	c.ram, c.ramSize = make([]byte, 512), 512
	return c
}

func (c *mbc2) romWrite(a, v int) {
	if a >= 0x4000 {
		return
	}
	if a&0x100 != 0 {
		b := v & 0x0f
		if b == 0 {
			b = 1
		}
		c.romX = c.bankOffset(b)
	} else {
		c.ramOn = v&0x0f == 0x0a
	}
}

func (c *mbc2) ramRead(a int) int {
	if c.ramOn {
		return 0xf0 | int(c.ram[a&0x1ff])
	}
	return 0xff
}

func (c *mbc2) ramWrite(a, v int) {
	if c.ramOn {
		c.ram[a&0x1ff] = byte(v & 0x0f)
	}
}

// ===================================================================
// Real-time clock of the MBC3 (counts in emulated time; catches up with
// the computer's clock after a pause and between play sessions)
// ===================================================================

type rtcClock struct {
	s, m, h, d  int
	halt, carry bool
	sub         int64   // clocks into the current second
	last        int64   // emulated clock time up to which we've counted
	wall        float64 // computer time (ms) at the last frame, to notice pauses
}

// tickSecond: one second passes; registers that hold out-of-range values
// count on to their bit limit.
func (c *rtcClock) tickSecond() {
	c.s = (c.s + 1) & 63
	if c.s == 60 {
		c.s = 0
		c.m = (c.m + 1) & 63
		if c.m == 60 {
			c.m = 0
			c.h = (c.h + 1) & 31
			if c.h == 24 {
				c.h = 0
				c.d++
				if c.d > 511 {
					c.d = 0
					c.carry = true
				}
			}
		}
	}
}

func (c *rtcClock) addSeconds(n int64) {
	if c.halt || n <= 0 {
		return
	}
	// step until every value is in range, then jump
	for n > 0 && (c.s >= 60 || c.m >= 60 || c.h >= 24) {
		c.tickSecond()
		n--
	}
	if n <= 0 {
		return
	}
	t := int64(c.s) + 60*(int64(c.m)+60*(int64(c.h)+24*int64(c.d))) + n
	c.s = int(t % 60)
	t /= 60
	c.m = int(t % 60)
	t /= 60
	c.h = int(t % 24)
	t /= 24
	if t > 511 {
		c.carry = true
	}
	c.d = int(t % 512)
}

// sync brings the clock up to emulated time now.
func (c *rtcClock) sync(now int64) {
	dt := now - c.last
	c.last = now
	if c.halt || dt <= 0 {
		return
	}
	c.sub += dt
	if c.sub >= clockHz {
		n := c.sub / clockHz
		c.sub -= n * clockHz
		c.addSeconds(n)
	}
}

// frame, once per frame: if the computer's clock ran on without us (paused), catch up.
func (c *rtcClock) frame(emuMs, w float64) {
	if c.wall != 0 {
		lost := (w - c.wall) - emuMs
		if lost > 1500 {
			c.addSeconds(int64(math.Floor(lost / 1000)))
		}
	}
	c.wall = w
}

func (c *rtcClock) state(s *sio) {
	s.Ints(&c.s, &c.m, &c.h, &c.d)
	s.Bools(&c.halt, &c.carry)
	s.I64(&c.sub)
	s.I64(&c.last)
	s.F64(&c.wall)
}

// ===================================================================
// MBC3 (with optional clock) and MBC30 (8-bit ROM bank, 8 RAM banks)
// ===================================================================

type mbc3 struct {
	cartBase
	hasRtc, mbc30 bool
	sel           int
	clock         rtcClock
	latched       [5]int
	latchPrev     int
}

func newMBC3(rom []byte, gb *GB) *mbc3 {
	c := &mbc3{cartBase: newCartBase(rom, gb, "MBC3")}
	c.hasRtc = c.typ == 0x0f || c.typ == 0x10
	c.mbc30 = len(rom) > 0x200000 || c.ramSize > 0x8000
	c.latchPrev = 0xff
	c.romX = 0x4000
	return c
}

func (c *mbc3) rtcSync() { c.clock.sync(c.now()) }

func (c *mbc3) romWrite(a, v int) {
	switch {
	case a < 0x2000:
		c.ramOn = v&0x0f == 0x0a
	case a < 0x4000:
		m := 0x7f
		if c.mbc30 {
			m = 0xff
		}
		b := v & m
		if b == 0 {
			b = 1
		}
		c.romX = c.bankOffset(b)
	case a < 0x6000:
		c.sel = v & 0x0f
		if c.sel < 8 {
			m := 3
			if c.mbc30 {
				m = 7
			}
			c.ramOff = (c.sel & m) * 0x2000
		}
	default:
		if c.latchPrev == 0 && v == 1 && c.hasRtc {
			c.latch()
		}
		c.latchPrev = v
	}
}

func (c *mbc3) clockRegs() [5]int {
	k := &c.clock
	return [5]int{k.s, k.m, k.h, k.d & 0xff, ((k.d >> 8) & 1) | b2i(k.halt)<<6 | b2i(k.carry)<<7}
}

func (c *mbc3) latch() {
	c.rtcSync()
	c.latched = c.clockRegs()
}

func (c *mbc3) ramRead(a int) int {
	if !c.ramOn {
		return 0xff
	}
	if c.sel >= 8 {
		if c.hasRtc && c.sel <= 0x0c {
			return c.latched[c.sel-8]
		}
		return 0xff
	}
	return c.cartBase.ramRead(a)
}

func (c *mbc3) ramWrite(a, v int) {
	if !c.ramOn {
		return
	}
	if c.sel >= 8 {
		if c.hasRtc && c.sel <= 0x0c {
			c.writeRtc(c.sel-8, v)
		}
		return
	}
	c.cartBase.ramWrite(a, v)
}

var rtcMasks = [5]int{63, 63, 31, 255, 0xc1}

func (c *mbc3) writeRtc(reg, v int) {
	c.rtcSync()
	k := &c.clock
	switch reg {
	case 0:
		k.s = v & 63
		k.sub = 0 // writing the seconds restarts the second
	case 1:
		k.m = v & 63
	case 2:
		k.h = v & 31
	case 3:
		k.d = (k.d & 0x100) | v
	case 4:
		k.d = (k.d & 0xff) | ((v & 1) << 8)
		k.halt = v&0x40 != 0
		k.carry = v&0x80 != 0
	}
	c.latched[reg] = v & rtcMasks[reg]
}

func (c *mbc3) frame() {
	if c.hasRtc {
		c.rtcSync()
		c.clock.frame(emuFrameMs, c.dateNow())
	}
}

// extraSave: the clock in battery saves, in the common 48-byte layout
// (registers, latched registers, time stamp).
func (c *mbc3) extraSave() []byte {
	if !c.hasRtc {
		return nil
	}
	c.rtcSync()
	out := make([]byte, 48)
	cur := c.clockRegs()
	for i := 0; i < 5; i++ {
		binary.LittleEndian.PutUint32(out[i*4:], uint32(cur[i]))
		binary.LittleEndian.PutUint32(out[20+i*4:], uint32(c.latched[i]))
	}
	t := int64(math.Floor(c.dateNow() / 1000))
	binary.LittleEndian.PutUint32(out[40:], uint32(t))
	binary.LittleEndian.PutUint32(out[44:], uint32(t>>32))
	return out
}

func (c *mbc3) loadExtra(x []byte) {
	if !c.hasRtc {
		return
	}
	k := &c.clock
	le := binary.LittleEndian
	if len(x) >= 44 {
		var r [5]int
		for i := 0; i < 5; i++ {
			r[i] = int(le.Uint32(x[i*4:]) & 0xff)
		}
		for i := 0; i < 5; i++ {
			c.latched[i] = int(le.Uint32(x[20+i*4:]) & 0xff)
		}
		k.s, k.m, k.h = r[0]&63, r[1]&63, r[2]&31
		k.d = r[3] | ((r[4] & 1) << 8)
		k.halt, k.carry = r[4]&0x40 != 0, r[4]&0x80 != 0
		t := int64(le.Uint32(x[40:]))
		if len(x) >= 48 {
			t += int64(le.Uint32(x[44:])) * 4294967296
		}
		k.addSeconds(int64(math.Floor(c.dateNow()/1000)) - t)
	} else if len(x) >= 13 { // RetroPixels' earlier layout: when the clock read zero, and the halted value
		base := math.Float64frombits(le.Uint64(x[0:]))
		halted := x[12] != 0
		k.halt = false
		if halted {
			k.addSeconds(int64(le.Uint32(x[8:])))
		} else {
			k.addSeconds(int64(math.Floor((c.dateNow() - base) / 1000)))
		}
		k.halt = halted
	}
	k.last = c.now()
}

func (c *mbc3) state(s *sio) {
	c.cartBase.state(s)
	s.Int(&c.sel)
	c.clock.state(s)
	for i := range c.latched {
		s.Int(&c.latched[i])
	}
	s.Int(&c.latchPrev)
}

// ===================================================================
// MBC5 (9-bit ROM bank; rumble carts use RAM bank bit 3 for the motor)
// ===================================================================

type mbc5 struct {
	cartBase
	bank                              int
	motor                             bool
	motorSince, motorTime, frameStart int64
}

func newMBC5(rom []byte, gb *GB) *mbc5 {
	c := &mbc5{cartBase: newCartBase(rom, gb, "MBC5")}
	c.hasRumble = c.typ >= 0x1c && c.typ <= 0x1e
	c.bank = 1
	c.romX = c.bankOffset(1)
	return c
}

func (c *mbc5) romWrite(a, v int) {
	switch {
	case a < 0x2000:
		c.ramOn = v&0x0f == 0x0a
	case a < 0x3000:
		c.bank = (c.bank & 0x100) | v
		c.romX = c.bankOffset(c.bank)
	case a < 0x4000:
		c.bank = (c.bank & 0xff) | ((v & 1) << 8)
		c.romX = c.bankOffset(c.bank)
	case a < 0x6000:
		if c.hasRumble {
			c.ramOff = (v & 7) * 0x2000
			c.setMotor(v&8 != 0)
		} else {
			c.ramOff = (v & 0x0f) * 0x2000
		}
	}
}

func (c *mbc5) setMotor(on bool) {
	if on == c.motor {
		return
	}
	t := c.now()
	if c.motor {
		c.motorTime += t - max(c.motorSince, c.frameStart)
	}
	c.motor = on
	c.motorSince = t
}

// frame: how hard the motor ran during the last frame (games pulse it to set the strength).
func (c *mbc5) frame() {
	if !c.hasRumble {
		return
	}
	t := c.now()
	span := max(1, t-c.frameStart)
	if c.motor {
		c.motorTime += t - max(c.motorSince, c.frameStart)
	}
	c.rumbleLevel = math.Min(1, float64(c.motorTime)/float64(span))
	c.motorTime = 0
	c.frameStart = t
}

func (c *mbc5) state(s *sio) {
	c.cartBase.state(s)
	s.Int(&c.bank)
	s.Bool(&c.motor)
	s.I64(&c.motorSince)
	s.I64(&c.motorTime)
	s.I64(&c.frameStart)
}

// ===================================================================
// MBC6 (Net de Get): two 8 KB ROM/flash windows, two 4 KB RAM windows,
// and 1 MB of flash memory programmed with the usual unlock sequence
// ===================================================================

type mbc6 struct {
	cartBase
	flash               []byte
	bankA, bankB        int
	flashA, flashB      bool
	ramA, ramB          int
	flashOn, flashWrite bool
	fState, fCmd        int
	fID                 bool
}

func newMBC6(rom []byte, gb *GB) *mbc6 {
	c := &mbc6{cartBase: newCartBase(rom, gb, "MBC6")}
	c.ram, c.ramSize = make([]byte, 0x8000), 0x8000
	c.flash = make([]byte, 0x100000)
	for i := range c.flash {
		c.flash[i] = 0xff
	}
	c.battery = true
	return c
}

func (c *mbc6) romRead(a int) int {
	if a < 0x4000 {
		return c.romAt(a)
	}
	bank, fl := c.bankA, c.flashA
	if a >= 0x6000 {
		bank, fl = c.bankB, c.flashB
	}
	off := a & 0x1fff
	if fl {
		if c.fID { // Macronix, MX29F008
			switch off {
			case 0:
				return 0xc2
			case 1:
				return 0x81
			}
			return 0xff
		}
		return int(c.flash[((bank&0x7f)*0x2000+off)&0xfffff])
	}
	return int(c.rom[(bank*0x2000+off)%len(c.rom)])
}

func (c *mbc6) romWrite(a, v int) {
	switch {
	case a < 0x400:
		c.ramOn = v&0x0f == 0x0a
	case a < 0x800:
		c.ramA = v & 7
	case a < 0xc00:
		c.ramB = v & 7
	case a < 0x1000:
		c.flashOn = v&1 != 0
	case a == 0x1000:
		c.flashWrite = v&1 != 0
	case a >= 0x2000 && a < 0x2800:
		c.bankA = v & 0x7f
	case a >= 0x2800 && a < 0x3000:
		c.flashA = v == 8
	case a >= 0x3000 && a < 0x3800:
		c.bankB = v & 0x7f
	case a >= 0x3800 && a < 0x4000:
		c.flashB = v == 8
	case a >= 0x4000: // writes into a flash window go to the flash chip
		bank, fl := c.bankA, c.flashA
		if a >= 0x6000 {
			bank, fl = c.bankB, c.flashB
		}
		if fl && c.flashOn {
			c.flashCmd((bank&0x7f)*0x2000+(a&0x1fff), v)
		}
	}
}

func (c *mbc6) flashCmd(fa, v int) {
	fa &= 0xfffff
	if c.fCmd == 0xa0 {
		if c.flashWrite {
			c.flash[fa] &= byte(v)
		}
		c.fCmd = 0
		return
	}
	if v == 0xf0 {
		c.fState, c.fCmd, c.fID = 0, 0, false
		return
	}
	lo := fa & 0x7fff
	if c.fState == 0 && lo == 0x5555 && v == 0xaa {
		c.fState = 1
		return
	}
	if c.fState == 1 && lo == 0x2aaa && v == 0x55 {
		c.fState = 2
		return
	}
	if c.fState == 2 {
		c.fState = 0
		if c.fCmd == 0x80 && v == 0x30 {
			if c.flashWrite {
				s := fa &^ 0x1ffff
				for i := s; i < s+0x20000; i++ {
					c.flash[i] = 0xff
				}
			}
			c.fCmd = 0
			return
		}
		if c.fCmd == 0x80 && v == 0x10 {
			if c.flashWrite {
				for i := range c.flash {
					c.flash[i] = 0xff
				}
			}
			c.fCmd = 0
			return
		}
		if v == 0x90 {
			c.fID = true
		} else if v == 0x80 || v == 0xa0 {
			c.fCmd = v
		}
		return
	}
	c.fState = 0
}

func (c *mbc6) ramIndex(a int) int {
	b := c.ramA
	if a >= 0xb000 {
		b = c.ramB
	}
	return b*0x1000 + (a & 0xfff)
}

func (c *mbc6) ramRead(a int) int {
	if !c.ramOn {
		return 0xff
	}
	return int(c.ram[c.ramIndex(a)])
}

func (c *mbc6) ramWrite(a, v int) {
	if c.ramOn {
		c.ram[c.ramIndex(a)] = byte(v)
	}
}

func (c *mbc6) extraSave() []byte  { return c.flash }
func (c *mbc6) loadExtra(x []byte) { copy(c.flash, x) }

func (c *mbc6) state(s *sio) {
	c.cartBase.state(s)
	s.Bytes(c.flash)
	s.Ints(&c.bankA, &c.bankB)
	s.Bools(&c.flashA, &c.flashB)
	s.Ints(&c.ramA, &c.ramB)
	s.Bools(&c.flashOn, &c.flashWrite)
	s.Ints(&c.fState, &c.fCmd)
	s.Bool(&c.fID)
}

// ===================================================================
// MBC7 (Kirby Tilt 'n' Tumble, Command Master): two-axis tilt sensor and
// a 93LC56 serial EEPROM (128 words of 16 bits)
// ===================================================================

type mbc7 struct {
	cartBase
	ramg1, ramg2                 bool
	accX, accY                   int
	latched                      bool
	cs, clk, di, do              int
	eState, eBits, eCount, eAddr int
	eOut, eOutBits               int
	wen                          bool
}

func newMBC7(rom []byte, gb *GB) *mbc7 {
	c := &mbc7{cartBase: newCartBase(rom, gb, "MBC7")}
	c.ram, c.ramSize = make([]byte, 256), 256 // the EEPROM is the battery save
	for i := range c.ram {
		c.ram[i] = 0xff
	}
	c.battery = true
	c.accX, c.accY = 0x8000, 0x8000
	c.do = 1
	c.romX = c.bankOffset(1)
	return c
}

func (c *mbc7) romWrite(a, v int) {
	switch {
	case a < 0x2000:
		c.ramg1 = v&0x0f == 0x0a
		if !c.ramg1 {
			c.ramg2 = false
		}
	case a < 0x4000:
		c.romX = c.bankOffset(v & 0x7f)
	case a < 0x6000:
		c.ramg2 = c.ramg1 && v == 0x40
	}
}

func (c *mbc7) ramRead(a int) int {
	if !c.ramg1 || !c.ramg2 || a >= 0xb000 {
		return 0xff
	}
	switch (a >> 4) & 0x0f {
	case 2:
		return c.accX & 0xff
	case 3:
		return c.accX >> 8
	case 4:
		return c.accY & 0xff
	case 5:
		return c.accY >> 8
	case 6:
		return 0x00
	case 8:
		return (c.cs << 7) | (c.clk << 6) | (c.di << 1) | c.do
	}
	return 0xff
}

func (c *mbc7) ramWrite(a, v int) {
	if !c.ramg1 || !c.ramg2 || a >= 0xb000 {
		return
	}
	switch (a >> 4) & 0x0f {
	case 0:
		if v == 0x55 {
			c.accX, c.accY, c.latched = 0x8000, 0x8000, false
		}
	case 1:
		if v == 0xaa && !c.latched {
			// centre 0x81D0, about 0x70 per g; tilting right or toward you lowers the value
			var tx, ty float64
			if c.gb != nil {
				tx, ty = c.gb.tiltX, c.gb.tiltY
			}
			c.accX = (0x81d0 - int(jsRound(tx*0x70))) & 0xffff
			c.accY = (0x81d0 - int(jsRound(ty*0x70))) & 0xffff
			c.latched = true
		}
	case 8:
		c.eeprom(v)
	}
}

// eeprom: the Microwire serial protocol: start bit, 2-bit opcode, 8-bit
// address (word address in the low 7 bits).
func (c *mbc7) eeprom(v int) {
	cs, clk, di := (v>>7)&1, (v>>6)&1, (v>>1)&1
	if cs == 0 {
		c.eState, c.do, c.cs, c.clk, c.di = 0, 1, 0, clk, di
		return
	}
	if c.cs == 0 { // chip selected: wait for a start bit
		c.eState, c.eCount, c.eBits = 0, 0, 0
	}
	rising := clk != 0 && c.clk == 0
	c.cs, c.clk, c.di = cs, clk, di
	if !rising {
		return
	}
	switch c.eState {
	case 0:
		if di != 0 {
			c.eState, c.eCount, c.eBits = 1, 0, 0
		}
	case 1: // opcode + address
		c.eBits = (c.eBits << 1) | di
		c.eCount++
		if c.eCount == 10 {
			op, addr := c.eBits>>8, c.eBits&0x7f
			c.eAddr, c.eCount = addr, 0
			switch op {
			case 2: // read: a dummy 0 first
				c.eState, c.eOut, c.eOutBits, c.do = 2, c.word(addr), 16, 0
			case 1: // write
				c.eState, c.eBits = 3, 0
			case 3: // erase
				if c.wen {
					c.setWord(addr, 0xffff)
				}
				c.eState, c.do = 4, 1
			default:
				switch (c.eBits >> 6) & 3 {
				case 0:
					c.wen, c.eState = false, 4
				case 3:
					c.wen, c.eState = true, 4
				case 2:
					if c.wen {
						for i := range c.ram {
							c.ram[i] = 0xff
						}
					}
					c.eState = 4
				default: // write all
					c.eState, c.eBits = 5, 0
				}
			}
		}
	case 2: // shifting data out, words follow on
		if c.eOutBits == 0 {
			c.eAddr = (c.eAddr + 1) & 0x7f
			c.eOut, c.eOutBits = c.word(c.eAddr), 16
		}
		c.do = (c.eOut >> 15) & 1
		c.eOut = (c.eOut << 1) & 0xffff
		c.eOutBits--
	case 3, 5:
		c.eBits = (c.eBits << 1) | di
		c.eCount++
		if c.eCount == 16 {
			if c.wen {
				if c.eState == 3 {
					c.setWord(c.eAddr, c.eBits)
				} else {
					for i := 0; i < 128; i++ {
						c.setWord(i, c.eBits)
					}
				}
			}
			c.eState, c.do = 4, 1 // done at once (always ready)
		}
	}
}

func (c *mbc7) word(i int) int { return int(c.ram[i*2])<<8 | int(c.ram[i*2+1]) }
func (c *mbc7) setWord(i, w int) {
	c.ram[i*2] = byte(w >> 8)
	c.ram[i*2+1] = byte(w)
}

func (c *mbc7) state(s *sio) {
	c.cartBase.state(s)
	s.Bools(&c.ramg1, &c.ramg2)
	s.Ints(&c.accX, &c.accY)
	s.Bool(&c.latched)
	s.Ints(&c.cs, &c.clk, &c.di, &c.do, &c.eState, &c.eBits, &c.eCount, &c.eAddr, &c.eOut, &c.eOutBits)
	s.Bool(&c.wen)
}

// ===================================================================
// HuC1 (Hudson): MBC1-like banking plus an infrared port, which here never sees light
// ===================================================================

type huc1 struct {
	cartBase
	ir bool
}

func newHuC1(rom []byte, gb *GB) *huc1 {
	c := &huc1{cartBase: newCartBase(rom, gb, "HuC1")}
	c.ramOn = true
	c.romX = c.bankOffset(1)
	return c
}

func (c *huc1) romWrite(a, v int) {
	switch {
	case a < 0x2000:
		c.ir = v&0x0f == 0x0e
		c.ramOn = !c.ir
	case a < 0x4000:
		b := v & 0x3f
		if b == 0 {
			b = 1
		}
		c.romX = c.bankOffset(b)
	case a < 0x6000:
		c.ramOff = (v & 3) * 0x2000
	}
}

func (c *huc1) ramRead(a int) int {
	if c.ir {
		return 0xc0
	}
	return c.cartBase.ramRead(a)
}

func (c *huc1) ramWrite(a, v int) {
	if !c.ir {
		c.cartBase.ramWrite(a, v)
	}
}

func (c *huc1) state(s *sio) {
	c.cartBase.state(s)
	s.Bool(&c.ir)
}

// ===================================================================
// HuC3 (Hudson): banking, infrared (dark), and a clock reached through a
// nibble-wide command port. Its memory holds minutes-of-day and days.
// ===================================================================

type huc3 struct {
	cartBase
	mode                  int
	mem                   [256]byte
	addr, result, lastCmd int
	minutes, days         int
	sub                   float64
	last                  int64
	wall                  float64
}

func newHuC3(rom []byte, gb *GB) *huc3 {
	c := &huc3{cartBase: newCartBase(rom, gb, "HuC3")}
	c.romX = c.bankOffset(1)
	return c
}

func (c *huc3) clockSync() {
	now := c.now()
	dt := now - c.last
	c.last = now
	if dt > 0 {
		c.sub += float64(dt)
		const per = clockHz * 60
		if c.sub >= per {
			n := math.Floor(c.sub / per)
			c.sub -= n * per
			c.addMinutes(int64(n))
		}
	}
}

func (c *huc3) addMinutes(n int64) {
	t := int64(c.minutes) + n
	c.minutes = int(t % 1440)
	c.days = int((int64(c.days) + t/1440) & 0xfff)
}

func (c *huc3) romWrite(a, v int) {
	switch {
	case a < 0x2000:
		c.mode = v & 0x0f
		c.ramOn = c.mode == 0x0a
	case a < 0x4000:
		c.romX = c.bankOffset(v & 0x7f)
	case a < 0x6000:
		c.ramOff = (v & 0x0f) * 0x2000
	}
}

func (c *huc3) ramRead(a int) int {
	switch c.mode {
	case 0x0a, 0x00:
		if len(c.ram) == 0 {
			return 0xff
		}
		return int(c.ram[(c.ramOff+(a&0x1fff))%len(c.ram)])
	case 0x0c:
		return 0x80 | (c.lastCmd << 4) | c.result
	case 0x0d:
		return 0x01 // the clock is always ready
	case 0x0e:
		return 0xc0 // no infrared light
	}
	return 0xff
}

func (c *huc3) ramWrite(a, v int) {
	if c.mode == 0x0a {
		c.cartBase.ramWrite(a, v)
		return
	}
	if c.mode != 0x0b {
		return
	}
	cmd, arg := (v>>4)&7, v&0x0f
	c.lastCmd = cmd
	switch cmd {
	case 1:
		c.result = int(c.mem[c.addr] & 0x0f)
		c.addr = (c.addr + 1) & 0xff
	case 2:
		c.result = int(c.mem[c.addr] & 0x0f)
	case 3:
		c.mem[c.addr] = byte(arg)
		c.addr = (c.addr + 1) & 0xff
	case 4:
		c.addr = (c.addr & 0xf0) | arg
	case 5:
		c.addr = (c.addr & 0x0f) | (arg << 4)
	case 6:
		switch arg {
		case 0: // copy the clock into memory 0-5
			c.clockSync()
			for i := 0; i < 3; i++ {
				c.mem[i] = byte((c.minutes >> (i * 4)) & 0x0f)
				c.mem[3+i] = byte((c.days >> (i * 4)) & 0x0f)
			}
		case 1: // set the clock from memory 0-5
			c.clockSync()
			m, d := 0, 0
			for i := 0; i < 3; i++ {
				m |= int(c.mem[i]&0x0f) << (i * 4)
				d |= int(c.mem[3+i]&0x0f) << (i * 4)
			}
			c.minutes, c.days, c.sub = m%1440, d, 0
		case 2:
			c.result = 1
		}
	}
}

func (c *huc3) frame() {
	c.clockSync()
	w := c.dateNow()
	if c.wall != 0 {
		lost := (w - c.wall) - emuFrameMs
		if lost > 1500 {
			c.sub += lost / 1000 * clockHz
			c.clockSync()
		}
	}
	c.wall = w
}

// extraSave: RetroPixels' clock block (minutes, days, time stamp, clock memory).
func (c *huc3) extraSave() []byte {
	c.clockSync()
	out := make([]byte, 16+256)
	binary.LittleEndian.PutUint32(out[0:], uint32(c.minutes))
	binary.LittleEndian.PutUint32(out[4:], uint32(c.days))
	binary.LittleEndian.PutUint64(out[8:], math.Float64bits(c.dateNow()))
	copy(out[16:], c.mem[:])
	return out
}

func (c *huc3) loadExtra(x []byte) {
	if len(x) < 16 {
		return
	}
	le := binary.LittleEndian
	c.minutes = int(le.Uint32(x[0:]) % 1440)
	c.days = int(le.Uint32(x[4:]) & 0xfff)
	if len(x) >= 272 {
		copy(c.mem[:], x[16:272])
	}
	away := c.dateNow() - math.Float64frombits(le.Uint64(x[8:]))
	if away > 0 {
		c.addMinutes(int64(math.Floor(away / 60000)))
	}
	c.last = c.now()
}

func (c *huc3) state(s *sio) {
	c.cartBase.state(s)
	s.Int(&c.mode)
	s.Bytes(c.mem[:])
	s.Ints(&c.addr, &c.result, &c.lastCmd, &c.minutes, &c.days)
	s.F64(&c.sub)
	s.I64(&c.last)
	s.F64(&c.wall)
}

// ===================================================================
// MMM01 (multi-game carts): starts with the menu (the last 32 KB of ROM)
// mapped in; once the menu picks a game it locks the outer bank bits.
// ===================================================================

type mmm01 struct {
	cartBase
	mapped                            bool
	romLow, romMid, romHigh, fixedLow int
	ramLow, ramMid, mask, mode        int
	modeLock                          bool
}

func newMMM01(rom []byte, gb *GB) *mmm01 {
	c := &mmm01{cartBase: newCartBase(rom, gb, "MMM01")}
	c.update()
	return c
}

func (c *mmm01) update() {
	size := c.romBanks * 0x4000
	if !c.mapped {
		c.rom0 = max(0, size-0x8000)
		c.romX = max(0x4000, size-0x4000)
		c.ramOff = 0
		return
	}
	// outer bank bits chosen by the menu, low bits by the game (the menu can freeze some of them too)
	outer, fixed := (c.romHigh<<7)|(c.romMid<<5), (c.mask<<1)&0x1e
	keep := c.fixedLow & fixed
	low := c.romLow
	if low == 0 {
		low = 1
	}
	c.romX = c.bankOffset(outer | keep | (low &^ fixed & 0x1f))
	c.rom0 = c.bankOffset(outer | keep)
	rl := 0
	if c.mode != 0 {
		rl = c.ramLow
	}
	c.ramOff = ((c.ramMid << 2) | rl) * 0x2000
}

func (c *mmm01) romWrite(a, v int) {
	switch {
	case a < 0x2000:
		c.ramOn = v&0x0f == 0x0a
		if !c.mapped && v&0x40 != 0 {
			c.mapped = true
			c.fixedLow = c.romLow
		}
	case a < 0x4000:
		c.romLow = v & 0x1f
		if !c.mapped {
			c.romMid = (v >> 5) & 3
		}
	case a < 0x6000:
		c.ramLow = v & 3
		if !c.mapped {
			c.ramMid = (v >> 2) & 3
			c.romHigh = (v >> 4) & 3
			c.modeLock = v&0x40 != 0
		}
	default:
		if !c.modeLock {
			c.mode = v & 1
		}
		if !c.mapped {
			c.mask = (v >> 2) & 0x0f
		}
	}
	c.update()
}

func (c *mmm01) state(s *sio) {
	c.cartBase.state(s)
	s.Bool(&c.mapped)
	s.Ints(&c.romLow, &c.romMid, &c.romHigh, &c.fixedLow, &c.ramLow, &c.ramMid, &c.mask, &c.mode)
	s.Bool(&c.modeLock)
}

// ===================================================================
// Bandai TAMA5 (Tamagotchi 3): registers reached a nibble at a time through
// A000 (data) and A001 (register number); 32 bytes of memory and a clock.
// ===================================================================

type tama5 struct {
	cartBase
	reg                     int
	r                       [16]byte
	out                     int
	clock                   rtcClock
	month, year, dayOfMonth int
}

func newTAMA5(rom []byte, gb *GB) *tama5 {
	c := &tama5{cartBase: newCartBase(rom, gb, "TAMA5")}
	c.battery = true
	c.ram, c.ramSize = make([]byte, 32), 32
	c.month, c.dayOfMonth = 1, 1
	c.romX = c.bankOffset(1)
	return c
}

func (c *tama5) romWrite(a, v int) {} // everything goes through A000/A001

func bcd(v int) int { return (v/10)*16 + v%10 }

func (c *tama5) ramRead(a int) int {
	if a&1 != 0 {
		return 0xff
	}
	switch c.reg {
	case 0x0a:
		return 0xf1 // ready
	case 0x0c:
		return 0xf0 | (c.out & 0x0f)
	case 0x0d:
		return 0xf0 | (c.out >> 4)
	}
	return 0xff
}

func (c *tama5) ramWrite(a, v int) {
	if a&1 != 0 {
		c.reg = v & 0x0f
		return
	}
	r := &c.r
	r[c.reg] = byte(v & 0x0f)
	switch c.reg {
	case 0, 1:
		c.romX = c.bankOffset(int(r[1]&1)<<4 | int(r[0]))
	case 7: // run the operation
		op, addr, data := int(r[6]>>1), int(r[6]&1)<<4|int(r[7]), int(r[5])<<4|int(r[4])
		switch op {
		case 0:
			c.ram[addr] = byte(data)
		case 1:
			c.out = int(c.ram[addr])
		case 2:
			c.out = c.readClock(addr)
		case 3:
			c.writeClock(addr, data)
		}
	}
}

// readClock: the clock chip's registers, in BCD: 0 seconds, 1 minutes, 2
// hours, 3 day, 4 month, 5 year.
func (c *tama5) readClock(i int) int {
	c.clock.sync(c.now())
	k := &c.clock
	switch i {
	case 0:
		return bcd(k.s % 60)
	case 1:
		return bcd(k.m % 60)
	case 2:
		return bcd(k.h % 24)
	case 3:
		return bcd(c.dayOfMonth)
	case 4:
		return bcd(c.month)
	case 5:
		return bcd(c.year)
	}
	return 0
}

func (c *tama5) writeClock(i, v int) {
	c.clock.sync(c.now())
	n, k := (v>>4)*10+(v&0x0f), &c.clock
	or1 := func(x int) int {
		if x == 0 {
			return 1
		}
		return x
	}
	switch i {
	case 0:
		k.s, k.sub = n%60, 0
	case 1:
		k.m = n % 60
	case 2:
		k.h = n % 24
	case 3:
		c.dayOfMonth = or1(n)
	case 4:
		c.month = or1(n)
	case 5:
		c.year = n
	}
}

func (c *tama5) frame() {
	before := c.clock.d
	c.clock.sync(c.now())
	c.clock.frame(emuFrameMs, c.dateNow())
	// a simple calendar: days roll into months of 30 days
	for k := before; k < c.clock.d; k++ {
		c.dayOfMonth++
		if c.dayOfMonth > 30 {
			c.dayOfMonth = 1
			c.month++
			if c.month > 12 {
				c.month = 1
				c.year = (c.year + 1) % 100
			}
		}
	}
	c.clock.d &= 0xff
}

func (c *tama5) extraSave() []byte {
	c.clock.sync(c.now())
	k := &c.clock
	out := make([]byte, 16)
	for i, v := range [6]int{k.s, k.m, k.h, c.dayOfMonth, c.month, c.year} {
		out[i] = byte(v)
	}
	binary.LittleEndian.PutUint64(out[8:], math.Float64bits(c.dateNow()))
	return out
}

func (c *tama5) loadExtra(x []byte) {
	if len(x) < 16 {
		return
	}
	k := &c.clock
	k.s, k.m, k.h = int(x[0]), int(x[1]), int(x[2])
	c.dayOfMonth, c.month, c.year = int(x[3]), int(x[4]), int(x[5])
	k.d = 0
	away := int64(math.Floor((c.dateNow() - math.Float64frombits(binary.LittleEndian.Uint64(x[8:]))) / 1000))
	if away > 0 {
		k.addSeconds(away)
	}
	k.last = c.now()
}

func (c *tama5) state(s *sio) {
	c.cartBase.state(s)
	s.Int(&c.reg)
	s.Bytes(c.r[:])
	s.Int(&c.out)
	c.clock.state(s)
	s.Ints(&c.month, &c.year, &c.dayOfMonth)
}

// ===================================================================
// Pocket Camera: MBC with 128 KB RAM and the camera's registers in RAM bank
// $10. There's no camera, so a capture "photographs" a built-in test card,
// shaded through the game's exposure and dithering settings.
// ===================================================================

var testCard = func() []byte {
	p := make([]byte, 128*112)
	for y := 0; y < 112; y++ {
		for x := 0; x < 128; x++ {
			v := int(40 + float64(x*180)/127) // left-to-right gradient
			dx, dy := float64(x-64), float64(y-56)
			if r := math.Sqrt(dx*dx + dy*dy); r < 40 { // target rings
				if int(math.Floor(r/6))&1 != 0 {
					v = 230
				} else {
					v = 30
				}
			}
			if y >= 100 { // grey-scale bars
				v = [4]int{20, 90, 160, 235}[x/32]
			}
			if x < 2 || y < 2 || x > 125 || y > 109 { // frame
				v = 10
			}
			p[y*128+x] = byte(v)
		}
	}
	return p
}()

type camera struct {
	cartBase
	regs      [0x36]byte
	camSel    bool
	busyUntil int64
}

func newCamera(rom []byte, gb *GB) *camera {
	c := &camera{cartBase: newCartBase(rom, gb, "Camera")}
	c.ram, c.ramSize = make([]byte, 0x20000), 0x20000
	c.romX = c.bankOffset(1)
	return c
}

func (c *camera) romWrite(a, v int) {
	switch {
	case a < 0x2000:
		c.ramOn = v&0x0f == 0x0a
	case a < 0x4000:
		c.romX = c.bankOffset(v & 0x3f)
	case a < 0x6000:
		c.camSel = v&0x10 != 0
		if !c.camSel {
			c.ramOff = (v & 0x0f) * 0x2000
		}
	}
}

func (c *camera) frame() { c.busy() }

func (c *camera) busy() bool {
	if c.busyUntil != 0 && c.now() >= c.busyUntil {
		c.busyUntil = 0
		c.regs[0] &^= 1
		c.capture()
	}
	return c.busyUntil != 0
}

func (c *camera) ramRead(a int) int {
	if c.camSel {
		if a&0x7f == 0 {
			return b2i(c.busy()) | int(c.regs[0]&6)
		}
		return 0x00
	}
	return int(c.ram[(c.ramOff+(a&0x1fff))%len(c.ram)]) // readable even when writes are off
}

func (c *camera) ramWrite(a, v int) {
	if c.camSel {
		r := a & 0x7f
		if r >= 0x36 {
			return
		}
		if r == 0 {
			c.busy()
			c.regs[0] = byte(v & 7)
			if v&1 != 0 && c.busyUntil == 0 {
				exposure := int64(c.regs[2])<<8 | int64(c.regs[3])
				c.busyUntil = c.now() + 32446 + exposure*16
			}
		} else {
			c.regs[r] = byte(v)
		}
		return
	}
	if c.ramOn {
		c.cartBase.ramWrite(a, v)
	}
}

// capture turns the test card into 16x14 tiles in RAM bank 0 at A100.
func (c *camera) capture() {
	R := &c.regs
	exposure := int(R[2])<<8 | int(R[3])
	gain := math.Min(2.5, math.Max(0.15, float64(exposure)/0x1000+0.15))
	for ty := 0; ty < 14; ty++ {
		for tx := 0; tx < 16; tx++ {
			for row := 0; row < 8; row++ {
				lo, hi := 0, 0
				y := ty*8 + row
				for col := 0; col < 8; col++ {
					x := tx*8 + col
					v := math.Min(255, float64(testCard[y*128+x])*gain)
					m := 6 + ((y&3)*4+(x&3))*3
					cv := 0
					switch {
					case v < float64(R[m]):
						cv = 3
					case v < float64(R[m+1]):
						cv = 2
					case v < float64(R[m+2]):
						cv = 1
					}
					lo |= (cv & 1) << (7 - col)
					hi |= (cv >> 1) << (7 - col)
				}
				o := 0x100 + (ty*16+tx)*16 + row*2
				c.ram[o], c.ram[o+1] = byte(lo), byte(hi)
			}
		}
	}
}

func (c *camera) state(s *sio) {
	c.cartBase.state(s)
	s.Bytes(c.regs[:])
	s.Bool(&c.camSel)
	s.I64(&c.busyUntil)
}
