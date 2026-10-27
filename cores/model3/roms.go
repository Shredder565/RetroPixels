package model3

import (
	"fmt"
	"hash/crc32"
	"strings"
)

// Input layouts
const (
	InputJoystick       = iota // 8-way stick and buttons
	InputAnalogJoystick        // Star Wars Trilogy's flight yoke
)

// RegionFile is one chip of a ROM region.
type RegionFile struct {
	Name   string
	CRC    uint32
	Offset int // byte offset of its first chunk in the region
	Size   int
}

// Region is how a group of chips is laid out in memory: each file adds
// Chunk bytes every Stride bytes, starting at its Offset; Swap swaps the
// bytes of each 16-bit word.
type Region struct {
	Stride, Chunk int
	Swap          bool
	Files         []RegionFile
}

// GameDef is one Model 3 game.
type GameDef struct {
	ID      string
	Parent  string
	Title   string
	Year    int
	Maker   string
	Step    int
	Inputs  int
	DSB     string // MPEG board: "DSB2", "DSB1" or ""
	Drive   string // force feedback board: "joystick" or ""
	Key     uint32 // security board encryption key (0: none)
	Regions map[string]*Region
}

// ROMs are a game's regions, loaded and laid out.
type ROMs struct {
	CROM       []byte // 8 MB, big endian (ends at 0xFFFFFFFF)
	BankedCROM []byte // 128 MB, big endian
	VROM       []byte // 64 MB, as on the board (little-endian words)
	SoundProg  []byte // 512 KB, big endian (68000)
	Samples    []byte // 16 MB, big endian
	DSBProg    []byte // 128 KB, big endian
	MPEG       []byte // 16 MB
	DriveProg  []byte // 64 KB, or nil
}

// Find returns the game with this set name.
func Find(id string) *GameDef {
	for _, g := range Games {
		if g.ID == id {
			return g
		}
	}
	return nil
}

// region: a game's region, falling back to its parent's.
func (g *GameDef) region(name string) *Region {
	if r := g.Regions[name]; r != nil {
		return r
	}
	if g.Parent != "" {
		if p := Find(g.Parent); p != nil {
			return p.region(name)
		}
	}
	return nil
}

// AllFiles lists every file the game needs (its parent's too), by region.
func (g *GameDef) AllFiles() map[string][]RegionFile {
	out := map[string][]RegionFile{}
	for _, name := range []string{"crom", "banked_crom", "vrom", "sound_program", "sound_samples", "mpeg_program", "mpeg_music", "driveboard_program"} {
		if r := g.region(name); r != nil {
			out[name] = r.Files
		}
	}
	return out
}

// FileSource finds a file's bytes by its CRC-32 or its name.
type FileSource struct {
	ByCRC  map[uint32][]byte
	ByName map[string][]byte
}

// NewFileSource indexes files (name -> bytes).
func NewFileSource(files map[string][]byte) *FileSource {
	s := &FileSource{ByCRC: map[uint32][]byte{}, ByName: map[string][]byte{}}
	for n, d := range files {
		s.ByName[strings.ToLower(n)] = d
		s.ByCRC[crc32.ChecksumIEEE(d)] = d
	}
	return s
}

func (s *FileSource) get(f RegionFile) []byte {
	if d, ok := s.ByCRC[f.CRC]; ok {
		return d
	}
	if d, ok := s.ByName[strings.ToLower(f.Name)]; ok && (f.Size == 0 || len(d) == f.Size) {
		return d
	}
	return nil
}

// Missing lists the required files src doesn't have.
func (g *GameDef) Missing(src *FileSource) []string {
	var miss []string
	for name, files := range g.AllFiles() {
		if name == "driveboard_program" {
			continue
		}
		for _, f := range files {
			if src.get(f) == nil {
				miss = append(miss, f.Name)
			}
		}
	}
	return miss
}

func loadRegion(r *Region, src *FileSource, size int) ([]byte, int, error) {
	buf := make([]byte, size)
	end := 0
	if r == nil {
		return buf, 0, nil
	}
	for _, f := range r.Files {
		d := src.get(f)
		if d == nil {
			return nil, 0, Error(fmt.Sprintf("%s is missing from the ROM set", f.Name))
		}
		chunks := len(d) / r.Chunk
		for i := 0; i < chunks; i++ {
			o := f.Offset + i*r.Stride
			if o+r.Chunk > size {
				break
			}
			copy(buf[o:o+r.Chunk], d[i*r.Chunk:])
			if o+r.Chunk > end {
				end = o + r.Chunk
			}
		}
	}
	if r.Swap {
		for i := 0; i+1 < size; i += 2 {
			buf[i], buf[i+1] = buf[i+1], buf[i]
		}
	}
	return buf, end, nil
}

// LoadROMs lays out a game's regions from its files.
func (g *GameDef) LoadROMs(src *FileSource) (*ROMs, error) {
	r := &ROMs{}
	var err error
	var n int

	// Fixed CROM: 8 MB; a smaller one sits at the top
	var crom []byte
	if crom, n, err = loadRegion(g.region("crom"), src, 0x800000); err != nil {
		return nil, err
	}
	r.CROM = make([]byte, 0x800000)
	copy(r.CROM[0x800000-n:], crom[:n])

	// Banked CROM: 128 MB, mirrored when smaller
	if r.BankedCROM, n, err = loadRegion(g.region("banked_crom"), src, 0x8000000); err != nil {
		return nil, err
	}
	mirror(r.BankedCROM, n)

	if r.VROM, n, err = loadRegion(g.region("vrom"), src, 0x4000000); err != nil {
		return nil, err
	}
	mirror(r.VROM, n)

	if r.SoundProg, _, err = loadRegion(g.region("sound_program"), src, 0x80000); err != nil {
		return nil, err
	}
	if r.Samples, n, err = loadRegion(g.region("sound_samples"), src, 0x1000000); err != nil {
		return nil, err
	}
	if n > 0 && n <= 0x800000 {
		copy(r.Samples[0x800000:], r.Samples[:0x800000])
	}
	if g.region("mpeg_program") != nil {
		if r.DSBProg, _, err = loadRegion(g.region("mpeg_program"), src, 0x20000); err != nil {
			return nil, err
		}
		if r.MPEG, _, err = loadRegion(g.region("mpeg_music"), src, 0x1000000); err != nil {
			return nil, err
		}
	}
	if d := g.region("driveboard_program"); d != nil && len(d.Files) > 0 && src.get(d.Files[0]) != nil {
		r.DriveProg, _, _ = loadRegion(d, src, 0x10000)
	}
	return r, nil
}

// mirror repeats the first n bytes (rounded up to a power of two) to fill buf.
func mirror(buf []byte, n int) {
	if n == 0 {
		return
	}
	p := 1
	for p < n {
		p <<= 1
	}
	for o := p; o < len(buf); o += p {
		copy(buf[o:], buf[:p])
	}
}
