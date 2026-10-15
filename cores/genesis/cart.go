package genesis

import (
	"errors"
	"regexp"
	"strings"
)

// The cartridge hardware (genesis_cart.js): everything a cartridge can add to
// the 68000's $000000-$3FFFFF area and the $A130xx "time" registers: battery
// SRAM (including the >2 MB switch), serial I2C EEPROM saves (24C01 to
// 24C65, in the Sega, EA, Acclaim and Codemasters wirings), the Super Street
// Fighter II bank mapper (also used by big homebrews), Realtec's mapper, and
// Codemasters' J-Cart with two extra pads.

// ===================================================================
// Serial (I2C) EEPROM
// ===================================================================
// mode 1: X24C01 — the first byte after START is the address (7 bits) + R/W
// mode 2: 24C02-24C16 — device byte (1010 + high address bits + R/W), then one address byte
// mode 3: 24C32-24C65 — device byte, then two address bytes

const (
	stIdle = iota
	stAddr1
	stDev
	stWordHi
	stWordLo
	stWrite
	stRead
)

type eeprom struct {
	mem                  []uint8
	mask, mode, pageMask int
	scl, sda, out        int
	state, bit, shift    int
	addr                 int
	dirty                bool
}

func newEEPROM(size, mode, page int) *eeprom {
	e := &eeprom{mem: make([]uint8, size), mask: size - 1, mode: mode, pageMask: page - 1}
	for i := range e.mem {
		e.mem[i] = 0xff
	}
	e.scl, e.sda, e.out = 1, 1, 1
	return e
}

// set: the game drives the clock and data lines.
func (e *eeprom) set(scl, sda int) {
	if e.scl != 0 && scl != 0 && e.sda != sda {
		// data changing while the clock is high: START (falling) or STOP (rising)
		if sda == 0 {
			e.state = stDev
			if e.mode == 1 {
				e.state = stAddr1
			}
			e.bit, e.shift, e.out = 0, 0, 1
		} else {
			e.state, e.out = stIdle, 1
		}
	} else if e.scl == 0 && scl != 0 {
		e.rise(sda)
	} else if e.scl != 0 && scl == 0 {
		e.fall()
	}
	e.scl, e.sda = scl, sda
}

// rise: the clock rising; the receiver samples SDA. Bit 9 is the acknowledge.
func (e *eeprom) rise(sda int) {
	st := e.state
	if st == stIdle {
		return
	}
	if e.bit < 8 {
		if st != stRead {
			e.shift = (e.shift<<1 | sda) & 0xff
		}
		e.bit++
		return
	}
	// ninth clock
	e.bit = 0
	if st == stRead {
		if sda != 0 {
			e.state = stIdle // no acknowledge from the game: stop sending
		} else {
			e.addr = (e.addr + 1) & e.mask // acknowledged: next byte
		}
		return
	}
	b := e.shift
	e.shift = 0
	switch st {
	case stAddr1:
		e.addr = (b >> 1) & e.mask
		e.state = stWrite
		if b&1 != 0 {
			e.state = stRead
		}
	case stDev:
		if b&0xf0 != 0xa0 {
			e.state = stIdle
			break
		}
		if e.mode == 2 {
			e.addr = (((b>>1)&7)<<8 | e.addr&0xff) & e.mask
		}
		switch {
		case b&1 != 0:
			e.state = stRead
		case e.mode == 3:
			e.state = stWordHi
		default:
			e.state = stWordLo
		}
	case stWordHi:
		e.addr = (b<<8 | e.addr&0xff) & e.mask
		e.state = stWordLo
	case stWordLo:
		e.addr = (e.addr&^0xff | b) & e.mask
		e.state = stWrite
	case stWrite:
		if int(e.mem[e.addr]) != b {
			e.mem[e.addr] = uint8(b)
			e.dirty = true
		}
		e.addr = e.addr&^e.pageMask | (e.addr+1)&e.pageMask
	}
}

// fall: the clock falling; the EEPROM puts its next bit (or the acknowledge) on SDA.
func (e *eeprom) fall() {
	switch {
	case e.state == stRead:
		e.out = 1
		if e.bit < 8 {
			e.out = int(e.mem[e.addr]>>(7-e.bit)) & 1
		}
	case e.state != stIdle:
		e.out = 1
		if e.bit == 8 {
			e.out = 0
		}
	default:
		e.out = 1
	}
}

// level: what the game reads on SDA; the line is low if either side pulls it low.
func (e *eeprom) level() int { return e.out & e.sda }

// How each maker wired the EEPROM: address and bit for SDA in, SCL, SDA out.
type pin struct{ addr, bit int }
type wiring struct{ sda, scl, out pin }

var wirings = map[string]wiring{
	"sega":        {pin{0x200001, 0}, pin{0x200001, 1}, pin{0x200001, 0}},
	"ea":          {pin{0x200001, 7}, pin{0x200001, 6}, pin{0x200001, 7}},
	"acclaim16":   {pin{0x200001, 0}, pin{0x200001, 1}, pin{0x200001, 1}},
	"acclaim32":   {pin{0x200001, 0}, pin{0x200000, 0}, pin{0x200001, 0}},
	"codemasters": {pin{0x300000, 0}, pin{0x300000, 1}, pin{0x380001, 7}},
}

// chip: bytes, mode, page size
type chip struct{ size, mode, page int }

var (
	x24c01 = chip{128, 1, 4}
	c02    = chip{256, 2, 8}
	c04    = chip{512, 2, 16}
	c08    = chip{1024, 2, 16}
	c16    = chip{2048, 2, 16}
	c65    = chip{8192, 3, 64}
)

type eeGame struct {
	code   string
	wiring string
	chip   chip
}

// Games with an EEPROM, by the product code in the header (without the
// "-xx" revision), in the JS table's order.
var eepromGames = []eeGame{
	// Sega
	{"MK-1215", "sega", x24c01},  // Wonder Boy in Monster World
	{"G-4060", "sega", x24c01},   // Wonder Boy V: Monster World III
	{"MK-1079", "sega", x24c01},  // Evander Holyfield's "Real Deal" Boxing
	{"MK-1063", "sega", x24c01},  // Sports Talk Baseball
	{"MK-12056", "sega", x24c01}, // Mega Man: The Wily Wars
	{"T-12046", "sega", x24c01},  // Rockman Mega World
	{"T-12053", "sega", x24c01},  // Mega Man: The Wily Wars (Europe)
	{"MK-1199", "sega", x24c01},  // Greatest Heavyweights
	{"G-4524", "sega", x24c01},   // Game Toshokan
	{"G-5538", "sega", x24c01},   // Ninja Burai Densetsu
	{"G-4528", "sega", x24c01},   // Honoo no Toukyuuji Dodge Danpei
	// Electronic Arts
	{"T-50176", "ea", x24c01}, // Rings of Power
	{"T-50396", "ea", x24c01}, // NHLPA Hockey '93
	{"T-50446", "ea", x24c01}, // John Madden Football '93
	{"T-50516", "ea", x24c01}, // John Madden Football '93: Championship Edition
	{"T-50286", "ea", x24c01}, // Bill Walsh College Football
	// Acclaim
	{"T-081326", "acclaim16", c02}, // NBA Jam
	{"T-81033", "acclaim16", c02},  // NBA Jam (Japan)
	{"T-81406", "acclaim32", c04},  // NBA Jam Tournament Edition
	{"T-081276", "acclaim32", c02}, // NFL Quarterback Club
	{"T-081586", "acclaim32", c16}, // NFL Quarterback Club '96
	{"T-81576", "acclaim32", c16},  // College Slam
	{"T-81476", "acclaim32", c65},  // Frank Thomas Big Hurt Baseball
}

func isDigit(r rune) bool { return r >= '0' && r <= '9' }

// gameCode: the product code in the header, as genesis_cart.js reads it:
// headerText(0x180, 0x18e).replace(/^\s*(GM|AI|BR|AL)\s+/, "").trim()
// .split(/[\s-]+(?=\d\d$)|\s+/)[0]
func gameCode(rom []uint8) string {
	s := headerText(rom, 0x180, 0x18e)
	// the prefix
	i := 0
	for i < len(s) && jsSpace(s[i]) {
		i++
	}
	if i+2 <= len(s) {
		p := string(s[i : i+2])
		if (p == "GM" || p == "AI" || p == "BR" || p == "AL") && i+2 < len(s) && jsSpace(s[i+2]) {
			j := i + 2
			for j < len(s) && jsSpace(s[j]) {
				j++
			}
			s = s[j:]
		}
	}
	s = jsTrim(s)
	// the first piece of the split
	for q := 0; q < len(s); q++ {
		if jsSpace(s[q]) || s[q] == '-' {
			e := q
			for e < len(s) && (jsSpace(s[e]) || s[e] == '-') {
				e++
			}
			if e+2 == len(s) && isDigit(s[e]) && isDigit(s[e+1]) {
				return string(s[:q])
			}
		}
		if jsSpace(s[q]) {
			return string(s[:q])
		}
	}
	return string(s)
}

// gameNames: the domestic and overseas names, upper case, runs of white
// space as one space.
func gameNames(rom []uint8) string {
	s := append(headerText(rom, 0x120, 0x150), ' ')
	s = jsUpper(append(s, headerText(rom, 0x150, 0x180)...))
	var b strings.Builder
	sp := false
	for _, r := range s {
		if jsSpace(r) {
			if !sp {
				b.WriteByte(' ')
			}
			sp = true
			continue
		}
		sp = false
		b.WriteRune(r)
	}
	return b.String()
}

func findLong(rom []uint8, addr uint32, limit int) bool {
	b0, b1, b2, b3 := uint8(addr>>24), uint8(addr>>16), uint8(addr>>8), uint8(addr)
	n := len(rom)
	if limit > 0 && limit < n {
		n = limit
	}
	n -= 3
	for i := 0; i < n; i += 2 {
		if rom[i+3] == b3 && rom[i+2] == b2 && rom[i+1] == b1 && rom[i] == b0 {
			return true
		}
	}
	return false
}

var (
	reCodemasters = regexp.MustCompile(`MICRO ?MACHINES|BRIAN ?LARA|SHANE ?WARNE`)
	reC65         = regexp.MustCompile(`BRIAN ?LARA.*96|SHANE ?WARNE`)
	reC16         = regexp.MustCompile(`96|TURBO TOURNAMENT`)
	reVirtua      = regexp.MustCompile(`VIRTUA ?RACING`)
	threeButton   = []*regexp.Regexp{
		regexp.MustCompile(`MS\.? ?PAC-?MAN`), regexp.MustCompile(`GOLDEN AXE II\b`), regexp.MustCompile(`FORGOTTEN WORLDS`),
		regexp.MustCompile(`SUNSET RIDERS`), regexp.MustCompile(`ARCH RIVALS`), regexp.MustCompile(`SECOND SAMURAI`),
		regexp.MustCompile(`UNNECESSARY ROUGHNESS`), regexp.MustCompile(`CAPTAIN AMERICA`),
	}
)

func eepromFor(rom []uint8) (wire string, c chip, ok bool) {
	code := gameCode(rom)
	for _, g := range eepromGames {
		if code == g.code || strings.HasPrefix(code, g.code+"-") {
			return g.wiring, g.chip, true
		}
	}
	names := gameNames(rom)
	// Codemasters: their codes vary, so recognise the games by name and by the EEPROM's read address.
	if reCodemasters.MatchString(names) && findLong(rom, 0x380001, 0) {
		if reC65.MatchString(names) {
			return "codemasters", c65, true
		}
		if reC16.MatchString(names) && !strings.Contains(strings.ReplaceAll(names, "96", ""), "2") {
			return "codemasters", c16, true
		}
		return "codemasters", c08, true
	}
	// Header marks a serial EEPROM at $200001
	if rom[0x1b0] == 0x52 && rom[0x1b1] == 0x41 && rom[0x1b2] == 0xe8 && rom[0x1b3] == 0x40 {
		return "sega", x24c01, true
	}
	return "", chip{}, false
}

// needsThreeButton: games that misread a 6-button pad.
func needsThreeButton(rom []uint8) bool {
	io := string(jsUpper(headerText(rom, 0x190, 0x1a0)))
	if strings.Contains(io, "6") { // the game says it knows the 6-button pad
		return false
	}
	names := gameNames(rom)
	for _, re := range threeButton {
		if re.MatchString(names) {
			return true
		}
	}
	return false
}

// isSVP: Virtua Racing's SVP chip runs its own program from a ROM inside the chip.
func isSVP(rom []uint8) bool {
	code, names := gameCode(rom), gameNames(rom)
	return reVirtua.MatchString(names) || code == "MK-1229" || code == "G-7001"
}

// isRealtec: Realtec carts keep their header in the last 8K; at power-on that 8K is seen everywhere.
func isRealtec(rom []uint8, size int) bool {
	return size == 0x80000 && string(rom[0x100:0x104]) != "SEGA" && string(rom[0x7e100:0x7e104]) == "SEGA"
}

var errSVP = errors.New("Virtua Racing uses Sega's SVP chip, which runs a program stored inside the chip itself. RetroPixels can't play it without that program, so this game isn't supported.")

// ===================================================================
// The cartridge
// ===================================================================

type cart struct {
	rom              []uint8
	romSize, romMask int
	input            func(p int) uint32 // pad bits, for the J-Cart's pads

	sram                  [0x10000]uint8
	sramStart, sramEnd    int
	sramMapped            bool
	sramProtect, sramUsed bool

	eeWire wiring
	ee     *eeprom

	mapper string
	banks  [8]uint8
	rt     [3]uint8 // Realtec registers ($400000, $402000, $404000)

	jcart       bool
	jth         int
	threeButton bool

	pageBase, pageMask [64]int // 64K pages of the 68000's view of the cartridge
}

func newCart(rom []uint8, realSize int, sram []byte, input func(int) uint32) (*cart, error) {
	c := &cart{rom: rom, romSize: realSize, romMask: len(rom) - 1, input: input}
	if isSVP(rom) {
		return nil, errSVP
	}
	// ---- battery SRAM at $200000 ----
	for i := range c.sram {
		c.sram[i] = 0xff
	}
	hasRA := rom[0x1b0] == 0x52 && rom[0x1b1] == 0x41
	c.sramStart, c.sramEnd = 0x200000, 0x20ffff
	if hasRA {
		be := func(o int) int { return int(rom[o])<<24 | int(rom[o+1])<<16 | int(rom[o+2])<<8 | int(rom[o+3]) }
		st, en := be(0x1b4), be(0x1b8)
		if st >= 0x200000 && st < 0x400000 && en >= st {
			c.sramStart = st &^ 1
			c.sramEnd = min(en|1, c.sramStart+0xffff)
		}
	}
	c.sramMapped = realSize <= 0x200000 // bigger games switch it in through $A130F1
	c.sramUsed = hasRA
	if len(sram) > 0 {
		copy(c.sram[:], sram)
		c.sramUsed = true
	}
	// ---- EEPROM ----
	if wire, ch, ok := eepromFor(rom); ok {
		c.eeWire = wirings[wire]
		c.ee = newEEPROM(ch.size, ch.mode, ch.page)
		c.sramMapped, c.sramUsed = false, true
		if len(sram) > 0 {
			copy(c.ee.mem, sram)
		}
	}
	// ---- mappers ----
	switch {
	case isRealtec(rom, realSize):
		c.mapper = "realtec"
	case realSize > 0x400000:
		c.mapper = "ssf2"
	}
	for i := range c.banks {
		c.banks[i] = uint8(i)
	}
	// ---- J-Cart: two more pads read at $38FFFE ----
	c.jcart = findLong(rom, 0x38fffe, 0x80000) || findLong(rom, 0x38ffff, 0x80000)
	c.threeButton = needsThreeButton(rom)
	c.remap()
	return c, nil
}

// reset: the console's reset button also resets the mapper registers.
func (c *cart) reset() {
	for i := range c.banks {
		c.banks[i] = uint8(i)
	}
	c.rt = [3]uint8{}
	c.jth = 0
	c.sramMapped = c.romSize <= 0x200000 && c.ee == nil
	c.sramProtect = false
	c.remap()
}

func (c *cart) remap() {
	for p := 0; p < 64; p++ {
		base, mask := p<<16, 0xffff
		switch c.mapper {
		case "ssf2":
			base = int(c.banks[p>>3])<<19 | (p&7)<<16
		case "realtec":
			if c.rt[0]&1 == 0 {
				base, mask = 0x7e000, 0x1fff // the boot 8K everywhere
			} else {
				bank := int(c.rt[0]&6)<<2 | int(c.rt[2]&7)
				size := int(c.rt[1] & 0x1f)
				if size == 0 {
					size = 1
				}
				base = (bank+(p>>1)%size)<<17 | (p&1)<<16
			}
		}
		c.pageBase[p], c.pageMask[p] = base&c.romMask, mask
	}
}

func (c *cart) romByte(a int) int {
	p := a >> 16
	return int(c.rom[(c.pageBase[p]+a&c.pageMask[p])&c.romMask])
}

// sramAt: a battery RAM byte; past the 64K the JS core reads undefined,
// which its callers treat as 0.
func (c *cart) sramAt(o int) int {
	if o < len(c.sram) {
		return int(c.sram[o])
	}
	return 0
}

// ---- 68000 reads ($000000-$3FFFFF) ----

func (c *cart) read8(a int) int {
	if a >= 0x200000 {
		if c.ee != nil && a&^1 == c.eeWire.out.addr&^1 {
			if a == c.eeWire.out.addr {
				return c.ee.level() << c.eeWire.out.bit
			}
			return 0
		}
		if c.jcart && a&^1 == 0x38fffe {
			w := c.jcartWord()
			if a&1 != 0 {
				return w & 0xff
			}
			return w >> 8
		}
		if c.sramMapped && a >= c.sramStart && a <= c.sramEnd {
			return c.sramAt(a - 0x200000)
		}
	}
	return c.romByte(a)
}

func (c *cart) read16(a int) int {
	if a >= 0x200000 {
		if (c.ee != nil && a == c.eeWire.out.addr&^1) || (c.jcart && a == 0x38fffe) {
			return c.read8(a)<<8 | c.read8(a+1)
		}
		if c.sramMapped && a >= c.sramStart && a <= c.sramEnd {
			o := a - 0x200000
			return c.sramAt(o)<<8 | c.sramAt(o+1)
		}
	}
	p := a >> 16
	o := (c.pageBase[p] + a&c.pageMask[p]) & c.romMask
	return int(c.rom[o])<<8 | int(c.rom[o+1])
}

// ---- 68000 writes ----

func (c *cart) write8(a, v int) {
	if c.ee != nil && (a == c.eeWire.sda.addr || a == c.eeWire.scl.addr) {
		c.eeWrite(a, v, -1, 0)
		return
	}
	if c.jcart && a&^1 == 0x38fffe {
		if a&1 != 0 {
			c.jth = v & 1
		}
		return
	}
	if c.mapper == "realtec" && a >= 0x400000 {
		return
	}
	if c.sramMapped && !c.sramProtect && a >= c.sramStart && a <= c.sramEnd {
		o := a - 0x200000
		if o >= len(c.sram) || int(c.sram[o]) != v {
			if o < len(c.sram) {
				c.sram[o] = uint8(v)
			}
			c.sramUsed = true
		}
	}
}

func (c *cart) write16(a, v int) {
	if c.ee != nil && (a&^1 == c.eeWire.sda.addr&^1 || a&^1 == c.eeWire.scl.addr&^1) {
		c.eeWrite(a, v>>8, a+1, v&0xff)
		return
	}
	if c.jcart && a == 0x38fffe {
		c.jth = v & 1
		return
	}
	c.write8(a, v>>8)
	c.write8(a+1, v&0xff)
}

// eeWrite: one or two bytes written (a2 < 0: one) to the EEPROM's lines.
func (c *cart) eeWrite(a1, b1, a2, b2 int) {
	w, e := c.eeWire, c.ee
	sda, scl, hit := e.sda, e.scl, false
	for k := 0; k < 2; k++ {
		ad, b := a1, b1
		if k == 1 {
			if a2 < 0 {
				break
			}
			ad, b = a2, b2
		}
		if ad == w.sda.addr {
			sda = (b >> w.sda.bit) & 1
			hit = true
		}
		if ad == w.scl.addr {
			scl = (b >> w.scl.bit) & 1
			hit = true
		}
	}
	if hit {
		e.set(scl, sda)
	}
}

// highWrite: writes to $400000-$7FFFFF (Realtec's registers live there).
func (c *cart) highWrite(a, v int) {
	if c.mapper != "realtec" {
		return
	}
	switch a {
	case 0x400000, 0x400001:
		c.rt[0] = uint8(v)
	case 0x402000, 0x402001:
		c.rt[1] = uint8(v)
	case 0x404000, 0x404001:
		c.rt[2] = uint8(v)
	default:
		return
	}
	c.remap()
}

// timeWrite: the $A130xx registers: SRAM switch at $A130F1, SSF2 banks at $A130F3-$A130FF.
func (c *cart) timeWrite(a, v int) {
	if a == 0xa130f1 {
		c.sramMapped = v&1 != 0 || c.romSize <= 0x200000
		if c.ee != nil {
			c.sramMapped = false
		}
		c.sramProtect = v&2 != 0
	} else if c.mapper == "ssf2" && a&1 != 0 && a > 0xa130f1 && a < 0xa13100 {
		c.banks[(a&0xf)>>1] = uint8(v & 0x3f)
		c.remap()
	}
}

// jcartWord: pads 3 (low byte) and 4 (high byte) as 3-button pads, with the TH line the game sets.
func (c *cart) jcartWord() int {
	pad := func(bits uint32) int {
		on := func(n uint) int { // bit order: A B C Start X Y Z Mode Up Down Left Right
			if bits&(1<<n) != 0 {
				return 0
			}
			return 1
		}
		if c.jth != 0 {
			return 0x40 | on(2)<<5 | on(1)<<4 | on(11)<<3 | on(10)<<2 | on(9)<<1 | on(8)
		}
		return on(3)<<5 | on(0)<<4 | on(9)<<1 | on(8)
	}
	return pad(c.input(3))<<8 | pad(c.input(2))
}

func (c *cart) getSRAM() []byte {
	if c.ee != nil {
		return append([]byte(nil), c.ee.mem...)
	}
	if !c.sramUsed {
		return nil
	}
	n := max(0x4000, c.sramEnd-0x200000+1)
	if n > len(c.sram) {
		// JS's slice stops at the end of the 64K
		n = len(c.sram)
	}
	return append([]byte(nil), c.sram[:n]...)
}
