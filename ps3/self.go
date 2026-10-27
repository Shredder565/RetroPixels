package ps3

import (
	"bytes"
	"compress/zlib"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

var be = binary.BigEndian

// IsSELF reports whether b is a signed (SCE) program rather than a plain ELF.
func IsSELF(b []byte) bool { return len(b) >= 4 && be.Uint32(b) == 0x53434500 }

// DecryptSELF turns a PSN program (EBOOT.BIN, .sprx) into the plain ELF the
// console runs. klic is the game's klicensee; nil means a free-licence game.
// A plain ELF is returned unchanged.
func DecryptSELF(b []byte, klic []byte) ([]byte, error) {
	if len(b) >= 4 && be.Uint32(b) == 0x7F454C46 {
		return b, nil
	}
	if !IsSELF(b) || len(b) < 0x90 {
		return nil, errors.New("not a PS3 program (EBOOT.BIN/SELF)")
	}
	rev := be.Uint16(b[8:])
	if be.Uint16(b[10:]) != 1 {
		return nil, errors.New("this SCE file isn't a program")
	}
	metaOff := uint64(be.Uint32(b[12:]))
	hdrLen := be.Uint64(b[16:])
	if hdrLen > uint64(len(b)) {
		return nil, errors.New("the program is cut short")
	}
	appOff := be.Uint64(b[0x28:])
	elfOff := be.Uint64(b[0x30:])
	phOff := be.Uint64(b[0x38:])
	ctrlOff := be.Uint64(b[0x58:])
	ctrlSize := be.Uint64(b[0x60:])
	if appOff+0x20 > hdrLen || elfOff+0x40 > hdrLen || ctrlOff+ctrlSize > hdrLen {
		return nil, errors.New("the program's header is damaged")
	}
	progType := be.Uint32(b[appOff+0x0C:])
	if progType != 8 {
		return nil, fmt.Errorf("only PSN programs can be opened here (program type %d)", progType)
	}
	keys, ok := npdrmKeys[rev]
	if !ok {
		return nil, fmt.Errorf("RetroPixels doesn't have the key for this program (revision %d)", rev)
	}

	// The control info: find the NPDRM block (type 3) for the licence.
	license := uint32(0)
	for p := ctrlOff; p+8 <= ctrlOff+ctrlSize; {
		t := be.Uint32(b[p:])
		n := uint64(be.Uint32(b[p+4:]))
		if n == 0 {
			break
		}
		if t == 3 && p+0x10+0x10 <= hdrLen {
			license = be.Uint32(b[p+0x10+8:])
		}
		p += n
	}
	if klic == nil {
		if license != 3 && license != 0 {
			return nil, errors.New("this game needs its licence (.rap) to start")
		}
		klic = klicFree
	}

	// The metadata info: under the NPDRM layer (AES-128-CBC with the
	// klicensee, itself encrypted) and the key set's AES-256-CBC.
	mi := metaOff + 0x20
	if mi+0x40+0x20 > hdrLen {
		return nil, errors.New("the program's header is damaged")
	}
	info := append([]byte(nil), b[mi:mi+0x40]...)
	k := make([]byte, 16)
	kb, _ := aes.NewCipher(klicKey)
	kb.Decrypt(k, klic)
	nb, _ := aes.NewCipher(k)
	cipher.NewCBCDecrypter(nb, make([]byte, 16)).CryptBlocks(info, info)
	eb, _ := aes.NewCipher(keys.erk)
	cipher.NewCBCDecrypter(eb, keys.riv).CryptBlocks(info, info)
	if !bytes.Equal(info[0x10:0x20], make([]byte, 16)) || !bytes.Equal(info[0x30:0x40], make([]byte, 16)) {
		return nil, errors.New("couldn't decrypt the program (wrong licence key?)")
	}

	// Metadata header, section headers and data keys: AES-128-CTR.
	hdrs := append([]byte(nil), b[mi+0x40:hdrLen]...)
	mb, _ := aes.NewCipher(info[:16])
	cipher.NewCTR(mb, info[0x20:0x30]).XORKeyStream(hdrs, hdrs)
	secCount := int(be.Uint32(hdrs[0x0C:]))
	keyCount := int(be.Uint32(hdrs[0x10:]))
	if secCount > 256 || keyCount > 1024 || 0x20+secCount*0x30+keyCount*16 > len(hdrs) {
		return nil, errors.New("the program's header is damaged")
	}
	dataKeys := hdrs[0x20+secCount*0x30:]
	key := func(i uint32) ([]byte, error) {
		if int(i) >= keyCount {
			return nil, errors.New("the program's header is damaged")
		}
		return dataKeys[i*16 : i*16+16], nil
	}

	// The ELF: its header and program headers are plain in the SELF.
	eh := b[elfOff:]
	if be.Uint32(eh) != 0x7F454C46 || eh[4] != 2 {
		return nil, errors.New("only 64-bit PS3 programs are supported")
	}
	phnum := int(be.Uint16(eh[0x38:]))
	phentsize := uint64(be.Uint16(eh[0x36:]))
	shoff := be.Uint64(eh[0x28:])
	if phOff+uint64(phnum)*phentsize > hdrLen {
		return nil, errors.New("the program's header is damaged")
	}
	phdrs := b[phOff : phOff+uint64(phnum)*phentsize]
	size := uint64(0x40) + uint64(len(phdrs))
	for i := 0; i < phnum; i++ {
		p := phdrs[uint64(i)*phentsize:]
		size = max(size, be.Uint64(p[8:])+be.Uint64(p[0x20:]))
	}
	_ = shoff // section headers are dropped: the loader doesn't need them
	out := make([]byte, size)
	ephoff := be.Uint64(eh[0x20:])
	header := func() {
		copy(out, eh[:0x40])
		copy(out[ephoff:], phdrs)
		// no section headers in the result
		be.PutUint64(out[0x28:], 0)
		be.PutUint16(out[0x3C:], 0)
		be.PutUint16(out[0x3E:], 0)
	}
	header()
	defer header()

	for i := 0; i < secCount; i++ {
		s := hdrs[0x20+i*0x30:]
		dOff := be.Uint64(s)
		dSize := be.Uint64(s[8:])
		typ := be.Uint32(s[0x10:])
		prog := int(be.Uint32(s[0x14:]))
		enc := be.Uint32(s[0x20:])
		ki := be.Uint32(s[0x24:])
		ivi := be.Uint32(s[0x28:])
		comp := be.Uint32(s[0x2C:])
		if typ != 2 {
			continue // only program segments go into the ELF
		}
		if prog >= phnum || dOff+dSize > uint64(len(b)) {
			return nil, errors.New("the program is damaged")
		}
		data := append([]byte(nil), b[dOff:dOff+dSize]...)
		if enc == 3 {
			sk, err := key(ki)
			if err != nil {
				return nil, err
			}
			siv, err := key(ivi)
			if err != nil {
				return nil, err
			}
			sb, _ := aes.NewCipher(sk)
			cipher.NewCTR(sb, siv).XORKeyStream(data, data)
		}
		ph := phdrs[uint64(prog)*phentsize:]
		pOff := be.Uint64(ph[8:])
		pSize := be.Uint64(ph[0x20:])
		if comp == 2 {
			zr, err := zlib.NewReader(bytes.NewReader(data))
			if err != nil {
				return nil, fmt.Errorf("couldn't unpack the program: %v", err)
			}
			u, err := io.ReadAll(io.LimitReader(zr, int64(pSize)))
			if err != nil {
				return nil, fmt.Errorf("couldn't unpack the program: %v", err)
			}
			data = u
		}
		if pOff+uint64(len(data)) > uint64(len(out)) {
			return nil, errors.New("the program is damaged")
		}
		copy(out[pOff:], data)
	}
	return out, nil
}
