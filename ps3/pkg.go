// Package ps3 is RetroPixels' own PlayStation 3 emulator. It is written for
// one game, Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled (PSN,
// NPEB00149 / NPUB30010), and grows only as far as that game needs.
package ps3

import (
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// pkgKey is the AES key of retail PS3 packages (revision 0x8000).
var pkgKey = []byte{0x2E, 0x7B, 0x71, 0xD7, 0xC9, 0xC9, 0xA1, 0x4E, 0xA3, 0x22, 0x1F, 0x18, 0x88, 0x28, 0xB8, 0xF8}

// PKGFile is one file or folder in a package.
type PKGFile struct {
	Name   string
	Offset uint64 // from the start of the package's data
	Size   uint64
	Type   uint32
	Dir    bool
}

// PKG is an open .pkg.
type PKG struct {
	r         io.ReaderAt
	ContentID string
	dataOff   uint64
	dataSize  uint64
	iv        [16]byte
	block     cipher.Block
	Files     []PKGFile
}

// OpenPKG reads a package's header and file list.
func OpenPKG(r io.ReaderAt) (*PKG, error) {
	h := make([]byte, 0x80)
	if _, err := r.ReadAt(h, 0); err != nil {
		return nil, err
	}
	if binary.BigEndian.Uint32(h) != 0x7F504B47 {
		return nil, errors.New("not a PS3 package (.pkg)")
	}
	rev := binary.BigEndian.Uint16(h[4:])
	typ := binary.BigEndian.Uint16(h[6:])
	if rev != 0x8000 || typ != 1 {
		return nil, fmt.Errorf("only retail PS3 packages can be opened (revision %#x, type %d)", rev, typ)
	}
	p := &PKG{r: r}
	count := binary.BigEndian.Uint32(h[0x14:])
	p.dataOff = binary.BigEndian.Uint64(h[0x20:])
	p.dataSize = binary.BigEndian.Uint64(h[0x28:])
	p.ContentID = strings.TrimRight(string(h[0x30:0x60]), "\x00")
	copy(p.iv[:], h[0x70:0x80])
	p.block, _ = aes.NewCipher(pkgKey)
	if count > 100000 {
		return nil, errors.New("the package's file list is damaged")
	}
	table := make([]byte, 32*int(count))
	if err := p.readData(table, 0); err != nil {
		return nil, err
	}
	for i := 0; i < int(count); i++ {
		e := table[i*32:]
		no := uint64(binary.BigEndian.Uint32(e))
		ns := uint64(binary.BigEndian.Uint32(e[4:]))
		f := PKGFile{
			Offset: binary.BigEndian.Uint64(e[8:]),
			Size:   binary.BigEndian.Uint64(e[16:]),
			Type:   binary.BigEndian.Uint32(e[24:]),
		}
		f.Dir = f.Type&0xFF == 4
		if ns > 1024 || no+ns > p.dataSize {
			return nil, errors.New("the package's file list is damaged")
		}
		name := make([]byte, ns)
		if err := p.readData(name, no); err != nil {
			return nil, err
		}
		f.Name = strings.TrimRight(string(name), "\x00")
		p.Files = append(p.Files, f)
	}
	return p, nil
}

// readData decrypts len(buf) bytes of the data area at off.
func (p *PKG) readData(buf []byte, off uint64) error {
	start := off &^ 15
	raw := make([]byte, (int(off-start)+len(buf)+15)&^15)
	n, err := p.r.ReadAt(raw, int64(p.dataOff+start))
	if n < int(off-start)+len(buf) {
		if err == nil {
			err = io.ErrUnexpectedEOF
		}
		return err
	}
	var ctr [16]byte
	copy(ctr[:], p.iv[:])
	addCounter(&ctr, start/16)
	s := cipher.NewCTR(p.block, ctr[:])
	s.XORKeyStream(raw, raw)
	copy(buf, raw[off-start:])
	return nil
}

func addCounter(c *[16]byte, n uint64) {
	lo := binary.BigEndian.Uint64(c[8:])
	hi := binary.BigEndian.Uint64(c[:8])
	nlo := lo + n
	if nlo < lo {
		hi++
	}
	binary.BigEndian.PutUint64(c[8:], nlo)
	binary.BigEndian.PutUint64(c[:8], hi)
}

// Read returns a whole file.
func (p *PKG) Read(f PKGFile) ([]byte, error) {
	if f.Size > 1<<31 {
		return nil, errors.New("file too big")
	}
	b := make([]byte, f.Size)
	return b, p.readData(b, f.Offset)
}

// Extract writes every file under dir, in chunks.
func (p *PKG) Extract(dir string, progress func(done, total uint64)) error {
	var total, done uint64
	for _, f := range p.Files {
		total += f.Size
	}
	buf := make([]byte, 4<<20)
	for _, f := range p.Files {
		name := filepath.FromSlash(f.Name)
		if !filepath.IsLocal(name) {
			return fmt.Errorf("the package has a bad file name: %q", f.Name)
		}
		path := filepath.Join(dir, name)
		if f.Dir {
			if err := os.MkdirAll(path, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return err
		}
		out, err := os.Create(path)
		if err != nil {
			return err
		}
		for off := uint64(0); off < f.Size; {
			n := min(uint64(len(buf)), f.Size-off)
			if err := p.readData(buf[:n], f.Offset+off); err != nil {
				out.Close()
				return err
			}
			if _, err := out.Write(buf[:n]); err != nil {
				out.Close()
				return err
			}
			off += n
			done += n
			if progress != nil {
				progress(done, total)
			}
		}
		if err := out.Close(); err != nil {
			return err
		}
	}
	return nil
}
