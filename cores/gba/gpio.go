package gba

import "math"

// The cartridge GPIO port and the sensors some carts carry (clock, sunlight,
// gyro, tilt, rumble): a port of web/emu/gba_gpio.js.
//
// A few cartridges wire extra chips to four general-purpose pins that show up
// in the ROM area at 0x080000C4 (data), 0x080000C6 (pin direction, 1 = the
// game drives the pin) and 0x080000C8 (bit 0: the pins can be read back).
// What sits on those pins depends on the game, so it's picked by game code:
//
//	real-time clock     pin 0 clock, pin 1 data, pin 2 select (high)
//	sunlight sensor     pin 0 clock, pin 1 reset, pin 2 select (low), pin 3 "reached" flag
//	gyro sensor         pin 0 start a reading, pin 1 clock, pin 2 data, pin 3 rumble motor
//	rumble motor        pin 3
//
// The tilt sensor of Yoshi Topsy-Turvy / Koro Koro Puzzle is not on the pins:
// it answers in the save-memory area at 0x0E008000.

// gpioGames: game codes (header bytes 0xAC-0xAF) of the carts with extra hardware.
var gpioGames = map[string]string{
	"U3IJ": "solar", "U3IE": "solar", "U3IP": "solar", // Boktai: The Sun Is in Your Hand
	"U32J": "solar", "U32E": "solar", "U32P": "solar", // Boktai 2: Solar Boy Django
	"U33J": "solar",                                // Shin Bokura no Taiyou (Boktai 3)
	"RZWJ": "gyro", "RZWE": "gyro", "RZWP": "gyro", // WarioWare: Twisted! / Mawaru Made in Wario
	"V49J": "rumble", "V49E": "rumble", "V49P": "rumble", // Drill Dozer / Screw Breaker
	"KYGJ": "tilt", "KYGE": "tilt", "KYGP": "tilt", // Yoshi Topsy-Turvy / Yoshi's Universal Gravitation
	"KHPJ": "tilt", // Koro Koro Puzzle Happy Panechu!
}

// jsRound is JavaScript's Math.round (halves go up).
func jsRound(x float64) float64 {
	f := math.Floor(x)
	if x-f >= 0.5 {
		return f + 1
	}
	return f
}

func clamp1(v float64) float64 { return math.Max(-1, math.Min(1, v)) }

// ---- sunlight sensor (Boktai) ----
// The game resets a counter, then clocks it until the sensor's comparator
// says "reached". Bright light trips it early, darkness late.
type solar struct {
	count, base, clk int
	level            float64
}

func (s *solar) threshold() int { return int(jsRound(0xe8 - s.level*(0xe8-0x50))) }

func (s *solar) pins(p int) { // p = pin levels the game drives
	if p&4 != 0 { // pin 2 high: the clock chip is selected instead
		s.clk = p & 1
		return
	}
	if p&2 != 0 {
		s.count = 0
	} else if p&1 != 0 && s.clk == 0 {
		s.count = (s.count + 1) & 0xff
	}
	s.clk = p & 1
}

func (s *solar) input() int {
	if s.count >= s.threshold() {
		return 8
	}
	return 0
}

// ---- gyro sensor (WarioWare: Twisted!) ----
// Raising pin 0 samples the turning speed (12 bits, about 0x6C0 at rest);
// each falling edge of the clock then puts the next of its 16 bits on pin 2,
// top bit first.
type gyro struct {
	rate         float64
	word, bit, p int
}

func (g *gyro) pins(p int) {
	if p&1 != 0 && g.p&1 == 0 {
		g.word = (0x6c0 - int(jsRound(g.rate*0x300))) & 0xfff
		g.bit = 16
	} else if p&2 == 0 && g.p&2 != 0 && g.bit >= 0 {
		g.bit--
	}
	g.p = p
}

func (g *gyro) input() int {
	if g.bit >= 0 && g.bit < 16 {
		return (g.word >> g.bit & 1) << 2
	}
	return 0
}

// ---- tilt sensor (Yoshi Topsy-Turvy, Koro Koro Puzzle) ----
// Write 0x55 to 0E008000 then 0xAA to 0E008100 to take a reading; the
// 12-bit X / Y values then sit at 0E008200/0300 and 0E008400/0500
// (bit 7 of 0300 says the reading is ready).
type tilt struct {
	armed       bool
	x, y, ready int
}

func (t *tilt) read8(a uint32) uint32 {
	switch a & 0xff00 {
	case 0x8200:
		return uint32(t.x & 0xff)
	case 0x8300:
		return uint32(t.x>>8&15 | t.ready)
	case 0x8400:
		return uint32(t.y & 0xff)
	case 0x8500:
		return uint32(t.y >> 8 & 15)
	}
	return 0
}

func (t *tilt) write8(a, v uint32, mx, my float64) {
	if a&0xff00 == 0x8000 && v == 0x55 {
		t.armed = true
	} else if a&0xff00 == 0x8100 && v == 0xaa && t.armed {
		// tilting right / toward the player lowers the value, like the GB tilt carts
		t.x = 0x392 - int(jsRound(mx*0xa0))
		t.y = 0x3a0 - int(jsRound(my*0xa0))
		t.ready = 0x80
		t.armed = false
	}
}

// ---- the port itself ----
type gpioPort struct {
	g              *GBA
	kind           string
	rtc            *rtcChip
	dir, ctrl, out int
	solar          *solar
	gyro           *gyro
	tilt           *tilt
	hasRumble      bool
	onPins         bool
	// controls
	stickX, stickY   float64
	extra, prevExtra int
	motionX, motionY float64
	// rumble: share of the frame the motor was on
	motor, motorSince int64
	motorOn           int
	rumbleLevel       float64
}

// newGPIO builds the port for this cartridge, or nil when it has nothing on it.
func newGPIO(g *GBA, rom []byte, wantsRTC bool) *gpioPort {
	code := string(rom[0xac:0xb0])
	kind := gpioGames[code]
	var rtc *rtcChip
	if wantsRTC || kind == "solar" {
		rtc = newRTC(g.now)
	}
	if kind == "" && rtc == nil {
		return nil
	}
	p := &gpioPort{g: g, kind: kind, rtc: rtc}
	switch kind {
	case "solar":
		p.solar = &solar{base: 5, level: 0.5}
	case "gyro":
		p.gyro = &gyro{bit: 16}
	case "tilt":
		p.tilt = &tilt{x: 0x392, y: 0x3a0}
	}
	p.hasRumble = kind == "gyro" || kind == "rumble"
	p.onPins = rtc != nil || p.solar != nil || p.gyro != nil || p.hasRumble
	return p
}

// pinLevels: the game's own levels on its output pins, the chips' on the rest.
func (p *gpioPort) pinLevels() int {
	ins := 0
	if p.rtc != nil {
		ins |= p.rtc.pins & 2
	}
	if p.solar != nil {
		ins |= p.solar.input()
	}
	if p.gyro != nil {
		ins |= p.gyro.input()
	}
	return (p.out&p.dir | ins&^p.dir) & 15
}

func (p *gpioPort) read16(o uint32) uint32 {
	switch o {
	case 0xc4:
		return uint32(p.pinLevels())
	case 0xc6:
		return uint32(p.dir)
	}
	return uint32(p.ctrl)
}

func (p *gpioPort) write16(o, v uint32) {
	switch o {
	case 0xc4:
		p.out = int(v & 15)
		p.drive()
	case 0xc6:
		p.dir = int(v & 15)
		p.drive()
	case 0xc8:
		p.ctrl = int(v & 1)
	}
}

func (p *gpioPort) drive() {
	pl := p.out & p.dir
	if p.rtc != nil {
		p.rtc.dir = p.dir
		p.rtc.write(p.out)
	}
	if p.solar != nil {
		p.solar.pins(pl)
	}
	if p.gyro != nil {
		p.gyro.pins(pl)
	}
	if p.hasRumble {
		on, now := pl>>3&1, p.g.cpu.Cycles
		if on != p.motorOn {
			if p.motorOn != 0 {
				p.motor += now - p.motorSince
			}
			p.motorOn = on
			p.motorSince = now
		}
	}
}

// tilt sensor in the save area: busRead8 returns false when it doesn't answer
func (p *gpioPort) busRead8(a uint32) (uint32, bool) {
	if p.tilt != nil && a&0xffff >= 0x8000 && a&0xffff < 0x8600 {
		return p.tilt.read8(a), true
	}
	return 0, false
}

func (p *gpioPort) busWrite8(a, v uint32) bool {
	if p.tilt != nil && a&0xffff >= 0x8000 && a&0xffff < 0x8600 {
		p.tilt.write8(a, v, p.motionX, p.motionY)
		return true
	}
	return false
}

// ---- controls ----

func b2f(b bool) float64 {
	if b {
		return 1
	}
	return 0
}

func (p *gpioPort) frameStart() {
	k := p.extra
	if p.solar != nil { // More Sun / Less Sun step the light by a tenth; pushing the right stick up adds sun on top
		pressed := k &^ p.prevExtra
		if pressed&1 != 0 {
			p.solar.base = min(10, p.solar.base+1)
		}
		if pressed&2 != 0 {
			p.solar.base = max(0, p.solar.base-1)
		}
		up := 0.0
		if -p.stickY > 0.15 {
			up = -p.stickY
		}
		p.solar.level = math.Max(float64(p.solar.base)/10, up)
	}
	if p.gyro != nil {
		p.gyro.rate = clamp1(p.stickX + b2f(k&2 != 0) - b2f(k&1 != 0))
	}
	if p.tilt != nil { // Tilt Up / Down / Left / Right
		p.motionX = clamp1(p.stickX + b2f(k&8 != 0) - b2f(k&4 != 0))
		p.motionY = clamp1(p.stickY + b2f(k&2 != 0) - b2f(k&1 != 0))
	}
	p.prevExtra = k
}

func (p *gpioPort) frameEnd(frameCycles int64) {
	if !p.hasRumble {
		return
	}
	now := p.g.cpu.Cycles
	if p.motorOn != 0 {
		p.motor += now - p.motorSince
		p.motorSince = now
	}
	p.rumbleLevel = math.Min(1, float64(p.motor)/float64(frameCycles))
	p.motor = 0
}

func (p *gpioPort) buttons() []string {
	switch p.kind {
	case "solar":
		return []string{"More Sun", "Less Sun"}
	case "gyro":
		return []string{"Turn Left", "Turn Right"}
	case "tilt":
		return []string{"Tilt Up", "Tilt Down", "Tilt Left", "Tilt Right"}
	}
	return nil
}

func (p *gpioPort) note() string {
	switch p.kind {
	case "solar":
		return "Sunlight: More Sun / Less Sun (I and K to start with) set the level; pushing the right stick up adds sun. Clicking the sticks (L3/R3) is used for save states."
	case "gyro":
		return "Turn the console with the right stick, or with Turn Left / Turn Right (J and L to start with). Clicking the sticks (L3/R3) is used for save states."
	case "tilt":
		return "Tilt the game with the right stick, or with the tilt keys (I J K L to start with). Clicking the sticks (L3/R3) is used for save states."
	}
	return ""
}

func (p *gpioPort) state(s *sio) {
	s.Ints(&p.dir, &p.ctrl, &p.out, &p.extra, &p.prevExtra)
	s.F64(&p.motionX)
	s.F64(&p.motionY)
	if p.solar != nil {
		s.Ints(&p.solar.count, &p.solar.base, &p.solar.clk)
		s.F64(&p.solar.level)
	}
	if p.gyro != nil {
		s.Ints(&p.gyro.word, &p.gyro.bit, &p.gyro.p)
		s.F64(&p.gyro.rate)
	}
	if p.tilt != nil {
		s.Bool(&p.tilt.armed)
		s.Ints(&p.tilt.x, &p.tilt.y, &p.tilt.ready)
	}
	s.Int(&p.motorOn)
	s.I64(&p.motor)
	s.I64(&p.motorSince)
	s.F64(&p.rumbleLevel)
}
