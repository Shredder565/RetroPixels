package wiiu

// .wua archives: Wii U titles (a game, its update and DLC) packed into one
// file. The format is ZArchive: the files' bytes run together as one
// stream cut into 64 KiB blocks, each compressed with Zstandard; a table
// gives where each block starts, and a file tree gives each file's offset
// and size in the stream. Every number is big-endian, and the footer at the
// end of the file says where each part is.
//
// Inside, each title is a folder named "<title ID>_v<version>" holding its
// code, content and meta folders.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"retropixels/internal/zstd"
)

const (
	zaBlock   = 64 << 10
	zaMagic   = 0x169f52d6
	zaVersion = 0x61bf3a01
	zaFooter  = 6*16 + 32 + 8 + 4 + 4
)

type zaSection struct{ off, size uint64 }

type zaNode struct {
	name     string
	file     bool
	off, len uint64 // files: in the stream
	first    uint32 // folders: their entries
	count    uint32
}

// zArchive reads a ZArchive file.
type zArchive struct {
	f       io.ReaderAt
	closer  io.Closer
	data    zaSection
	records []byte
	nodes   []zaNode

	mu    sync.Mutex
	cache map[uint64][]byte // recent blocks
	order []uint64
}

func openZArchive(f io.ReaderAt, size int64) (*zArchive, error) {
	if size < zaFooter {
		return nil, errors.New("too short to be a .wua")
	}
	ft := make([]byte, zaFooter)
	if _, err := f.ReadAt(ft, size-zaFooter); err != nil {
		return nil, err
	}
	if binary.BigEndian.Uint32(ft[zaFooter-4:]) != zaMagic {
		return nil, errors.New("not a .wua (ZArchive) file")
	}
	if v := binary.BigEndian.Uint32(ft[zaFooter-8:]); v != zaVersion {
		return nil, fmt.Errorf("a .wua of an unknown version (%08x)", v)
	}
	var sec [6]zaSection
	for i := range sec {
		sec[i] = zaSection{binary.BigEndian.Uint64(ft[i*16:]), binary.BigEndian.Uint64(ft[i*16+8:])}
		if sec[i].off+sec[i].size > uint64(size) {
			return nil, errors.New("the .wua is cut short")
		}
	}
	read := func(s zaSection) ([]byte, error) {
		if s.size > 1<<30 {
			return nil, errors.New("the .wua's tables are too big")
		}
		b := make([]byte, s.size)
		_, err := f.ReadAt(b, int64(s.off))
		return b, err
	}
	a := &zArchive{f: f, data: sec[0], cache: map[uint64][]byte{}}
	var err error
	if a.records, err = read(sec[1]); err != nil {
		return nil, err
	}
	names, err := read(sec[2])
	if err != nil {
		return nil, err
	}
	tree, err := read(sec[3])
	if err != nil {
		return nil, err
	}
	name := func(off uint32) (string, error) {
		if off == 0x7fffffff {
			return "", nil
		}
		o := int(off)
		if o >= len(names) {
			return "", errors.New("bad name")
		}
		n := int(names[o])
		o++
		if n&0x80 != 0 {
			if o >= len(names) {
				return "", errors.New("bad name")
			}
			n = n&0x7f | int(names[o])<<7
			o++
		}
		if o+n > len(names) {
			return "", errors.New("bad name")
		}
		return string(names[o : o+n]), nil
	}
	for i := 0; i+16 <= len(tree); i += 16 {
		e := tree[i:]
		tn := binary.BigEndian.Uint32(e)
		nm, err := name(tn & 0x7fffffff)
		if err != nil {
			return nil, fmt.Errorf("the .wua's file tree: %v", err)
		}
		n := zaNode{name: nm, file: tn>>31 != 0}
		if n.file {
			n.off = uint64(binary.BigEndian.Uint32(e[4:])) | uint64(binary.BigEndian.Uint16(e[14:]))<<32
			n.len = uint64(binary.BigEndian.Uint32(e[8:])) | uint64(binary.BigEndian.Uint16(e[12:]))<<32
		} else {
			n.first, n.count = binary.BigEndian.Uint32(e[4:]), binary.BigEndian.Uint32(e[8:])
		}
		a.nodes = append(a.nodes, n)
	}
	if len(a.nodes) == 0 || a.nodes[0].file {
		return nil, errors.New("the .wua has no file tree")
	}
	for _, n := range a.nodes {
		if !n.file && uint64(n.first)+uint64(n.count) > uint64(len(a.nodes)) {
			return nil, errors.New("the .wua's file tree is damaged")
		}
	}
	return a, nil
}

// block gives uncompressed block i of the stream.
func (a *zArchive) block(i uint64) ([]byte, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if b, ok := a.cache[i]; ok {
		return b, nil
	}
	r := int(i/16) * 40
	if r+40 > len(a.records) {
		return nil, errors.New("read past the end of the .wua")
	}
	rec := a.records[r:]
	off := binary.BigEndian.Uint64(rec)
	for k := uint64(0); k < i%16; k++ {
		off += uint64(binary.BigEndian.Uint16(rec[8+2*k:])) + 1
	}
	n := int(binary.BigEndian.Uint16(rec[8+2*(i%16):])) + 1
	raw := make([]byte, n)
	if _, err := a.f.ReadAt(raw, int64(a.data.off+off)); err != nil && err != io.EOF {
		return nil, err
	}
	b := raw
	if n != zaBlock {
		var err error
		if b, err = zstd.Decode(raw); err != nil {
			return nil, fmt.Errorf("the .wua: %v", err)
		}
	}
	if len(a.order) >= 32 {
		delete(a.cache, a.order[0])
		a.order = a.order[1:]
	}
	a.cache[i] = b
	a.order = append(a.order, i)
	return b, nil
}

func (a *zArchive) readAt(p []byte, off uint64) error {
	for len(p) > 0 {
		b, err := a.block(off / zaBlock)
		if err != nil {
			return err
		}
		in := int(off % zaBlock)
		if in >= len(b) {
			return errors.New("the .wua's data is cut short")
		}
		k := copy(p, b[in:])
		p, off = p[k:], off+uint64(k)
	}
	return nil
}

func (a *zArchive) lookup(dir uint32, name string) (uint32, bool) {
	n := a.nodes[dir]
	for i := n.first; i < n.first+n.count; i++ {
		if strings.EqualFold(a.nodes[i].name, name) {
			return i, true
		}
	}
	return 0, false
}

// zaFS is a folder of an archive as a file system.
type zaFS struct {
	a    *zArchive
	root uint32
}

func (z *zaFS) find(name string) (uint32, error) {
	if !fs.ValidPath(name) {
		return 0, fs.ErrInvalid
	}
	i := z.root
	if name == "." {
		return i, nil
	}
	for _, part := range strings.Split(name, "/") {
		if z.a.nodes[i].file {
			return 0, fs.ErrNotExist
		}
		j, ok := z.a.lookup(i, part)
		if !ok {
			return 0, fs.ErrNotExist
		}
		i = j
	}
	return i, nil
}

func (z *zaFS) Open(name string) (fs.File, error) {
	i, err := z.find(name)
	if err != nil {
		return nil, &fs.PathError{Op: "open", Path: name, Err: err}
	}
	if z.a.nodes[i].file {
		return &zaFile{a: z.a, i: i}, nil
	}
	return &zaDir{a: z.a, i: i}, nil
}

type zaInfo struct {
	a *zArchive
	i uint32
}

func (s zaInfo) Name() string {
	if s.a.nodes[s.i].name == "" {
		return "."
	}
	return s.a.nodes[s.i].name
}
func (s zaInfo) Size() int64 { return int64(s.a.nodes[s.i].len) }
func (s zaInfo) Mode() fs.FileMode {
	if s.a.nodes[s.i].file {
		return 0o444
	}
	return fs.ModeDir | 0o555
}
func (s zaInfo) ModTime() time.Time         { return time.Time{} }
func (s zaInfo) IsDir() bool                { return !s.a.nodes[s.i].file }
func (s zaInfo) Sys() any                   { return nil }
func (s zaInfo) Type() fs.FileMode          { return s.Mode().Type() }
func (s zaInfo) Info() (fs.FileInfo, error) { return s, nil }

type zaFile struct {
	a   *zArchive
	i   uint32
	pos int64
}

func (f *zaFile) Stat() (fs.FileInfo, error) { return zaInfo{f.a, f.i}, nil }
func (f *zaFile) Close() error               { return nil }

func (f *zaFile) ReadAt(p []byte, off int64) (int, error) {
	n := f.a.nodes[f.i]
	if off >= int64(n.len) {
		return 0, io.EOF
	}
	k := int(min(int64(len(p)), int64(n.len)-off))
	if err := f.a.readAt(p[:k], n.off+uint64(off)); err != nil {
		return 0, err
	}
	if k < len(p) {
		return k, io.EOF
	}
	return k, nil
}

func (f *zaFile) Read(p []byte) (int, error) {
	n, err := f.ReadAt(p, f.pos)
	f.pos += int64(n)
	if err == io.EOF && n > 0 {
		err = nil
	}
	return n, err
}

type zaDir struct {
	a    *zArchive
	i    uint32
	read int
}

func (d *zaDir) Stat() (fs.FileInfo, error) { return zaInfo{d.a, d.i}, nil }
func (d *zaDir) Read([]byte) (int, error)   { return 0, errors.New("is a folder") }
func (d *zaDir) Close() error               { return nil }

func (d *zaDir) ReadDir(count int) ([]fs.DirEntry, error) {
	n := d.a.nodes[d.i]
	var all []fs.DirEntry
	for j := n.first + uint32(d.read); j < n.first+n.count; j++ {
		all = append(all, zaInfo{d.a, j})
	}
	if count > 0 {
		if len(all) == 0 {
			return nil, io.EOF
		}
		all = all[:min(count, len(all))]
	}
	d.read += len(all)
	return all, nil
}

// ---- titles ----

// overlayFS shows upper's files over lower's (an update over its game).
type overlayFS struct{ upper, lower fs.FS }

func (o overlayFS) Open(name string) (fs.File, error) {
	f, err := o.upper.Open(name)
	if err == nil {
		if st, e := f.Stat(); e == nil && st.IsDir() {
			f.Close()
			return &overlayDir{o: o, name: name}, nil
		}
		return f, nil
	}
	if real := findFold(o.upper, name); real != "" && real != name {
		return o.Open(real)
	}
	f, err = o.lower.Open(name)
	if err != nil {
		if real := findFold(o.lower, name); real != "" && real != name {
			return o.lower.Open(real)
		}
		return nil, err
	}
	return f, nil
}

type overlayDir struct {
	o    overlayFS
	name string
	ents []fs.DirEntry
	read bool
}

func (d *overlayDir) Stat() (fs.FileInfo, error) {
	f, err := d.o.upper.Open(d.name)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return f.Stat()
}
func (d *overlayDir) Read([]byte) (int, error) { return 0, errors.New("is a folder") }
func (d *overlayDir) Close() error             { return nil }

func (d *overlayDir) ReadDir(count int) ([]fs.DirEntry, error) {
	if !d.read {
		d.read = true
		seen := map[string]bool{}
		for _, m := range []fs.FS{d.o.upper, d.o.lower} {
			dir := d.name
			if real := findFold(m, dir); real != "" {
				dir = real
			}
			es, _ := fs.ReadDir(m, dir)
			for _, e := range es {
				k := strings.ToLower(e.Name())
				if !seen[k] {
					seen[k] = true
					d.ents = append(d.ents, e)
				}
			}
		}
		sort.Slice(d.ents, func(i, j int) bool { return d.ents[i].Name() < d.ents[j].Name() })
	}
	all := d.ents
	if count > 0 {
		if len(all) == 0 {
			return nil, io.EOF
		}
		all = all[:min(count, len(all))]
	}
	d.ents = d.ents[len(all):]
	return all, nil
}

type wuaTitle struct {
	id      uint64
	version int
	node    uint32
}

// TitleFromWUA opens the game in a .wua (with its update, when the archive
// has one): its program, the program's name and the title's folders.
func TitleFromWUA(path string) (rpx []byte, name string, files *FileSystem, err error) {
	h, err := os.Open(path)
	if err != nil {
		return nil, "", nil, err
	}
	st, err := h.Stat()
	if err != nil {
		h.Close()
		return nil, "", nil, err
	}
	a, err := openZArchive(h, st.Size())
	if err != nil {
		h.Close()
		return nil, "", nil, err
	}
	a.closer = h
	var titles []wuaTitle
	root := a.nodes[0]
	for i := root.first; i < root.first+root.count; i++ {
		n := a.nodes[i]
		idStr, ver, ok := strings.Cut(n.name, "_v")
		if n.file || !ok || len(idStr) != 16 {
			continue
		}
		id, err1 := strconv.ParseUint(idStr, 16, 64)
		v, err2 := strconv.Atoi(ver)
		if err1 != nil || err2 != nil {
			continue
		}
		titles = append(titles, wuaTitle{id, v, i})
	}
	pick := func(high uint64, low uint64) *wuaTitle {
		var best *wuaTitle
		for i := range titles {
			t := &titles[i]
			if t.id>>32 == high && (low == 0 || t.id&0xffffffff == low) && (best == nil || t.version > best.version) {
				best = t
			}
		}
		return best
	}
	game := pick(0x00050000, 0)
	if game == nil {
		h.Close()
		return nil, "", nil, errors.New("the .wua has no game in it (only updates or DLC)")
	}
	update := pick(0x0005000e, game.id&0xffffffff)
	files = NewFileSystem()
	for _, d := range []string{"code", "content", "meta"} {
		var lower, upper fs.FS
		if i, ok := a.lookup(game.node, d); ok {
			lower = &zaFS{a: a, root: i}
		}
		if update != nil {
			if i, ok := a.lookup(update.node, d); ok {
				upper = &zaFS{a: a, root: i}
			}
		}
		switch {
		case lower != nil && upper != nil:
			files.Mount("/vol/"+d, overlayFS{upper, lower})
		case upper != nil:
			files.Mount("/vol/"+d, upper)
		case lower != nil:
			files.Mount("/vol/"+d, lower)
		}
	}
	// DLC: /vol/aoc<its title ID>, as the AOC library hands out
	if dlc := pick(0x0005000c, game.id&0xffffffff); dlc != nil {
		if i, ok := a.lookup(dlc.node, "content"); ok {
			files.Mount(fmt.Sprintf("/vol/aoc%016x", dlc.id), &zaFS{a: a, root: i})
		}
	}
	files.TitleID = game.id
	rpx, name, err = titleProgram(files)
	if err != nil {
		h.Close()
		return nil, "", nil, err
	}
	return rpx, name, files, nil
}
