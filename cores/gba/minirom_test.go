package gba

import "encoding/binary"

// miniROM builds a small ARM program (no tools needed) for the tests that
// must always run: each frame it waits for V-blank, reads the buttons, draws
// a line of the mode 3 picture in colours that depend on them, restarts the
// square-wave channel with a pitch from the frame count every 16 frames and
// writes the frame count to the battery save every 32.
func miniROM() []byte {
	const al = 0xe0000000
	var code []uint32
	at := func() int { return len(code) }
	emit := func(w uint32) { code = append(code, w) }
	imm := func(v uint32) uint32 { // an ARM rotated immediate
		for r := uint32(0); r < 16; r++ {
			x := v<<(2*r) | v>>(32-2*r)
			if r == 0 {
				x = v
			}
			if x < 256 {
				return 1<<25 | r<<8 | x
			}
		}
		panic("immediate")
	}
	dp := func(op, s, rn, rd, op2 uint32) { emit(al | op<<21 | s<<20 | rn<<16 | rd<<12 | op2) }
	const (
		and, eor, sub, add, cmp, orr, mov = 0, 1, 2, 4, 10, 12, 13
	)
	movi := func(rd, v uint32) { dp(mov, 0, 0, rd, imm(v)) }
	reg := func(rm, shiftType, amount uint32) uint32 { return rm | shiftType<<5 | amount<<7 }
	strh := func(rd, rn, off uint32) { emit(al | 0x01c000b0 | rn<<16 | rd<<12 | off>>4<<8 | off&15) }
	ldrh := func(rd, rn, off uint32) { emit(al | 0x01d000b0 | rn<<16 | rd<<12 | off>>4<<8 | off&15) }
	strhPost2 := func(rd, rn uint32) { emit(al | 0x00c000b2 | rn<<16 | rd<<12) }
	strbReg := func(rd, rn, rm uint32) { emit(al | 0x07c00000 | rn<<16 | rd<<12 | rm) }
	branch := func(cond uint32, to int) { emit(cond<<28 | 0x0a000000 | uint32(to-at()-2)&0xffffff) }
	const eq, ne, al4 = 0, 1, 14

	movi(0, 0x04000000) // r0: I/O
	movi(1, 0x400)
	dp(orr, 0, 1, 1, imm(3))
	strh(1, 0, 0) // DISPCNT: mode 3, BG2
	movi(1, 0x80)
	strh(1, 0, 0x84) // sound on
	movi(1, 0x1100)
	dp(orr, 0, 1, 1, imm(0x77))
	strh(1, 0, 0x80) // PSG volume, channel 1 left and right
	movi(1, 2)
	strh(1, 0, 0x82)
	movi(1, 0xf000)
	dp(orr, 0, 1, 1, imm(0x80))
	strh(1, 0, 0x62)    // envelope 15, duty 50%
	movi(9, 0x0e000000) // r9: save memory
	movi(4, 0)          // r4: frame count
	loop := at()        //
	ldrh(1, 0, 6)       // wait for line 160
	dp(cmp, 1, 1, 0, imm(160))
	branch(ne, loop)
	ldrh(2, 0, 0x130) // r2: buttons
	dp(add, 0, 4, 4, imm(1))
	dp(and, 0, 4, 3, imm(127)) // r3: line = count & 127
	dp(mov, 0, 0, 5, reg(3, 0, 9))
	dp(sub, 0, 5, 5, reg(3, 0, 5)) // r5 = line*480
	dp(add, 0, 5, 5, imm(0x06000000))
	dp(eor, 0, 2, 6, reg(4, 0, 5)) // r6: colour = buttons ^ count<<5
	movi(7, 240)
	px := at()
	strhPost2(6, 5)
	dp(add, 0, 6, 6, imm(33))
	dp(sub, 1, 7, 7, imm(1))
	branch(ne, px)
	dp(and, 1, 4, 3, imm(15)) // every 16 frames: a note
	branch(ne, at()+5)
	dp(and, 0, 4, 1, imm(0x3f0))
	dp(orr, 0, 1, 1, imm(0x400))
	dp(orr, 0, 1, 1, imm(0x8000))
	strh(1, 0, 0x64)
	dp(and, 1, 4, 3, imm(31)) // every 32 frames: the save
	branch(ne, at()+3)
	dp(mov, 0, 0, 3, reg(4, 1, 5))
	strbReg(4, 9, 3)
	wait := at() // wait for line 160 to pass
	ldrh(1, 0, 6)
	dp(cmp, 1, 1, 0, imm(160))
	branch(eq, wait)
	branch(al4, loop)

	rom := make([]byte, 0x200+4*len(code))
	binary.LittleEndian.PutUint32(rom, 0xea00007e) // b 0x08000200
	copy(rom[0xa0:], "RPMINI      RPMN01")
	rom[0xb2] = 0x96
	copy(rom[0x100:], "SRAM_V113")
	for i, w := range code {
		binary.LittleEndian.PutUint32(rom[0x200+4*i:], w)
	}
	return rom
}
