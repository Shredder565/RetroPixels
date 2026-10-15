package boards

// "Poker" test programs for generated sets: each processor runs a loop that
// writes a long table of pseudo-random values to pseudo-random addresses of
// the board's chips (tilemaps and their registers, palette, sprites, the
// mixer, the sound chips...), so the video and sound chips draw and play
// all sorts of things that both cores must agree on.

import (
	"encoding/binary"

	"retropixels/cores/arcade"
)

type rng struct{ s uint64 }

func (r *rng) next() uint64 {
	r.s ^= r.s << 13
	r.s ^= r.s >> 7
	r.s ^= r.s << 17
	return r.s
}
func (r *rng) n(k int) int { return int(r.next() % uint64(k)) }

// span is an address range the pokers write to, with a weight.
type span struct {
	lo, hi int // [lo, hi)
	weight int
}

func pickAddr(r *rng, spans []span, even bool) int {
	total := 0
	for _, s := range spans {
		total += s.weight
	}
	k := r.n(total)
	for _, s := range spans {
		if k < s.weight {
			a := s.lo + r.n(s.hi-s.lo)
			if even {
				a &^= 1
			}
			return a
		}
		k -= s.weight
	}
	return spans[0].lo
}

// m68kPoker: a 68000 program at 0x400 looping over a table at 0x420 of
// (address.l, value.w) entries, with the vectors set up for it.
func m68kPoker(rom []byte, sp uint32, spans []span, r *rng) {
	be := binary.BigEndian
	be.PutUint32(rom[0:], sp)
	be.PutUint32(rom[4:], 0x400)
	n := (len(rom) - 0x420) / 6
	if n > 0x8000 {
		n = 0x8000
	}
	prog := []uint16{
		0x41fa, uint16(0x420 - 0x402), // lea table(pc),a0
		0x3e3c, uint16(n - 1), // move.w #n-1,d7
		0x2258,         // inner: movea.l (a0)+,a1
		0x3298,         // move.w (a0)+,(a1)
		0x51cf, 0xfffa, // dbra d7,inner
		0x6000, 0xffee, // bra start
	}
	for i, w := range prog {
		be.PutUint16(rom[0x400+i*2:], w)
	}
	for i := 0; i < n; i++ {
		o := 0x420 + i*6
		be.PutUint32(rom[o:], uint32(pickAddr(r, spans, true)))
		be.PutUint16(rom[o+4:], uint16(r.next()))
	}
}

// z80Poker: a Z80 program at 0 looping over a table at 0x20 of (address,
// value) entries.
func z80Poker(rom []byte, size int, spans []span, ymAt int, r *rng) {
	n := (size - 0x20) / 3
	if n > 0x2000 {
		n = 0x2000
	}
	copy(rom, []byte{
		0x31, 0x00, 0x00, // ld sp,0 (unused)
		0x21, 0x20, 0x00, // start: ld hl,table
		0x01, byte(n), byte(n >> 8), // ld bc,n
		0x5e, 0x23, 0x56, 0x23, 0x7e, 0x23, // inner: ld e,(hl) / inc hl / ld d,(hl) / inc hl / ld a,(hl) / inc hl
		0x12,       // ld (de),a
		0x0b,       // dec bc
		0x78, 0xb1, // ld a,b / or c
		0x20, 0xf4, // jr nz,inner
		0x18, 0xec, // jr start
	})
	for i := 0; i < n; i++ {
		o := 0x20 + i*3
		var a int
		if ymAt >= 0 && r.n(3) == 0 { // the YM2151: an address write, then data
			if i%2 == 0 {
				a = ymAt
			} else {
				a = ymAt + 1
			}
		} else {
			a = pickAddr(r, spans, false)
		}
		rom[o], rom[o+1], rom[o+2] = byte(a), byte(a>>8), byte(r.next())
		if a == ymAt && r.n(2) == 0 {
			rom[o+2] = byte(0x08 + r.n(0xf8)) // registers that do something
		}
	}
}

func pokeSet(set map[string][]byte, g *arcade.Game, board string, seed uint64) {
	r := &rng{s: seed*0x9e3779b97f4a7c15 | 1}
	file := func(role string, lane, offset int) []byte {
		for _, f := range g.Files {
			if f.Role == role && f.Lane == lane && (offset < 0 || f.Offset == offset) {
				return set[f.Name]
			}
		}
		return nil
	}
	switch board {
	case "tmnt":
		prog := make([]byte, 0x40000)
		m68kPoker(prog, 0x064000, []span{
			{0x100000, 0x108000, 40}, // K052109 (tilemaps, scroll, banks)
			{0x080000, 0x081000, 20}, // palette
			{0x140400, 0x140800, 25}, // sprites
			{0x140000, 0x140008, 2},  // K051937
			{0x0c0000, 0x0c0002, 1},  // layer priority
			{0x0a0008, 0x0a000a, 3},  // sound command
			{0x0a0000, 0x0a0002, 2},  // control (sound interrupt, ROM reading)
		}, r)
		even, odd := file("cpu_even", 0, -1), file("cpu_odd", 0, -1)
		for i := range even {
			even[i], odd[i] = prog[i*2], prog[i*2+1]
		}
		z80Poker(file("audio", 0, -1), 0x8000, []span{
			{0xb000, 0xb00e, 30}, {0x9000, 0x9001, 3}, {0xd000, 0xd001, 3}, {0xe000, 0xe001, 3}, {0x8000, 0x8800, 2},
		}, 0xc000, r)
	case "tmnt2":
		prog := make([]byte, 0x40000)
		m68kPoker(prog, 0x108000, []span{
			{0x600000, 0x604000, 40}, // K052109
			{0x140000, 0x141000, 20}, // palette
			{0x180000, 0x184000, 30}, // sprite RAM
			{0x5a0000, 0x5a0020, 3},  // K053244
			{0x5c0700, 0x5c0720, 4},  // K053251
			{0x1c0800, 0x1c0820, 3},  // sprite helper chip
			{0x5c0600, 0x5c0606, 3},  // K053260 ports, sound interrupt
			{0x1c0300, 0x1c0302, 1},  // control
			{0x1c0200, 0x1c0202, 1},  // EEPROM, darkening
		}, r)
		lo, hi := file("cpu", 0, 0), file("cpu", 1, 0)
		for i := range lo {
			lo[i], hi[i] = prog[i*2], prog[i*2+1]
		}
		z80Poker(file("sound", 0, -1), 0xf000, []span{{0xfa00, 0xfa30, 30}, {0xf000, 0xf800, 2}}, 0xf800, r)
	case "simpsons":
		// the last program ROM's top 32 KB is the fixed 8000-ffff
		var last []byte
		for _, f := range g.Files {
			if f.Role == "cpu" {
				last = set[f.Name]
			}
		}
		fixed := last[len(last)-0x8000:]
		spans := []span{
			{0x0000, 0x1000, 25}, // palette or K052109
			{0x1000, 0x1f80, 25}, // K052109 and its registers
			{0x2000, 0x4000, 30}, // sprite RAM or K052109
			{0x1fa0, 0x1fa8, 3},  // K053246
			{0x1fb0, 0x1fc0, 4},  // K053251
			{0x1fc2, 0x1fc3, 3},  // EEPROM lines, video banks
			{0x1fc6, 0x1fc8, 3},  // K053260 ports
			{0x1fc4, 0x1fc5, 1},  // (a read interrupts the Z80; a write does nothing)
			{0x4000, 0x5e00, 2},  // work RAM
		}
		const table = 0x8100
		n := (0xfff0 - table) / 3
		copy(fixed, []byte{
			0x48, 0x5f, 0xf0, // lds #$5ff0
			0x42, table >> 8, table & 0xff, // loop: ldx #table
			0x45, 0x21, // inner: ldy ,x++
			0x12, 0x20, // lda ,x+
			0x3a, 0x36, // sta ,y
			0x4c, byte((table + n*3) >> 8), byte(table + n*3), // cmpx #end
			0x63, 0xf5, // bne inner
			0x60, 0xf0, // bra loop
		})
		for i := 0; i < n; i++ {
			a := pickAddr(r, spans, false)
			o := table - 0x8000 + i*3
			fixed[o], fixed[o+1], fixed[o+2] = byte(a>>8), byte(a), byte(r.next())
		}
		fixed[0x7ffe], fixed[0x7fff] = 0x80, 0x00 // reset vector
		z80Poker(file("sound", 0, -1), 0x8000, []span{{0xfc00, 0xfc30, 30}, {0xf000, 0xf800, 2}, {0xfe00, 0xfe01, 1}}, 0xf800, r)
	}
}
