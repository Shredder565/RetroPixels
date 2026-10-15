package atari2600

// Atari 2600 cartridges: every bank-switching scheme RetroPixels knows, and
// how a game file's scheme is recognised (a port of atari2600_carts.js).
//
// A cartridge sees the 4K at $1000-$1FFF. Some schemes also watch the whole bus
// (access()), the value on the bus after an access (watch()), or TIA writes
// (tiaWrite()). Detection uses the file size, the hotspots the game's code
// touches (found by following the code from its start vector), a few text
// markers, and a scheme name in the file name, e.g.
// "Game (F8SC).bin" or "Game [3E+].a26".

import (
	"bytes"
	"errors"
	"fmt"
	"math"
	"regexp"
	"slices"
	"strings"
)

const kb = 1024

type cart interface {
	read(a int, m *Atari) uint8
	write(a int, v uint8, m *Atari)
	sram() []byte
	save(w *stateW)
	load(r *stateR)
	clone() cart
	kind() string
}

// Optional cartridge hooks.
type snooper interface{ access(a int) }         // every bus access
type watcher interface{ watch(a int, v uint8) } // the value on the bus after an access
type tiaHooker interface{ tiaWrite(a int, v uint8) }

// at reads rom[i], or 0 past its end.
func at(rom []byte, i int) uint8 {
	if i >= 0 && i < len(rom) {
		return rom[i]
	}
	return 0
}

// ------------------------------------------------------------------
// Byte-search helpers
// ------------------------------------------------------------------

func hasText(rom []byte, s string) bool { return bytes.Contains(rom, []byte(s)) }

// Absolute-address instructions (loads, stores, compares, BIT, NOP abs).
var absOps = opSet(0xad, 0xae, 0xac, 0x2c, 0x0c, 0x8d, 0x8e, 0x8c, 0xcd, 0xec, 0xcc, 0xbd, 0xb9, 0x9d, 0x99, 0xbe, 0xbc)
var indexedOps = opSet(0xbd, 0xb9, 0x9d, 0x99, 0xbe, 0xbc)
var plainOps = func() (s [256]bool) {
	for i := range s {
		s[i] = absOps[i] && !indexedOps[i]
	}
	return
}()
var indexedStores = opSet(0x9d, 0x99)

func opSet(ops ...int) (s [256]bool) {
	for _, o := range ops {
		s[o] = true
	}
	return
}

// countAbs counts absolute instructions anywhere in the file whose address passes test.
func countAbs(rom []byte, test func(int) bool) int {
	n := 0
	for i := 0; i+2 < len(rom); i++ {
		if absOps[rom[i]] && test(int(rom[i+1])|int(rom[i+2])<<8) {
			n++
		}
	}
	return n
}

// ------------------------------------------------------------------
// Following the game's code
// ------------------------------------------------------------------
// Data and the middle of instructions are full of byte pairs that look like
// addresses, so detection only looks at instructions reached by following the
// code from the cartridge's start vectors.

// opLength is a 6502 instruction's length, from its opcode's bit fields
// (aaabbbcc); 0 for those after which the code doesn't simply go on (BRK, RTI,
// RTS, JMP, the jams).
func opLength(op int) int {
	if op == 0x00 || op == 0x40 || op == 0x60 || op == 0x4c || op == 0x6c {
		return 0
	}
	if op == 0x20 {
		return 3
	}
	b, c := (op>>2)&7, op&3
	if c == 2 && (b == 4 || (b == 0 && op < 0x80)) {
		return 0
	}
	if b == 3 || b == 7 {
		return 3 // absolute, absolute,X
	}
	if b == 2 {
		if c&1 != 0 {
			return 2 // immediate
		}
		return 1 // implied / accumulator
	}
	if b == 6 {
		if c&1 != 0 {
			return 3 // absolute,Y
		}
		return 1
	}
	return 2 // zero page forms, (zp,X), (zp),Y, immediate, branches
}

type insn struct{ op, a int }

// trace follows the code in rom[start : start+size), built to run at address
// home, from the entry offsets given. visit sees every instruction with an
// operand. Jumps and calls are followed when they land in the same block.
func trace(rom []byte, start, size, home int, entries []int, visit func(op, a int)) {
	seen := make([]bool, size)
	todo := slices.Clone(entries)
	for len(todo) > 0 {
		pc := todo[len(todo)-1]
		todo = todo[:len(todo)-1]
		for pc >= 0 && pc < size-2 && !seen[pc] {
			seen[pc] = true
			op := int(rom[start+pc])
			n := opLength(op)
			lo := int(rom[start+pc+1])
			word := lo | int(rom[start+pc+2])<<8
			if op == 0x20 || op == 0x4c {
				visit(op, word)
				if word&-size&0xffff == home {
					todo = append(todo, word&(size-1))
				}
			} else if n == 3 {
				visit(op, word)
			} else if n == 2 {
				visit(op, lo)
			}
			if op&0x1f == 0x10 { // branch
				todo = append(todo, pc+2+((lo^0x80)-0x80))
			}
			if n == 0 {
				break
			}
			pc += n
		}
	}
}

func vector(rom []byte, end int) int { return int(rom[end-4]) | int(rom[end-3])<<8 }

// detector holds a file and what was found by following its code.
type detector struct {
	rom  []byte
	list []insn
	done bool
}

// code is every instruction (op, operand) reached from the start vectors at
// the end of each 4K (or of the whole file, when it is smaller).
func (d *detector) code() []insn {
	if d.done {
		return d.list
	}
	d.done = true
	rom := d.rom
	size := 1024
	if len(rom) >= 4096 {
		size = 4096
	} else if len(rom) >= 2048 {
		size = 2048
	}
	for end := len(rom); end-size >= 0; end -= size {
		reset, irq := vector(rom, end), vector(rom, end+2)
		home := reset & -size & 0xffff
		if reset&0x1000 == 0 {
			continue
		}
		entries := []int{reset & (size - 1)}
		if irq&-size&0xffff == home {
			entries = append(entries, irq&(size-1))
		}
		trace(rom, end-size, size, home, entries, func(op, a int) { d.list = append(d.list, insn{op, a}) })
	}
	return d.list
}

// touched is the set of different addresses (as the cartridge sees them, 13
// bits) that the code's absolute instructions touch and that pass test (given
// the address as written). An indexed instruction is taken to reach one byte
// past its base.
func (d *detector) touched(test func(int) bool, ops *[256]bool) map[int]bool {
	seen := map[int]bool{}
	for _, in := range d.code() {
		if !ops[in.op] {
			continue
		}
		if test(in.a) {
			seen[in.a&0x1fff] = true
		}
		if indexedOps[in.op] && test((in.a+1)&0xffff) {
			seen[(in.a+1)&0x1fff] = true
		}
	}
	return seen
}

func between(lo, hi int) func(int) bool {
	return func(a int) bool { return a&0x1fff >= lo && a&0x1fff <= hi }
}

// uses: does the code touch the cartridge addresses lo-hi (e.g. the F8 hotspots)?
func (d *detector) uses(lo, hi int) bool { return len(d.touched(between(lo, hi), &absOps)) > 0 }

// writesZp: zero-page writes (STA / STX / STY) to address z.
func (d *detector) writesZp(z int) bool {
	for _, in := range d.code() {
		if (in.op == 0x85 || in.op == 0x86 || in.op == 0x84) && in.a == z {
			return true
		}
	}
	return false
}

// Parker Brothers E0: two different slice selects in $1FE0-$1FF7, and none of F8's.
func (d *detector) isE0() bool {
	return len(d.touched(between(0x1fe0, 0x1ff7), &plainOps)) >= 2 && !d.uses(0x1ff8, 0x1ff9)
}

// M-Network E7: two different selects in $1FE0-$1FEB, and none of the plain scheme's (F6 or FA).
func (d *detector) isE7(lo, hi int) bool {
	return len(d.touched(between(0x1fe0, 0x1feb), &plainOps)) >= 2 && !d.uses(lo, hi)
}

// both halves of a two-bank scheme selected
func both(set map[int]bool, bank1 func(int) bool) bool {
	a, b := false, false
	for x := range set {
		if bank1(x) {
			b = true
		} else {
			a = true
		}
	}
	return a && b
}

// 0840: address bit 6 picks the bank.
func (d *detector) is0840() bool {
	return both(d.touched(func(a int) bool { return a >= 0x0800 && a < 0x1000 }, &plainOps), func(a int) bool { return a&0x40 != 0 })
}

// UA: $0220 / $0240 (or a mirror).
func (d *detector) isUA() bool {
	return both(d.touched(func(a int) bool { return a < 0x1000 && (a&0x1260 == 0x0220 || a&0x1260 == 0x0240) }, &absOps),
		func(a int) bool { return a&0x1260 == 0x0240 })
}

// Activision FE: the second 4K is built for $F000 and calls into the first, built
// for $D000, which calls back; nothing touches F8's hotspots.
func (d *detector) isFE() bool {
	rom := d.rom
	if len(rom) != 8192 || vector(rom, 8192)&0xf000 != 0xf000 || d.uses(0x1ff8, 0x1ff9) {
		return false
	}
	into := func(list []insn, hi int) []int {
		var out []int
		for _, in := range list {
			if in.op == 0x20 && in.a&0xf000 == hi {
				out = append(out, in.a&0xfff)
			}
		}
		return out
	}
	var low []insn
	trace(rom, 0, 4096, 0xd000, into(d.code(), 0xd000), func(op, a int) { low = append(low, insn{op, a}) })
	if len(into(low, 0xf000)) == 0 {
		return false
	}
	hs := between(0x1ff8, 0x1ff9)
	for _, in := range low {
		if absOps[in.op] && hs(in.a) {
			return false
		}
	}
	return true
}

// CommaVid: indexed stores into the RAM's write port, $1400-$17FF.
func (d *detector) isCV() bool {
	return len(d.touched(func(a int) bool { return a&0x1c00 == 0x1400 && a&0x1000 != 0 }, &indexedStores)) > 0
}

// hasSuperchip: a Superchip cart leaves the RAM's address range in every bank
// unused (filled with one value). (Some dumps instead show the RAM's read port
// as a copy of its write port.)
func hasSuperchip(rom []byte) bool {
	get := func(i int) int {
		if i < len(rom) {
			return int(rom[i])
		}
		return -1 // JS undefined
	}
	filled, mirrored := true, true
	for b := 0; b < len(rom); b += 4096 {
		v := get(b)
		for i := 1; i < 256; i++ {
			if get(b+i) != v || get(b+i) < 0 {
				filled = false
				break
			}
		}
		for i := 0; i < 128; i++ {
			if get(b+i) != get(b+128+i) {
				mirrored = false
				break
			}
		}
	}
	return filled || mirrored
}

// Tigervision 3F: the code selects banks by writing $3F; 3E also writes $3E (its RAM).
func (d *detector) tigervision() bool { return d.writesZp(0x3f) }
func (d *detector) ram3E() bool       { return d.writesZp(0x3e) && d.tigervision() }

// Scheme names RetroPixels understands (and a few other names people use for them).
var schemeNames = []string{"2K", "4K", "CV", "F8", "F8SC", "F6", "F6SC", "F4", "F4SC", "FA", "FA2", "EF", "EFSC", "DF", "DFSC", "BF", "BFSC",
	"F0", "E0", "E7", "FE", "3F", "3E", "3E+", "0840", "UA", "UASW", "X07", "SB", "MDM", "DPC", "DPC+", "AR", "4A50", "CDF", "CDFJ", "CDFJ+"}
var schemeAliases = map[string]string{"3EP": "3E+", "3EPLUS": "3E+", "DPCP": "DPC+", "MB": "F0", "SC": "F8SC", "SUPERCHARGER": "AR", "ECONOBANKING": "0840"}

var (
	tagRe   = regexp.MustCompile(`[\[(]([^\])]+)[\])]`)
	splitRe = regexp.MustCompile(`[\s,;]+`)
	bsRe    = regexp.MustCompile(`^BS[-_]?`)
)

// typeFromName finds a scheme name in brackets in the file name.
func typeFromName(name string) string {
	if name == "" {
		return ""
	}
	for _, t := range tagRe.FindAllString(strings.ToUpper(name), -1) {
		for _, w := range splitRe.Split(t[1:len(t)-1], -1) {
			w = bsRe.ReplaceAllString(w, "")
			if a, ok := schemeAliases[w]; ok {
				return a
			}
			if slices.Contains(schemeNames, w) {
				return w
			}
		}
	}
	return ""
}

// detectType works out the scheme from the file itself.
func detectType(rom []byte, name string) (string, error) {
	if hint := typeFromName(name); hint != "" {
		return hint, nil
	}
	d := &detector{rom: rom}
	n := len(rom)
	switch {
	case n >= 8448 && n%8448 == 0:
		return "AR", nil
	case n == 6144:
		return "AR6K", nil
	case n == 10240 || n == 10495:
		return "DPC", nil
	}
	if hasText(rom, "TJ3E") {
		return "3E+", nil
	}
	if n >= 8192 && n%8192 == 0 && n <= 8192*1024 && hasText(rom[:4096], "MDMC") {
		return "MDM", nil
	}
	if n <= 2048 {
		if d.isCV() {
			return "CV", nil
		}
		return "2K", nil
	}
	if n <= 4096 {
		if d.isCV() {
			return "CV", nil
		}
		if n == 4096 {
			return "4K", nil
		}
		return "2K", nil
	}
	sc := func(with, without string) string {
		if hasSuperchip(rom) {
			return with
		}
		return without
	}
	switch n {
	case 8192:
		switch {
		case d.isE0():
			return "E0", nil
		case d.is0840():
			return "0840", nil
		case d.isUA():
			return "UA", nil
		case d.ram3E():
			return "3E", nil
		case d.tigervision():
			return "3F", nil
		case d.isFE():
			return "FE", nil
		}
		return sc("F8SC", "F8"), nil
	case 12288:
		if d.isE7(0x1ff8, 0x1ffa) {
			return "E7", nil
		}
		return "FA", nil
	case 16384:
		switch {
		case d.isE7(0x1ff6, 0x1ff9):
			return "E7", nil
		case d.ram3E():
			return "3E", nil
		case d.tigervision():
			return "3F", nil
		}
		return sc("F6SC", "F6"), nil
	case 24 * kb, 28 * kb, 29 * kb:
		return "FA2", nil
	case 32768:
		switch {
		case hasText(rom, "DPC+"):
			return "DPC+", nil
		case hasText(rom, "CDFJ"):
			return "CDFJ", nil
		case hasText(rom, "CDF"):
			return "CDF", nil
		case d.ram3E():
			return "3E", nil
		case d.tigervision():
			return "3F", nil
		}
		return sc("F4SC", "F4"), nil
	case 65536:
		switch {
		case hasText(rom, "CDFJ"):
			return "CDFJ", nil
		case hasText(rom, "EFEF"):
			return "EF", nil
		case hasText(rom, "EFSC"):
			return "EFSC", nil
		case d.ram3E():
			return "3E", nil
		case d.tigervision():
			return "3F", nil
		}
		// X07: hotspots at $080D-$08FD
		if countAbs(rom, func(a int) bool { return a&0x180f == 0x080d }) >= 2 {
			return "X07", nil
		}
		// F0 / Megaboy: one hotspot at $1FF0 that steps through the banks
		if countAbs(rom, func(a int) bool { return a&0x1fff == 0x1ff0 }) >= 1 &&
			countAbs(rom, func(a int) bool { return a&0x1fff > 0x1fe0 && a&0x1fff <= 0x1fef }) == 0 {
			return "F0", nil
		}
		return sc("EFSC", "EF"), nil
	case 131072:
		switch {
		case hasText(rom, "DFSC"):
			return "DFSC", nil
		case hasText(rom, "DFDF"):
			return "DF", nil
		case d.ram3E():
			return "3E", nil
		case d.tigervision():
			return "3F", nil
		}
		if countAbs(rom, func(a int) bool { return a&0x1fff >= 0x1fc0 && a&0x1fff <= 0x1fdf }) >= 2 {
			return sc("DFSC", "DF"), nil
		}
		return "SB", nil
	case 262144:
		switch {
		case hasText(rom, "BFSC"):
			return "BFSC", nil
		case hasText(rom, "BFBF"):
			return "BF", nil
		case d.ram3E():
			return "3E", nil
		case d.tigervision():
			return "3F", nil
		}
		if countAbs(rom, func(a int) bool { return a&0x1fff >= 0x1f80 && a&0x1fff <= 0x1fbf }) >= 2 {
			return sc("BFSC", "BF"), nil
		}
		return "SB", nil
	}
	if n%2048 == 0 && d.ram3E() {
		return "3E", nil
	}
	if n%2048 == 0 && d.tigervision() {
		return "3F", nil
	}
	if n%4096 == 0 && n > 65536 {
		return "SB", nil
	}
	return "", fmt.Errorf("This Atari 2600 cartridge uses hardware RetroPixels doesn't support yet (%d bytes).", n)
}

var unsupported = map[string]string{
	"4A50":  "This Atari 2600 game uses the 4A50 cartridge, which RetroPixels doesn't support yet.",
	"CDF":   "This Atari 2600 game uses a CDF (Harmony / Melody) cartridge, which RetroPixels doesn't support yet.",
	"CDFJ":  "This Atari 2600 game uses a CDFJ (Harmony / Melody) cartridge, which RetroPixels doesn't support yet.",
	"CDFJ+": "This Atari 2600 game uses a CDFJ+ (Harmony / Melody) cartridge, which RetroPixels doesn't support yet.",
	"AR6K":  "This looks like a 6K Supercharger dump without load headers. RetroPixels needs the usual Supercharger file (8448 bytes per load).",
}

// ------------------------------------------------------------------
// The base cartridge: plain 2K / 4K ROM
// ------------------------------------------------------------------
type plainCart struct {
	typ string
	rom []byte
}

func (c *plainCart) kind() string               { return c.typ }
func (c *plainCart) read(a int, _ *Atari) uint8 { return c.rom[a&(len(c.rom)-1)] }
func (c *plainCart) write(int, uint8, *Atari)   {}
func (c *plainCart) sram() []byte               { return nil }
func (c *plainCart) save(*stateW)               {}
func (c *plainCart) load(*stateR)               {}
func (c *plainCart) clone() cart                { n := *c; return &n }

// ------------------------------------------------------------------
// Hotspot schemes: touching one of a run of addresses near the top
// selects a 4K bank (F8, F6, F4, FA, FA2, EF, DF, BF), optionally with
// the 128-byte Superchip RAM (write $1000-$107F, read $1080-$10FF) or
// CBS's 256 bytes (write $1000-$10FF, read $1100-$11FF).
// ------------------------------------------------------------------
var fx = map[string][3]int{
	"F8": {0xff8, 2, 1}, "F6": {0xff6, 4, 0}, "F4": {0xff4, 8, 0}, "FA": {0xff8, 3, 2}, "FA2": {0xff5, 7, 0},
	"EF": {0xfe0, 16, 15}, "DF": {0xfc0, 32, 31}, "BF": {0xf80, 64, 63},
}

type hotspotCart struct {
	typ       string
	rom       []byte
	hsLo      int
	nBanks    int
	bank      int
	ramSize   int
	ram       []uint8
	flash     []uint8 // FA2: the Harmony's flash copy of the RAM (nil otherwise)
	flashUsed bool
}

func newHotspotCart(rom []byte, typ string, sram []byte) *hotspotCart {
	base := strings.TrimSuffix(typ, "SC")
	c := &hotspotCart{typ: typ, rom: rom}
	f := fx[base]
	lo, banks, start := f[0], f[1], f[2]
	if base == "FA2" {
		if len(rom) == 29*kb {
			rom = rom[kb:]
			c.rom = rom
		}
		banks = len(rom) >> 12
	}
	c.hsLo = lo
	c.nBanks = max(1, min(banks, len(rom)>>12))
	c.bank = min(start, c.nBanks-1)
	switch {
	case strings.HasSuffix(typ, "SC"):
		c.ramSize = 128
	case base == "FA" || base == "FA2":
		c.ramSize = 256
	}
	if c.ramSize > 0 {
		c.ram = make([]uint8, c.ramSize)
		for i := range c.ram {
			c.ram[i] = uint8(i*37 + 11)
		}
	}
	if base == "FA2" {
		c.flash = make([]uint8, 256)
		copy(c.flash, sram)
		c.flashUsed = len(sram) > 0
	}
	return c
}

func (c *hotspotCart) kind() string { return c.typ }

func (c *hotspotCart) hot(off int) {
	i := off - c.hsLo
	if i >= 0 && i < c.nBanks {
		c.bank = i
	} else if c.flash != nil && off == 0xff4 {
		c.flashOp()
	}
}

// flashOp: FA2 on a Harmony cart can keep its 256 bytes of RAM in flash
// memory: the game puts 1 (load) or 2 (save) in its last RAM byte and touches $1FF4.
func (c *hotspotCart) flashOp() {
	cmd := c.ram[255]
	if cmd == 1 {
		copy(c.ram[:255], c.flash[:255])
	} else if cmd == 2 {
		copy(c.flash[:255], c.ram[:255])
		c.flashUsed = true
	}
	if cmd == 1 || cmd == 2 {
		c.ram[255] = 0
	}
}

func (c *hotspotCart) read(a int, _ *Atari) uint8 {
	off := a & 0xfff
	c.hot(off)
	if rs := c.ramSize; rs != 0 && off < 2*rs {
		if off >= rs {
			return c.ram[off-rs]
		}
		return c.ram[off] // reading the write port gives junk; use the RAM
	}
	return at(c.rom, c.bank<<12|off)
}

func (c *hotspotCart) write(a int, v uint8, _ *Atari) {
	off := a & 0xfff
	c.hot(off)
	if c.ramSize != 0 && off < c.ramSize {
		c.ram[off] = v
	}
}

func (c *hotspotCart) sram() []byte {
	if c.flash != nil && c.flashUsed {
		return slices.Clone(c.flash)
	}
	return nil
}

// The state includes FA2's flash copy (the web core leaves it out), so a
// loaded state continues exactly as it would have.
func (c *hotspotCart) save(w *stateW) {
	w.int(c.bank)
	w.bytes(c.ram)
	w.bool(c.flashUsed)
	w.bytes(c.flash)
}
func (c *hotspotCart) load(r *stateR) {
	c.bank = r.int()
	r.bytesInto(c.ram)
	c.flashUsed = r.bool()
	r.bytesInto(c.flash)
}
func (c *hotspotCart) clone() cart {
	n := *c
	n.ram = slices.Clone(c.ram)
	n.flash = slices.Clone(c.flash)
	return &n
}

// ------------------------------------------------------------------
// F0 (Dynacom Megaboy): every touch of $1FF0 steps to the next of 16 banks.
// ------------------------------------------------------------------
type f0Cart struct {
	rom          []byte
	nBanks, bank int
}

func (c *f0Cart) kind() string { return "F0" }
func (c *f0Cart) step() {
	if c.nBanks > 0 {
		c.bank = (c.bank + 1) % c.nBanks
	}
}
func (c *f0Cart) read(a int, _ *Atari) uint8 {
	off := a & 0xfff
	if off == 0xff0 {
		c.step()
	}
	return at(c.rom, c.bank<<12|off)
}
func (c *f0Cart) write(a int, _ uint8, _ *Atari) {
	if a&0xfff == 0xff0 {
		c.step()
	}
}
func (c *f0Cart) sram() []byte   { return nil }
func (c *f0Cart) save(w *stateW) { w.int(c.bank) }
func (c *f0Cart) load(r *stateR) { c.bank = r.int() }
func (c *f0Cart) clone() cart    { n := *c; return &n }

// ------------------------------------------------------------------
// Parker Brothers (E0): four 1K slices; the last is fixed to the last 1K.
// ------------------------------------------------------------------
type e0Cart struct {
	rom    []byte
	slices [4]int
}

func (c *e0Cart) kind() string { return "E0" }
func (c *e0Cart) hot(off int) {
	if off >= 0xfe0 && off <= 0xff7 {
		c.slices[(off-0xfe0)>>3] = off & 7
	}
}
func (c *e0Cart) read(a int, _ *Atari) uint8 {
	off := a & 0xfff
	c.hot(off)
	return at(c.rom, c.slices[off>>10]*1024+off&0x3ff)
}
func (c *e0Cart) write(a int, _ uint8, _ *Atari) { c.hot(a & 0xfff) }
func (c *e0Cart) sram() []byte                   { return nil }
func (c *e0Cart) save(w *stateW)                 { w.ints(c.slices[:]) }
func (c *e0Cart) load(r *stateR)                 { r.intsInto(c.slices[:]) }
func (c *e0Cart) clone() cart                    { n := *c; return &n }

// ------------------------------------------------------------------
// M-Network (E7): 2K slice at $1000 (ROM, or 1K of RAM), 256-byte RAM
// window at $1800, and the end of the last 2K fixed at $1A00-$1FFF.
// 12K versions simply lack the first two 2K slices.
// ------------------------------------------------------------------
type e7Cart struct {
	rom           []byte
	bank, ramBank int
	ram           [2048]uint8
}

func newE7Cart(rom []byte) *e7Cart {
	if len(rom) < 16384 {
		r := make([]byte, 16384)
		copy(r[16384-len(rom):], rom)
		rom = r
	}
	return &e7Cart{rom: rom}
}
func (c *e7Cart) kind() string { return "E7" }
func (c *e7Cart) hot(off int) {
	if off >= 0xfe0 && off <= 0xfe7 {
		c.bank = off - 0xfe0
	} else if off >= 0xfe8 && off <= 0xfeb {
		c.ramBank = off - 0xfe8
	}
}
func (c *e7Cart) read(a int, _ *Atari) uint8 {
	off := a & 0xfff
	c.hot(off)
	if off < 0x800 {
		if c.bank == 7 {
			return c.ram[off&0x3ff]
		}
		return at(c.rom, c.bank*2048+off)
	}
	if off < 0xa00 {
		return c.ram[1024+c.ramBank*256+off&0xff]
	}
	return at(c.rom, 7*2048+off-0x800)
}
func (c *e7Cart) write(a int, v uint8, _ *Atari) {
	off := a & 0xfff
	c.hot(off)
	if off < 0x400 && c.bank == 7 {
		c.ram[off] = v
	} else if off >= 0x800 && off < 0x900 {
		c.ram[1024+c.ramBank*256+off-0x800] = v
	}
}
func (c *e7Cart) sram() []byte { return nil }
func (c *e7Cart) save(w *stateW) {
	w.int(c.bank)
	w.int(c.ramBank)
	w.bytes(c.ram[:])
}
func (c *e7Cart) load(r *stateR) {
	c.bank = r.int()
	c.ramBank = r.int()
	r.bytesInto(c.ram[:])
}
func (c *e7Cart) clone() cart { n := *c; return &n }

// ------------------------------------------------------------------
// Activision (FE): the bank follows bit 5 of the high byte of a JSR/RTS
// return address, seen on the bus right after an access to $01FE.
// ------------------------------------------------------------------
type feCart struct {
	rom    []byte
	bank   int
	lastFE bool
}

func (c *feCart) kind() string               { return "FE" }
func (c *feCart) read(a int, _ *Atari) uint8 { return at(c.rom, c.bank<<12|a&0xfff) }
func (c *feCart) write(int, uint8, *Atari)   {}
func (c *feCart) watch(a int, v uint8) {
	if c.lastFE {
		if v&0x20 != 0 {
			c.bank = 0
		} else {
			c.bank = 1
		}
	}
	c.lastFE = a == 0x01fe
}
func (c *feCart) sram() []byte { return nil }
func (c *feCart) save(w *stateW) {
	w.int(c.bank)
	w.bool(c.lastFE)
}
func (c *feCart) load(r *stateR) {
	c.bank = r.int()
	c.lastFE = r.bool()
}
func (c *feCart) clone() cart { n := *c; return &n }

// ------------------------------------------------------------------
// Tigervision (3F): writing to $00-$3F picks the 2K ROM bank at $1000;
// the last 2K is fixed at $1800. 3E adds up to 32K of RAM: writing to $3E
// puts a 1K RAM bank at $1000 (read $1000-$13FF, write $1400-$17FF).
// ------------------------------------------------------------------
type tigerCart struct {
	typ          string
	rom          []byte
	nBanks, bank int
	isRAM        bool
	ram          []uint8 // 3E only
}

func newTigerCart(rom []byte, typ string) *tigerCart {
	c := &tigerCart{typ: typ, rom: rom, nBanks: len(rom) >> 11}
	if typ == "3E" {
		c.ram = make([]uint8, 32*kb)
	}
	return c
}
func (c *tigerCart) kind() string { return c.typ }
func (c *tigerCart) read(a int, _ *Atari) uint8 {
	off := a & 0xfff
	if off >= 0x800 {
		return at(c.rom, (c.nBanks-1)*2048+off-0x800)
	}
	if c.isRAM {
		if off < 0x400 {
			return c.ram[c.bank*1024+off]
		}
		return 0
	}
	return at(c.rom, c.bank*2048+off)
}
func (c *tigerCart) write(a int, v uint8, _ *Atari) {
	off := a & 0xfff
	if c.isRAM && off >= 0x400 && off < 0x800 {
		c.ram[c.bank*1024+off-0x400] = v
	}
}
func (c *tigerCart) tiaWrite(a int, v uint8) {
	if c.ram != nil && a == 0x3e {
		c.bank = int(v & 31)
		c.isRAM = true
	} else if (c.ram != nil && a == 0x3f) || (c.ram == nil && a < 0x40) {
		if c.nBanks > 0 {
			c.bank = int(v) % c.nBanks
		}
		c.isRAM = false
	}
}
func (c *tigerCart) sram() []byte { return nil }
func (c *tigerCart) save(w *stateW) {
	w.int(c.bank)
	w.bool(c.isRAM)
	w.bytes(c.ram)
}
func (c *tigerCart) load(r *stateR) {
	c.bank = r.int()
	c.isRAM = r.bool()
	r.bytesInto(c.ram)
}
func (c *tigerCart) clone() cart {
	n := *c
	n.ram = slices.Clone(c.ram)
	return &n
}

// ------------------------------------------------------------------
// 3E+ (Thomas Jentzsch): four 1K segments. Writing to $3F selects a 1K ROM
// bank, to $3E a 512-byte RAM bank (read in the segment's first half, write
// in its second); bits 7-6 of the value pick the segment. At power-on every
// segment shows the first 1K of the ROM (which holds the reset vectors).
// ------------------------------------------------------------------
type tj3eCart struct {
	rom    []byte
	nBanks int
	ram    [32 * kb]uint8
	seg    [4]int // >= 0: ROM bank; < 0: -(RAM bank) - 1
}

func (c *tj3eCart) kind() string { return "3E+" }
func (c *tj3eCart) read(a int, _ *Atari) uint8 {
	off := a & 0xfff
	s := c.seg[off>>10]
	if s >= 0 {
		return at(c.rom, s*1024+off&0x3ff)
	}
	if off&0x200 != 0 {
		return 0
	}
	return c.ram[(-s-1)*512+off&0x1ff]
}
func (c *tj3eCart) write(a int, v uint8, _ *Atari) {
	off := a & 0xfff
	if s := c.seg[off>>10]; s < 0 && off&0x200 != 0 {
		c.ram[(-s-1)*512+off&0x1ff] = v
	}
}
func (c *tj3eCart) tiaWrite(a int, v uint8) {
	if a == 0x3f {
		c.seg[v>>6] = int(v&0x3f) % c.nBanks
	} else if a == 0x3e {
		c.seg[v>>6] = -int(v&0x3f) - 1
	}
}
func (c *tj3eCart) sram() []byte { return nil }
func (c *tj3eCart) save(w *stateW) {
	w.bytes(c.ram[:])
	w.ints(c.seg[:])
}
func (c *tj3eCart) load(r *stateR) {
	r.bytesInto(c.ram[:])
	r.intsInto(c.seg[:])
}
func (c *tj3eCart) clone() cart { n := *c; return &n }

// ------------------------------------------------------------------
// CommaVid (CV): 2K ROM at $1800, 1K RAM (read $1000-$13FF, write $1400-$17FF).
// ------------------------------------------------------------------
type cvCart struct {
	rom []byte
	ram [1024]uint8
}

func newCVCart(rom []byte) *cvCart {
	c := &cvCart{}
	if len(rom) == 4096 { // dumps with a RAM image in front
		copy(c.ram[:], rom[:1024])
		rom = rom[2048:]
	} else if len(rom) < 2048 {
		r := make([]byte, 2048)
		for i := range r {
			r[i] = rom[i%len(rom)]
		}
		rom = r
	}
	c.rom = rom
	return c
}
func (c *cvCart) kind() string { return "CV" }
func (c *cvCart) read(a int, _ *Atari) uint8 {
	off := a & 0xfff
	if off >= 0x800 {
		return at(c.rom, off&0x7ff)
	}
	return c.ram[off&0x3ff]
}
func (c *cvCart) write(a int, v uint8, _ *Atari) {
	if off := a & 0xfff; off >= 0x400 && off < 0x800 {
		c.ram[off&0x3ff] = v
	}
}
func (c *cvCart) sram() []byte   { return nil }
func (c *cvCart) save(w *stateW) { w.bytes(c.ram[:]) }
func (c *cvCart) load(r *stateR) { r.bytesInto(c.ram[:]) }
func (c *cvCart) clone() cart    { n := *c; return &n }

// ------------------------------------------------------------------
// Schemes switched by touching addresses outside the cartridge's own
// 4K: Econobanking (0840), UA, X07, SuperBanking (SB) and MDM.
// ------------------------------------------------------------------
type busCart struct {
	typ          string
	rom          []byte
	nBanks, bank int
	locked       bool
}

func newBusCart(rom []byte, typ string) *busCart {
	c := &busCart{typ: typ, rom: rom, nBanks: max(1, len(rom)>>12)}
	switch typ {
	case "SB":
		c.bank = c.nBanks - 1
	case "UASW":
		c.bank = 1
	}
	return c
}
func (c *busCart) kind() string               { return c.typ }
func (c *busCart) read(a int, _ *Atari) uint8 { return at(c.rom, c.bank<<12|a&0xfff) }
func (c *busCart) write(int, uint8, *Atari)   {}
func (c *busCart) access(a int) {
	switch c.typ {
	case "0840":
		if a&0x1840 == 0x0800 {
			c.bank = 0
		} else if a&0x1840 == 0x0840 {
			c.bank = 1
		}
	case "UA":
		if a&0x1260 == 0x0220 {
			c.bank = 0
		} else if a&0x1260 == 0x0240 {
			c.bank = 1
		}
	case "UASW":
		if a&0x1260 == 0x0220 {
			c.bank = 1
		} else if a&0x1260 == 0x0240 {
			c.bank = 0
		}
	case "SB":
		if a&0x1800 == 0x0800 {
			c.bank = (a & 0xff) % c.nBanks
		}
	case "MDM":
		if !c.locked && a&0x1c00 == 0x0800 {
			c.bank = (a & 0xff) % c.nBanks
			if a&0x80 != 0 {
				c.locked = true
			}
		}
	case "X07":
		if a&0x180f == 0x080d {
			c.bank = (a >> 4) & 15
		} else if a&0x1880 == 0 && c.bank&14 == 14 {
			c.bank = 14 | (a>>6)&1
		}
	}
}
func (c *busCart) sram() []byte { return nil }
func (c *busCart) save(w *stateW) {
	w.int(c.bank)
	w.bool(c.locked)
}
func (c *busCart) load(r *stateR) {
	c.bank = r.int()
	c.locked = r.bool()
}
func (c *busCart) clone() cart { n := *c; return &n }

// ------------------------------------------------------------------
// Pitfall II's DPC chip: 8 data fetchers, a random number generator and
// three music channels, plus F8-style bank switching.
// ------------------------------------------------------------------
type dpcCart struct {
	rom         []byte
	dd          []byte
	bank        int
	tops        [8]uint8
	bottoms     [8]uint8
	counters    [8]uint16
	flags       [8]uint8
	music       [3]bool
	rand        uint8
	musicCycles float64
}

func newDPCCart(rom []byte) *dpcCart {
	return &dpcCart{rom: rom, dd: rom[8192:10240], bank: 1, rand: 1}
}
func (c *dpcCart) kind() string { return "DPC" }
func (c *dpcCart) read(a int, m *Atari) uint8 {
	off := a & 0xfff
	if off < 0x40 {
		return c.dpcRead(off, m)
	}
	if off == 0xff8 || off == 0xff9 {
		c.bank = off - 0xff8
	}
	return c.rom[c.bank*4096+off]
}
func (c *dpcCart) write(a int, v uint8, _ *Atari) {
	off := a & 0xfff
	if off >= 0x40 && off < 0x80 {
		c.dpcWrite(off, v)
	} else if off == 0xff8 || off == 0xff9 {
		c.bank = off - 0xff8
	}
}

// dpcMusic: fetchers 5-7 in music mode count their low byte down at 20 kHz
// (from their top value to 0, then round again). The flag is set while the
// count is above the bottom value: a square wave.
func (c *dpcCart) dpcMusic(m *Atari) {
	const cpuHz = float64(clock) / 3
	ticks := int64(math.Floor((float64(m.cycles) - c.musicCycles) * 20000 / cpuHz))
	if ticks <= 0 {
		return
	}
	c.musicCycles += float64(ticks) * cpuHz / 20000
	for ch := 0; ch < 3; ch++ {
		f := 5 + ch
		top := int64(c.tops[f])
		if !c.music[ch] {
			continue
		}
		count := int64(0)
		if top != 0 {
			count = int64(c.counters[f]&0xff) - ticks%(top+1)
			if count < 0 {
				count += top + 1
			}
		}
		if count <= int64(c.bottoms[f]) {
			c.flags[f] = 0
		} else if count <= top {
			c.flags[f] = 0xff
		}
		c.counters[f] = c.counters[f]&0x700 | uint16(count)
	}
}

func (c *dpcCart) dpcRead(off int, m *Atari) uint8 {
	r := c.rand
	c.rand = r<<1 | ^((r>>7)^(r>>5)^(r>>4)^(r>>3))&1
	i, fn := off&7, (off>>3)&7
	if fn == 0 && i >= 4 {
		c.dpcMusic(m)
		// the three square waves, mixed with weights 4, 5 and 6 into a 4-bit level
		level := uint8(0)
		for ch := 0; ch < 3; ch++ {
			if c.music[ch] && c.flags[5+ch] != 0 {
				level += uint8(4 + ch)
			}
		}
		return level
	}
	if uint8(c.counters[i]) == c.tops[i] {
		c.flags[i] = 0xff
	} else if uint8(c.counters[i]) == c.bottoms[i] {
		c.flags[i] = 0
	}
	var res uint8
	switch fn {
	case 0:
		res = r
	case 1:
		res = c.dd[2047-int(c.counters[i])]
	case 2:
		res = c.dd[2047-int(c.counters[i])] & c.flags[i]
	case 7:
		res = c.flags[i]
	}
	if fn != 0 && (i < 5 || !c.music[i-5]) {
		c.counters[i] = (c.counters[i] - 1) & 0x7ff
	}
	return res
}

func (c *dpcCart) dpcWrite(off int, v uint8) {
	i, fn := off&7, (off>>3)&7
	switch fn {
	case 0:
		c.tops[i] = v
		c.flags[i] = 0
	case 1:
		c.bottoms[i] = v
	case 2:
		c.counters[i] = c.counters[i]&0x700 | uint16(v)
	case 3:
		c.counters[i] = c.counters[i]&0xff | uint16(v&7)<<8
		if i >= 5 {
			c.music[i-5] = v&0x10 != 0
		}
	case 6:
		c.rand = 1
	}
}
func (c *dpcCart) sram() []byte { return nil }
func (c *dpcCart) save(w *stateW) {
	w.int(c.bank)
	w.bytes(c.tops[:])
	w.bytes(c.bottoms[:])
	w.u16s(c.counters[:])
	w.bytes(c.flags[:])
	w.bools(c.music[:])
	w.u8(c.rand)
	w.f64(c.musicCycles)
}
func (c *dpcCart) load(r *stateR) {
	c.bank = r.int()
	r.bytesInto(c.tops[:])
	r.bytesInto(c.bottoms[:])
	r.u16sInto(c.counters[:])
	r.bytesInto(c.flags[:])
	r.boolsInto(c.music[:])
	c.rand = r.u8()
	c.musicCycles = r.f64()
}
func (c *dpcCart) clone() cart { n := *c; return &n }

// ------------------------------------------------------------------
// Starpath Supercharger (AR): 6K of RAM in three 2K banks plus a 2K BIOS
// ROM, normally loaded from cassette. The game file holds the tape's
// loads (8K of data + a 256-byte header each). RetroPixels doesn't use the
// BIOS: when the game reaches the BIOS's load entry ($F800), the load is
// copied straight into RAM and the game started, as the BIOS would.
//
// Writing to the Supercharger: touching $F000+n latches n; the fifth
// address change after that writes n to the RAM address on the bus (if
// writing is on), or into the control register if that address is $FFF8.
// ------------------------------------------------------------------

// Control register bits 4-2: which bank is at $F000 / $F800 (-1 = ROM).
var arMap = [8][2]int{{2, -1}, {0, -1}, {2, 0}, {0, 2}, {2, -1}, {1, -1}, {2, 1}, {1, 2}}

type arCart struct {
	loads        [][]byte
	ram          [6144]uint8
	bios         [2048]uint8
	latch, count int
	lastA        int
	inject       []uint8
	booted       bool
	loadNo       int
	config       uint8
	lo, hi       int
	writeOn      bool
}

func newARCart(rom []byte) (*arCart, error) {
	c := &arCart{latch: -1, lastA: -1, loadNo: -1}
	for o := 0; o+8448 <= len(rom); o += 8448 {
		c.loads = append(c.loads, rom[o:o+8448])
	}
	if len(c.loads) == 0 {
		return nil, errors.New("This Supercharger file is too short: it should hold one or more loads of 8448 bytes.")
	}
	for i := range c.ram {
		c.ram[i] = uint8(i*29 + 3)
	}
	// RetroPixels' stand-in for the BIOS: NOPs, with the reset vectors pointing at the load entry.
	for i := range c.bios {
		c.bios[i] = 0xea
	}
	c.bios[0x7fc], c.bios[0x7fd], c.bios[0x7fe], c.bios[0x7ff] = 0x00, 0xf8, 0x00, 0xf8
	c.setConfig(0)
	return c, nil
}
func (c *arCart) kind() string { return "AR" }
func (c *arCart) setConfig(v uint8) {
	c.config = v
	m := arMap[(v>>2)&7]
	c.lo, c.hi = m[0], m[1]
	c.writeOn = v&2 != 0
}
func (c *arCart) access(a int) {
	if c.latch >= 0 && a != c.lastA {
		c.count++
		if c.count == 5 {
			if a&0x1000 != 0 {
				if a&0xfff == 0xff8 {
					c.setConfig(uint8(c.latch))
				} else if c.writeOn {
					b := c.lo
					if a&0x800 != 0 {
						b = c.hi
					}
					if b >= 0 {
						c.ram[b*2048+a&0x7ff] = uint8(c.latch)
					}
				}
			}
			c.latch = -1
		}
	}
	c.lastA = a
	if a&0x1f00 == 0x1000 {
		c.latch = a & 0xff
		c.count = 0
	}
}
func (c *arCart) read(a int, m *Atari) uint8 {
	if len(c.inject) > 0 {
		v := c.inject[0]
		c.inject = c.inject[1:]
		return v
	}
	off := a & 0xfff
	b := c.lo
	if off&0x800 != 0 {
		b = c.hi
	}
	if b < 0 {
		if off == 0x800 && m != nil && m.isOp {
			return c.loadGame(m)
		}
		return c.bios[off&0x7ff]
	}
	return c.ram[b*2048+off&0x7ff]
}
func (c *arCart) write(int, uint8, *Atari) {}

// loadGame is the BIOS's load entry: copy the wanted load into RAM and jump to its start.
func (c *arCart) loadGame(m *Atari) uint8 {
	want := -1
	if c.booted {
		want = int(m.riot.ram[0x7a]) // the game puts the load number it wants at $FA
	}
	l := c.loads[0]
	if want >= 0 {
		for _, x := range c.loads {
			if int(x[8192+5]) == want {
				l = x
				break
			}
		}
	}
	c.booted = true
	h := l[8192:]
	pages := int(h[3])
	if pages == 0 || pages > 32 {
		pages = 32
	}
	for i := 0; i < pages; i++ {
		dest := h[0x10+i]
		bank, page := int(dest&3), int(dest>>2)&7
		if bank < 3 {
			copy(c.ram[bank*2048+page*256:bank*2048+page*256+256], l[i*256:i*256+256])
		}
	}
	c.loadNo = int(h[5])
	c.setConfig(h[2])
	c.latch = -1
	// The BIOS leaves the RIOT timer and the zero-page load number as they were; it finishes with a JMP.
	c.inject = []uint8{h[0], h[1]}
	return 0x4c
}
func (c *arCart) sram() []byte { return nil }
func (c *arCart) save(w *stateW) {
	w.bytes(c.ram[:])
	w.int(c.latch)
	w.int(c.count)
	w.int(c.lastA)
	w.bytes(c.inject)
	w.bool(c.booted)
	w.int(c.loadNo)
	w.u8(c.config)
}
func (c *arCart) load(r *stateR) {
	r.bytesInto(c.ram[:])
	c.latch = r.int()
	c.count = r.int()
	c.lastA = r.int()
	c.inject = r.bytes()
	c.booted = r.bool()
	c.loadNo = r.int()
	c.setConfig(r.u8())
}
func (c *arCart) clone() cart {
	n := *c
	n.inject = slices.Clone(c.inject)
	return &n
}

// ------------------------------------------------------------------
// Creating a cartridge
// ------------------------------------------------------------------
func createCart(rom []byte, name string, sram []byte) (cart, error) {
	typ, err := detectType(rom, name)
	if err != nil {
		return nil, err
	}
	if msg, ok := unsupported[typ]; ok {
		return nil, errors.New(msg)
	}
	if typ == "DPC+" {
		return newDPCPlusCart(rom), nil
	}
	if _, ok := fx[strings.TrimSuffix(typ, "SC")]; ok {
		return newHotspotCart(rom, typ, sram), nil
	}
	switch typ {
	case "2K", "4K":
		r := make([]byte, 4096)
		n := min(len(rom), 4096)
		for i := range r {
			r[i] = rom[i%n]
		}
		return &plainCart{typ: typ, rom: r}, nil
	case "CV":
		return newCVCart(rom), nil
	case "F0":
		return &f0Cart{rom: rom, nBanks: len(rom) >> 12, bank: len(rom)>>12 - 1}, nil
	case "E0":
		return &e0Cart{rom: rom, slices: [4]int{4, 5, 6, 7}}, nil
	case "E7":
		return newE7Cart(rom), nil
	case "FE":
		return &feCart{rom: rom}, nil
	case "3F", "3E":
		return newTigerCart(rom, typ), nil
	case "3E+":
		return &tj3eCart{rom: rom, nBanks: max(1, len(rom)>>10)}, nil
	case "0840", "UA", "UASW", "X07", "SB", "MDM":
		return newBusCart(rom, typ), nil
	case "DPC":
		if len(rom) < 10240 {
			return nil, errors.New("This DPC (Pitfall II) file is too short.")
		}
		return newDPCCart(rom), nil
	case "AR":
		return newARCart(rom)
	}
	return nil, fmt.Errorf("This Atari 2600 game uses the %s cartridge, which RetroPixels doesn't support yet.", typ)
}
