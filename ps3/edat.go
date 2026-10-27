package ps3

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/subtle"
	"errors"
	"io"
)

// cmac is AES-CMAC (RFC 4493).
func cmac(key, msg []byte) []byte {
	b, _ := aes.NewCipher(key)
	l := make([]byte, 16)
	b.Encrypt(l, l)
	dbl := func(in []byte) []byte {
		out := make([]byte, 16)
		c := in[0] >> 7
		for i := 0; i < 15; i++ {
			out[i] = in[i]<<1 | in[i+1]>>7
		}
		out[15] = in[15] << 1
		if c != 0 {
			out[15] ^= 0x87
		}
		return out
	}
	k1 := dbl(l)
	k2 := dbl(k1)
	n := (len(msg) + 15) / 16
	full := n > 0 && len(msg)%16 == 0
	if n == 0 {
		n = 1
	}
	x := make([]byte, 16)
	for i := 0; i < n-1; i++ {
		subtle.XORBytes(x, x, msg[i*16:i*16+16])
		b.Encrypt(x, x)
	}
	last := make([]byte, 16)
	if full {
		subtle.XORBytes(last, msg[(n-1)*16:], k1)
	} else {
		r := msg[(n-1)*16:]
		copy(last, r)
		last[len(r)] = 0x80
		subtle.XORBytes(last, last, k2)
	}
	subtle.XORBytes(x, x, last)
	b.Encrypt(x, x)
	return x
}

// NPD is the header every PSN-protected file starts with.
type NPD struct {
	Version, License, Type uint32
	ContentID              string
	digest, titleHash      []byte
	devHash                []byte
	raw                    []byte // the first 0x60 bytes
}

func parseNPD(b []byte) (*NPD, error) {
	if len(b) < 0x80 || be.Uint32(b) != 0x4E504400 {
		return nil, errors.New("not a PSN-protected file (NPD)")
	}
	n := &NPD{Version: be.Uint32(b[4:]), License: be.Uint32(b[8:]), Type: be.Uint32(b[12:])}
	id := b[0x10:0x40]
	for i, c := range id {
		if c == 0 {
			id = id[:i]
			break
		}
	}
	n.ContentID = string(id)
	n.digest = b[0x40:0x50]
	n.titleHash = b[0x50:0x60]
	n.devHash = b[0x60:0x70]
	n.raw = b[:0x60]
	return n, nil
}

// klicMatches reports whether klic is the key a free-licence file was made with.
func (n *NPD) klicMatches(klic []byte) bool {
	k := make([]byte, 16)
	subtle.XORBytes(k, klic, omacKey2)
	return subtle.ConstantTimeCompare(cmac(k, n.raw), n.devHash) == 1
}

// FindKlicensee looks through a decrypted program for the 16 bytes a
// free-licence file (EDAT or SPRX header) was made with. PSN games keep it
// in their own code and hand it to sceNpDrmIsAvailable.
func FindKlicensee(elf []byte, npdHeader []byte) ([]byte, bool) {
	n, err := parseNPD(npdHeader)
	if err != nil {
		return nil, false
	}
	if n.klicMatches(klicFree) {
		return klicFree, true
	}
	k := make([]byte, 16)
	for i := 0; i+16 <= len(elf); i += 4 {
		w := elf[i : i+16]
		// skip runs that are clearly not keys (zeros, code with many zero bytes)
		z := 0
		for _, c := range w {
			if c == 0 {
				z++
			}
		}
		if z > 3 {
			continue
		}
		subtle.XORBytes(k, w, omacKey2)
		if subtle.ConstantTimeCompare(cmac(k, n.raw), n.devHash) == 1 {
			return append([]byte(nil), w...), true
		}
	}
	return nil, false
}

// EDAT reads a PSN data file (.EDAT/.SDAT) as plain data.
type EDAT struct {
	r         io.ReaderAt
	npd       *NPD
	flags     uint32
	blockSize uint64
	Size      int64
	blocks    uint64
	key       []byte // the klicensee (or SDAT key)
}

const (
	edatCompressed   = 0x1
	edatFlag2        = 0x2
	edatEncryptedKey = 0x8
	edatFlag10       = 0x10
	edatFlag20       = 0x20
	edatSDAT         = 0x01000000
	edatDebug        = 0x80000000
)

// OpenEDAT opens a PSN data file with the game's klicensee.
func OpenEDAT(r io.ReaderAt, klic []byte) (*EDAT, error) {
	h := make([]byte, 0x90)
	if _, err := r.ReadAt(h, 0); err != nil {
		return nil, err
	}
	n, err := parseNPD(h)
	if err != nil {
		return nil, err
	}
	e := &EDAT{r: r, npd: n}
	e.flags = be.Uint32(h[0x80:])
	e.blockSize = uint64(be.Uint32(h[0x84:]))
	e.Size = int64(be.Uint64(h[0x88:]))
	if e.blockSize == 0 || e.blockSize > 1<<20 {
		return nil, errors.New("the data file is damaged")
	}
	e.blocks = (uint64(e.Size) + e.blockSize - 1) / e.blockSize
	switch {
	case e.flags&edatSDAT != 0:
		e.key = make([]byte, 16)
		subtle.XORBytes(e.key, n.devHash, sdatKey)
	case e.flags&edatDebug != 0:
	default:
		if klic == nil {
			klic = klicFree
		}
		if n.License&3 == 3 && !n.klicMatches(klic) {
			return nil, errors.New("this data file needs the game's own key")
		}
		if n.License&3 != 3 {
			return nil, errors.New("this data file needs its licence (.rap)")
		}
		e.key = klic
	}
	if e.flags&edatCompressed != 0 {
		return nil, errors.New("compressed data files aren't supported yet")
	}
	return e, nil
}

// block decrypts block i into out (len >= blockSize) and returns its length.
func (e *EDAT) block(i uint64, out []byte) (int, error) {
	metaSize := uint64(0x10)
	if e.flags&(edatCompressed|edatFlag20) != 0 {
		metaSize = 0x20
	}
	var off uint64
	length := e.blockSize
	if i == e.blocks-1 && uint64(e.Size)%e.blockSize != 0 {
		length = uint64(e.Size) % e.blockSize
	}
	if e.flags&edatFlag20 != 0 {
		off = 0x100 + i*(metaSize+e.blockSize) + 0x20
	} else {
		off = 0x100 + i*e.blockSize + e.blocks*metaSize
	}
	padded := (length + 15) &^ 15
	buf := out[:padded]
	if _, err := e.r.ReadAt(buf, int64(off)); err != nil && !(err == io.EOF) {
		return 0, err
	}
	if e.flags&edatDebug != 0 {
		return int(length), nil
	}
	// the block's key: the key encrypts (dev hash[:12], block number)
	bk := make([]byte, 16)
	if e.npd.Version > 1 {
		copy(bk, e.npd.devHash[:12])
	}
	be.PutUint32(bk[12:], uint32(i))
	kb, _ := aes.NewCipher(e.key)
	kr := make([]byte, 16)
	kb.Encrypt(kr, bk)
	if e.flags&edatFlag2 != 0 {
		return int(length), nil // no encryption
	}
	ver4 := e.npd.Version == 4
	final := kr
	if e.flags&edatEncryptedKey != 0 {
		ek := edatKey0
		if ver4 {
			ek = edatKey1
		}
		eb, _ := aes.NewCipher(ek)
		final = make([]byte, 16)
		cipher.NewCBCDecrypter(eb, make([]byte, 16)).CryptBlocks(final, kr)
	}
	iv := make([]byte, 16)
	if e.npd.Version > 1 {
		copy(iv, e.npd.digest)
	}
	fb, _ := aes.NewCipher(final)
	cipher.NewCBCDecrypter(fb, iv).CryptBlocks(buf, buf)
	return int(length), nil
}

// ReadAt reads decrypted data.
func (e *EDAT) ReadAt(p []byte, off int64) (int, error) {
	if off >= e.Size {
		return 0, io.EOF
	}
	buf := make([]byte, e.blockSize+16)
	n := 0
	for n < len(p) && off < e.Size {
		bi := uint64(off) / e.blockSize
		bo := uint64(off) % e.blockSize
		l, err := e.block(bi, buf)
		if err != nil {
			return n, err
		}
		c := copy(p[n:], buf[bo:l])
		n += c
		off += int64(c)
	}
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}
