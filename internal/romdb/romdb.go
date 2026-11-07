// Package romdb is RetroPixels' history of arcade ROM sets: for every
// arcade machine in every MAME version from 0.171 on, the chip files (ROMs)
// its set holds, identified by their CRC-32. It's made from MAME's own
// source code by tools/romhistory and built into the programs, so a ROM set
// can be recognised whatever its files and its zip are called, and the MAME
// versions that accept it worked out without asking any MAME.
//
// The facts come from MAME's source (the MAME team's ROM listings, under
// MAME's licence); the format and the code here are RetroPixels' own.
package romdb

import (
	"bytes"
	"compress/gzip"
	_ "embed"
	"encoding/binary"
	"errors"
	"io"
	"sort"
	"strings"
	"sync"
)

// Machine is one arcade machine (a "set" in MAME's words).
type Machine struct {
	Name     string // MAME's short name: "mspacman"
	Desc     string // its full name, from the newest version that has it
	Parent   string // the set it's a clone of, or the BIOS it runs on ("" = none)
	BiosRoot bool   // a BIOS set (neogeo, pgm...), not a game
	From, To int    // the first and last version (indexes into Versions) that have it
	// the BIOS choices (ROM_SYSTEM_BIOS) by number, and the default one
	Bios        []string
	DefaultBios string
	Roms        []Rom
	CHD         bool // needs a disk image (.chd) as well, which this history doesn't follow
}

// Rom is one chip file of a machine, in a range of versions.
type Rom struct {
	Name     string
	CRC      uint32
	Size     uint32
	Bios     int  // the BIOS choice it belongs to, -1 = every choice
	Bad      bool // MAME marks the dump as bad (a better one may come)
	From, To int  // versions that list it with this CRC
}

// In: the ROM is part of the machine in version v.
func (r Rom) In(v int) bool { return v >= r.From && v <= r.To }

// In: the machine exists in version v.
func (m *Machine) In(v int) bool { return v >= m.From && v <= m.To }

// DB is the whole history.
type DB struct {
	Versions []string // "0.171" ... oldest first
	Machines []*Machine
	byName   map[string]*Machine
	byCRC    map[uint32][]*Machine
}

// Machine finds a machine by its short name.
func (d *DB) Machine(name string) *Machine { return d.byName[strings.ToLower(name)] }

// WithCRC lists the machines that have a file with this CRC in any version.
func (d *DB) WithCRC(crc uint32) []*Machine { return d.byCRC[crc] }

// Version is the index of a version string ("0.264"), or -1.
func (d *DB) Version(v string) int {
	v = strings.TrimSpace(v)
	for i, x := range d.Versions {
		if x == v {
			return i
		}
	}
	return -1
}

// Newest is the index of the newest version.
func (d *DB) Newest() int { return len(d.Versions) - 1 }

func (d *DB) index() {
	d.byName = make(map[string]*Machine, len(d.Machines))
	d.byCRC = make(map[uint32][]*Machine)
	for _, m := range d.Machines {
		d.byName[m.Name] = m
		seen := map[uint32]bool{}
		for _, r := range m.Roms {
			if !seen[r.CRC] {
				seen[r.CRC] = true
				d.byCRC[r.CRC] = append(d.byCRC[r.CRC], m)
			}
		}
	}
}

// ---------------------------------------------------------------- file format
//
// gzip of: "RPROMDB1", then uvarints and strings (uvarint length + bytes):
// version count, versions; string table count, strings; machine count, then
// each machine: name, desc, parent (string indexes), flags (1 BIOS root,
// 2 CHD), from, to, bios count, bios names, default bios, rom count, then
// each rom: name, crc (4 bytes little-endian), size, bios+1, bad, from, to.

const magic = "RPROMDB1"

// Encode writes the history in the format Decode reads.
func (d *DB) Encode(w io.Writer) error {
	zw, err := gzip.NewWriterLevel(w, gzip.BestCompression)
	if err != nil {
		return err
	}
	var b bytes.Buffer
	b.WriteString(magic)
	uv := func(v int) {
		var t [binary.MaxVarintLen64]byte
		b.Write(t[:binary.PutUvarint(t[:], uint64(v))])
	}
	str := func(s string) { uv(len(s)); b.WriteString(s) }
	uv(len(d.Versions))
	for _, v := range d.Versions {
		str(v)
	}
	// strings used more than once go in a table
	idx := map[string]int{}
	var table []string
	add := func(s string) {
		if _, ok := idx[s]; !ok {
			idx[s] = len(table)
			table = append(table, s)
		}
	}
	add("")
	for _, m := range d.Machines {
		add(m.Name)
		add(m.Desc)
		add(m.Parent)
		add(m.DefaultBios)
		for _, x := range m.Bios {
			add(x)
		}
		for _, r := range m.Roms {
			add(r.Name)
		}
	}
	uv(len(table))
	for _, s := range table {
		str(s)
	}
	uv(len(d.Machines))
	for _, m := range d.Machines {
		uv(idx[m.Name])
		uv(idx[m.Desc])
		uv(idx[m.Parent])
		f := 0
		if m.BiosRoot {
			f |= 1
		}
		if m.CHD {
			f |= 2
		}
		uv(f)
		uv(m.From)
		uv(m.To)
		uv(len(m.Bios))
		for _, x := range m.Bios {
			uv(idx[x])
		}
		uv(idx[m.DefaultBios])
		uv(len(m.Roms))
		for _, r := range m.Roms {
			uv(idx[r.Name])
			var c [4]byte
			binary.LittleEndian.PutUint32(c[:], r.CRC)
			b.Write(c[:])
			uv(int(r.Size))
			uv(r.Bios + 1)
			if r.Bad {
				uv(1)
			} else {
				uv(0)
			}
			uv(r.From)
			uv(r.To)
		}
	}
	if _, err := zw.Write(b.Bytes()); err != nil {
		return err
	}
	return zw.Close()
}

var errFormat = errors.New("romdb: not a ROM history file")

// Decode reads a history written by Encode.
func Decode(data []byte) (*DB, error) {
	zr, err := gzip.NewReader(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	raw, err := io.ReadAll(zr)
	if err != nil {
		return nil, err
	}
	if !bytes.HasPrefix(raw, []byte(magic)) {
		return nil, errFormat
	}
	p := len(magic)
	bad := false
	uv := func() int {
		v, n := binary.Uvarint(raw[p:])
		if n <= 0 {
			bad = true
			return 0
		}
		p += n
		return int(v)
	}
	str := func() string {
		n := uv()
		if bad || n > len(raw)-p {
			bad = true
			return ""
		}
		s := string(raw[p : p+n])
		p += n
		return s
	}
	d := &DB{}
	nv := uv()
	for i := 0; i < nv && !bad; i++ {
		d.Versions = append(d.Versions, str())
	}
	nt := uv()
	if bad || nt > len(raw) {
		return nil, errFormat
	}
	table := make([]string, 0, nt)
	for i := 0; i < nt && !bad; i++ {
		table = append(table, str())
	}
	ts := func() string {
		i := uv()
		if i >= len(table) {
			bad = true
			return ""
		}
		return table[i]
	}
	nm := uv()
	if bad || nm > len(raw) {
		return nil, errFormat
	}
	d.Machines = make([]*Machine, 0, nm)
	for i := 0; i < nm && !bad; i++ {
		m := &Machine{Name: ts(), Desc: ts(), Parent: ts()}
		f := uv()
		m.BiosRoot, m.CHD = f&1 != 0, f&2 != 0
		m.From, m.To = uv(), uv()
		nb := uv()
		if nb > 1000 {
			return nil, errFormat
		}
		for j := 0; j < nb; j++ {
			m.Bios = append(m.Bios, ts())
		}
		m.DefaultBios = ts()
		nr := uv()
		if bad || nr > len(raw) {
			return nil, errFormat
		}
		m.Roms = make([]Rom, 0, nr)
		for j := 0; j < nr && !bad; j++ {
			r := Rom{Name: ts()}
			if p+4 > len(raw) {
				return nil, errFormat
			}
			r.CRC = binary.LittleEndian.Uint32(raw[p:])
			p += 4
			r.Size = uint32(uv())
			r.Bios = uv() - 1
			r.Bad = uv() != 0
			r.From, r.To = uv(), uv()
			m.Roms = append(m.Roms, r)
		}
		d.Machines = append(d.Machines, m)
	}
	if bad {
		return nil, errFormat
	}
	sort.Slice(d.Machines, func(a, b int) bool { return d.Machines[a].Name < d.Machines[b].Name })
	d.index()
	return d, nil
}

// New makes a DB from machines (for tools and tests).
func New(versions []string, machines []*Machine) *DB {
	d := &DB{Versions: versions, Machines: machines}
	sort.Slice(d.Machines, func(a, b int) bool { return d.Machines[a].Name < d.Machines[b].Name })
	d.index()
	return d
}

//go:embed history.bin
var embedded []byte

var (
	loadOnce sync.Once
	loaded   *DB
	loadErr  error
)

// Load returns the built-in history (read the first time it's asked for).
func Load() (*DB, error) {
	loadOnce.Do(func() { loaded, loadErr = Decode(embedded) })
	return loaded, loadErr
}
