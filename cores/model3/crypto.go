package model3

// The security board's Sega 315-5881 decryption chip: the game puts
// encrypted (and sometimes compressed) data in the security RAM and reads it
// back decrypted through register 0x1C. This follows MAME's emulation
// (BSD-3-Clause, Andreas Naive, Olivier Galibert, David Haywood) as used by
// Supermodel.

// sbox: the table (64 entries), the 6 input bit positions (-1: none) and
// the 2 output bit positions
type sbox = [3][]int

const (
	cryptBufSize    = 2
	cryptLineSize   = 512
	cryptCompressed = 0x20000
)

type crypto struct {
	key  uint32
	read func(uint32) uint16

	buffer         [cryptBufSize]uint8
	line, linePrev []uint8
	addr           uint32
	subkey, hist   uint16
	header         uint32
	ready          bool

	bufPos, linePos, lineSize, bit, bit2 int
	buffer2                              [2]uint8
	buffer2a                             uint16
	blockSize, blockPos, blockLines      int
	doneCompression                      bool
}

func newCrypto(key uint32, read func(uint32) uint16) *crypto {
	c := &crypto{key: key, read: read}
	c.reset()
	return c
}

func (c *crypto) reset() {
	c.buffer = [cryptBufSize]uint8{}
	c.line = make([]uint8, cryptLineSize)
	c.linePrev = make([]uint8, cryptLineSize)
	c.addr, c.subkey, c.hist, c.header = 0, 0, 0, 0
	c.ready = false
	c.bufPos, c.linePos, c.lineSize, c.bit = 0, 0, 0, 0
}

func bit(x uint32, n int) uint32 { return x >> uint(n) & 1 }

func bitswap16(v uint32, b ...int) uint32 {
	var r uint32
	for i, n := range b {
		r |= bit(v, n) << uint(15-i)
	}
	return r
}

func feistel(input uint32, boxes *[4]sbox, subkeys uint32) uint32 {
	var result uint32
	for m := 0; m < 4; m++ {
		var aux uint32
		for k := 0; k < 6; k++ {
			if boxes[m][1][k] != -1 {
				aux |= bit(input, boxes[m][1][k]) << uint(k)
			}
		}
		aux = uint32(boxes[m][0][(aux^subkeys)&0x3F])
		for k := 0; k < 2; k++ {
			result |= bit(aux, k) << uint(boxes[m][2][k])
		}
		subkeys >>= 6
	}
	return result
}

func blockDecrypt(gameKey uint32, seqKey, counter, data uint16) uint16 {
	var k1, k2 [4]uint32
	for _, s := range fn1GameKeySched {
		if bit(gameKey, s[0]) != 0 {
			k1[s[1]/24] ^= 1 << uint(s[1]%24)
		}
	}
	for _, s := range fn2GameKeySched {
		if bit(gameKey, s[0]) != 0 {
			k2[s[1]/24] ^= 1 << uint(s[1]%24)
		}
	}
	for _, s := range fn1SeqKeySched {
		if bit(uint32(seqKey), s[0]) != 0 {
			k1[s[1]/24] ^= 1 << uint(s[1]%24)
		}
	}
	for j, s := range fn2SeqKeySched {
		if bit(uint32(seqKey), j) != 0 {
			k2[s/24] ^= 1 << uint(s%24)
		}
	}
	aux := bitswap16(uint32(counter), 5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2)
	b := aux >> 8
	a := aux&0xFF ^ feistel(b, &fn1Sboxes[0], k1[0])
	b ^= feistel(a, &fn1Sboxes[1], k1[1])
	a ^= feistel(b, &fn1Sboxes[2], k1[2])
	b ^= feistel(a, &fn1Sboxes[3], k1[3])
	middle := b<<8 | a
	for j, s := range fn2MiddleSched {
		if bit(middle, j) != 0 {
			k2[s/24] ^= 1 << uint(s%24)
		}
	}
	aux = bitswap16(uint32(data), 14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10)
	b = aux >> 8
	a = aux&0xFF ^ feistel(b, &fn2Sboxes[0], k2[0])
	b ^= feistel(a, &fn2Sboxes[1], k2[1])
	a ^= feistel(b, &fn2Sboxes[2], k2[2])
	b ^= feistel(a, &fn2Sboxes[3], k2[3])
	aux = b<<8 | a
	return uint16(bitswap16(aux, 15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8))
}

func (c *crypto) decrypted16() uint16 {
	enc := c.read(c.addr)
	dec := blockDecrypt(c.key, c.subkey, uint16(c.addr), enc)
	res := dec&3 | c.hist&0xFFFC
	c.hist = dec
	c.addr++
	return res
}

func (c *crypto) start() {
	c.blockPos = 0
	c.doneCompression = false
	c.bufPos = cryptBufSize
	if c.bit2 != 15 {
		c.header = uint32(c.buffer2a&3) << 16
	} else {
		c.hist = 0
		c.header = uint32(c.decrypted16()) << 16
	}
	c.header |= uint32(c.decrypted16())
	c.blockLines = int(c.header&0xFF) + 1
	blocky := int(c.header&0x1FF00>>8) + 1
	c.blockSize = c.blockLines * blocky
	if c.header&cryptCompressed != 0 {
		c.lineSize = blocky
		c.linePos = c.lineSize
		c.bit, c.bit2 = 7, 15
	}
	c.ready = true
}

func (c *crypto) fill() {
	for i := 0; i != cryptBufSize; i += 2 {
		v := c.decrypted16()
		c.buffer[i] = uint8(v)
		c.buffer[i+1] = uint8(v >> 8)
		c.blockPos += 2
		if c.header&cryptCompressed == 0 && c.blockPos == c.blockSize {
			c.start()
		}
	}
	c.bufPos = 0
}

func (c *crypto) compressedBit() int {
	if c.bit2 == 15 {
		c.bit2 = 0
		c.buffer2a = c.decrypted16()
		c.buffer2[0] = uint8(c.buffer2a)
		c.buffer2[1] = uint8(c.buffer2a >> 8)
		c.bufPos = 0
	} else {
		c.bit2++
	}
	res := int(c.buffer2[(c.bufPos&1)^1]>>uint(c.bit)) & 1
	c.bit--
	if c.bit == -1 {
		c.bit = 7
		c.bufPos++
	}
	return res
}

func (c *crypto) lineFill() {
	lp, lc := c.line, c.linePrev
	c.line, c.linePrev = c.linePrev, c.line
	c.linePos = 0
	n := c.lineSize
	for i := 0; i != n; {
		slot := 0
		if i != 0 {
			if i < n-7 {
				slot = 1
			} else {
				slot = i&7 + 1
			}
		}
		tmp := uint8(0)
		for tmp&0x80 == 0 {
			tmp = cryptoTrees[slot][c.compressedBit()][tmp]
		}
		if tmp == 0xFF {
			continue
		}
		count := int(tmp&7) + 1
		if tmp&0x40 != 0 {
			offset := [4]int{0, 1, 0, -1}[(tmp&0x18)>>3]
			for j := 0; j != count; j++ {
				lc[(i^1)%cryptLineSize] = lp[(((i+offset)%n+n)%n^1)%cryptLineSize]
				i++
			}
		} else {
			var b uint8
			for k := 0; k < 8; k++ {
				b = b<<1 | uint8(c.compressedBit())
			}
			for j := 0; j != count; j++ {
				lc[(i^1)%cryptLineSize] = b
				i++
			}
		}
	}
	c.blockPos++
	if c.blockLines == c.blockPos {
		c.doneCompression = true
	}
}

func (c *crypto) decrypt() uint16 {
	if c.key == 0 {
		return 0xFFFF
	}
	if !c.ready {
		c.start()
	}
	var b0, b1 uint8
	if c.header&cryptCompressed != 0 {
		if c.linePos == c.lineSize {
			if c.doneCompression {
				c.start()
			}
			c.lineFill()
		}
		b0, b1 = c.line[c.linePos%cryptLineSize], c.line[(c.linePos+1)%cryptLineSize]
		c.linePos += 2
	} else {
		if c.bufPos == cryptBufSize {
			c.fill()
		}
		b0, b1 = c.buffer[c.bufPos], c.buffer[c.bufPos+1]
		c.bufPos += 2
	}
	return uint16(b0)<<8 | uint16(b1)
}

func (c *crypto) setAddressLow(v uint16) {
	c.addr = c.addr&0xFFFF0000 | uint32(v)
	c.ready = false
}

func (c *crypto) setAddressHigh(v uint16) {
	c.addr = c.addr&0xFFFF | uint32(v)<<16
	c.ready = false
	c.bit, c.bit2 = 7, 15
}

func (c *crypto) setSubKey(v uint16) {
	c.subkey = v
	c.ready = false
}
