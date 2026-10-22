package d3d

import (
	"encoding/binary"
	"math/bits"
)

// Direct3D refuses shader bytecode unless the container header carries the
// right checksum. It is MD5 run over everything after the checksum field,
// but with Direct3D's own padding: the bit count goes in the first word of
// the final block and (bits>>2)|1 in its last word.

var md5s = [64]uint32{7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21}

var md5k = md5table

var md5table = [64]uint32{
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391}

func md5block(s *[4]uint32, p []byte) {
	var m [16]uint32
	for i := range m {
		m[i] = binary.LittleEndian.Uint32(p[i*4:])
	}
	a, b, c, d := s[0], s[1], s[2], s[3]
	for i := 0; i < 64; i++ {
		var f uint32
		var g int
		switch i / 16 {
		case 0:
			f, g = (b&c)|(^b&d), i
		case 1:
			f, g = (d&b)|(^d&c), (5*i+1)&15
		case 2:
			f, g = b^c^d, (3*i+5)&15
		default:
			f, g = c^(b|^d), (7*i)&15
		}
		f += a + md5k[i] + m[g]
		a, d, c = d, c, b
		b += bits.RotateLeft32(f, int(md5s[i]))
	}
	s[0] += a
	s[1] += b
	s[2] += c
	s[3] += d
}

// Checksum computes the container checksum of a whole DXBC blob.
func Checksum(blob []byte) [4]uint32 {
	data := blob[20:]
	s := [4]uint32{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}
	n := len(data)
	full := n &^ 63
	for i := 0; i < full; i += 64 {
		md5block(&s, data[i:i+64])
	}
	nbits := uint32(n * 8)
	tail := data[full:]
	var blk [64]byte
	if len(tail) >= 56 {
		copy(blk[:], tail)
		blk[len(tail)] = 0x80
		md5block(&s, blk[:])
		blk = [64]byte{}
		binary.LittleEndian.PutUint32(blk[0:], nbits)
	} else {
		binary.LittleEndian.PutUint32(blk[0:], nbits)
		copy(blk[4:], tail)
		blk[4+len(tail)] = 0x80
	}
	binary.LittleEndian.PutUint32(blk[60:], nbits>>2|1)
	md5block(&s, blk[:])
	return s
}
