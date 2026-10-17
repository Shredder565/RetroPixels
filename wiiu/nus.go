package wiiu

// Encrypted titles as they come off Nintendo's update servers or out of a
// console (the "NUS" layout): title.tmd lists the contents, title.tik holds
// the title key, and each content is an encrypted file named after its ID
// (00000000.app ...). Content 0 is the file table (FST) of the title's code,
// content and meta folders; the rest hold the files.
//
// RetroPixels ships no keys. The title key is encrypted with the Wii U
// common key, which the player supplies in a keys.txt next to the title or
// next to RetroPixels (any line with a 32-digit hex key; the right one is
// found by trying them).
//
// The layout follows the public descriptions of the formats (WiiUBrew).

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// ErrNoKeys: the title needs the Wii U common key and no keys.txt had it.
var ErrNoKeys = errors.New("this title is encrypted: put the Wii U common key (from your own console) in a keys.txt next to the title or next to RetroPixels")

type nusContent struct {
	id    uint32
	index uint16
	typ   uint16
	size  uint64
}

// hashed: contents in blocks of 0x400 bytes of hashes and 0xFC00 of data.
func (c *nusContent) hashed() bool { return c.typ&2 != 0 }

type tmdInfo struct {
	titleID  uint64
	contents []nusContent
}

// signed data starts after the signature (its size depends on its type)
func sigSkip(b []byte) (int, error) {
	if len(b) < 4 {
		return 0, errors.New("too short")
	}
	switch binary.BigEndian.Uint32(b) {
	case 0x10000, 0x10003: // RSA-4096
		return 0x240, nil
	case 0x10001, 0x10004: // RSA-2048
		return 0x140, nil
	case 0x10002, 0x10005: // ECC
		return 0x80, nil
	}
	return 0, errors.New("unknown signature type")
}

func parseTMD(b []byte) (*tmdInfo, error) {
	o, err := sigSkip(b)
	if err != nil {
		return nil, fmt.Errorf("title.tmd: %v", err)
	}
	if len(b) < o+0x9c4 {
		return nil, errors.New("title.tmd is too short")
	}
	t := &tmdInfo{titleID: binary.BigEndian.Uint64(b[o+0x4c:])}
	n := int(binary.BigEndian.Uint16(b[o+0x9e:]))
	recs := o + 0xc4 + 64*0x24
	for i := 0; i < n; i++ {
		r := recs + i*0x30
		if r+0x30 > len(b) {
			return nil, errors.New("title.tmd is cut short")
		}
		t.contents = append(t.contents, nusContent{
			id:    binary.BigEndian.Uint32(b[r:]),
			index: binary.BigEndian.Uint16(b[r+4:]),
			typ:   binary.BigEndian.Uint16(b[r+6:]),
			size:  binary.BigEndian.Uint64(b[r+8:]),
		})
	}
	if len(t.contents) == 0 {
		return nil, errors.New("title.tmd lists no contents")
	}
	return t, nil
}

// parseTicket gives the encrypted title key and the title ID.
func parseTicket(b []byte) (key [16]byte, titleID uint64, err error) {
	o, err := sigSkip(b)
	if err != nil {
		return key, 0, fmt.Errorf("title.tik: %v", err)
	}
	if len(b) < o+0xa4 {
		return key, 0, errors.New("title.tik is too short")
	}
	copy(key[:], b[o+0x7f:])
	return key, binary.BigEndian.Uint64(b[o+0x9c:]), nil
}

// readKeys gives every 32-digit hex key in the keys.txt files in dirs.
func readKeys(dirs ...string) [][16]byte {
	var keys [][16]byte
	seen := map[[16]byte]bool{}
	for _, d := range dirs {
		if d == "" {
			continue
		}
		b, err := os.ReadFile(filepath.Join(d, "keys.txt"))
		if err != nil {
			continue
		}
		for _, line := range strings.Split(string(b), "\n") {
			for _, tok := range strings.FieldsFunc(line, func(r rune) bool {
				return !(r >= '0' && r <= '9' || r >= 'a' && r <= 'f' || r >= 'A' && r <= 'F')
			}) {
				if len(tok) != 32 {
					continue
				}
				var k [16]byte
				if _, err := hex.Decode(k[:], []byte(tok)); err == nil && !seen[k] {
					seen[k] = true
					keys = append(keys, k)
				}
			}
		}
	}
	return keys
}

// nusReader decrypts one content.
type nusReader struct {
	f    io.ReaderAt
	c    nusContent
	ciph cipher.Block
}

// readData reads n bytes of the content's data (hash blocks left out) from
// offset off.
func (r *nusReader) readData(off int64, n int) ([]byte, error) {
	out := make([]byte, 0, n)
	if !r.c.hashed() {
		// CBC: block k's IV is block k-1's ciphertext (the first's, the
		// content index)
		start := off &^ 15
		end := (off + int64(n) + 15) &^ 15
		buf := make([]byte, end-start+16)
		if start == 0 {
			binary.BigEndian.PutUint16(buf, r.c.index)
			clear(buf[2:16])
			if _, err := r.f.ReadAt(buf[16:], 0); err != nil && err != io.EOF {
				return nil, err
			}
		} else if _, err := r.f.ReadAt(buf, start-16); err != nil && err != io.EOF {
			return nil, err
		}
		plain := make([]byte, len(buf)-16)
		cipher.NewCBCDecrypter(r.ciph, buf[:16]).CryptBlocks(plain, buf[16:])
		return append(out, plain[off-start:off-start+int64(n)]...), nil
	}
	const blk, data = 0x10000, 0xfc00
	enc := make([]byte, blk)
	for n > 0 {
		b := off / data
		in := int(off % data)
		if _, err := r.f.ReadAt(enc, b*blk); err != nil && err != io.EOF {
			return nil, err
		}
		hashes := make([]byte, 0x400)
		var zero [16]byte
		cipher.NewCBCDecrypter(r.ciph, zero[:]).CryptBlocks(hashes, enc[:0x400])
		iv := hashes[(b%16)*20:][:16]
		plain := make([]byte, data)
		cipher.NewCBCDecrypter(r.ciph, iv).CryptBlocks(plain, enc[0x400:])
		k := min(n, data-in)
		out = append(out, plain[in:in+k]...)
		off += int64(k)
		n -= k
	}
	return out, nil
}

// ---- the file table ----

type nusNode struct {
	name    string
	dir     bool
	kids    map[string]*nusNode // by lower-case name
	size    int64
	off     int64 // in the content's data
	content int   // cluster: the index of the content holding it
}

type nusFS struct {
	root    *nusNode
	readers []*nusReader // by cluster
	files   []io.Closer
}

func parseFST(b []byte) (*nusNode, error) {
	if len(b) < 0x20 || string(b[:3]) != "FST" {
		return nil, errors.New("no file table")
	}
	factor := int64(binary.BigEndian.Uint32(b[4:]))
	clusters := int(binary.BigEndian.Uint32(b[8:]))
	ents := 0x20 + clusters*0x20
	if ents+0x10 > len(b) {
		return nil, errors.New("the file table is cut short")
	}
	n := int(binary.BigEndian.Uint32(b[ents+8:]))
	names := ents + n*0x10
	if n < 1 || names > len(b) {
		return nil, errors.New("the file table is cut short")
	}
	name := func(i int) string {
		o := names + int(binary.BigEndian.Uint32(b[ents+i*0x10:])&0xffffff)
		if o >= len(b) {
			return ""
		}
		e := bytes.IndexByte(b[o:], 0)
		if e < 0 {
			e = len(b) - o
		}
		return string(b[o : o+e])
	}
	root := &nusNode{dir: true, kids: map[string]*nusNode{}}
	// walk: each directory's entries run up to its "next" index
	type frame struct {
		n   *nusNode
		end int
	}
	stack := []frame{{root, n}}
	for i := 1; i < n; i++ {
		for len(stack) > 1 && i >= stack[len(stack)-1].end {
			stack = stack[:len(stack)-1]
		}
		e := b[ents+i*0x10:]
		typ := e[0]
		a, s := binary.BigEndian.Uint32(e[4:]), binary.BigEndian.Uint32(e[8:])
		flags, cl := binary.BigEndian.Uint16(e[12:]), binary.BigEndian.Uint16(e[14:])
		nd := &nusNode{name: name(i)}
		parent := stack[len(stack)-1].n
		if typ&1 != 0 {
			nd.dir, nd.kids = true, map[string]*nusNode{}
			parent.kids[strings.ToLower(nd.name)] = nd
			stack = append(stack, frame{nd, int(s)})
			continue
		}
		if typ&0x80 != 0 {
			continue // not in this title
		}
		nd.size, nd.off, nd.content = int64(s), int64(a)*factor, int(cl)
		if flags&4 != 0 {
			nd.off = int64(a)
		}
		parent.kids[strings.ToLower(nd.name)] = nd
	}
	return root, nil
}

// OpenNUS opens an encrypted title in dir (title.tmd, title.tik and the
// .app contents). keyDirs are more folders to look for keys.txt in.
func OpenNUS(dir string, keyDirs ...string) (*nusFS, error) {
	tb, err := os.ReadFile(filepath.Join(dir, "title.tmd"))
	if err != nil {
		return nil, err
	}
	tmd, err := parseTMD(tb)
	if err != nil {
		return nil, err
	}
	kb, err := os.ReadFile(filepath.Join(dir, "title.tik"))
	if err != nil {
		return nil, errors.New("the title's ticket (title.tik) isn't there")
	}
	encKey, tikID, err := parseTicket(kb)
	if err != nil {
		return nil, err
	}
	if tikID != tmd.titleID {
		return nil, errors.New("title.tik is for another title")
	}
	f := &nusFS{}
	ok := false
	defer func() {
		if !ok {
			f.Close()
		}
	}()
	ents, _ := os.ReadDir(dir)
	open := func(id uint32) (*os.File, error) {
		want := fmt.Sprintf("%08x.app", id)
		for _, e := range ents {
			if strings.EqualFold(e.Name(), want) {
				return os.Open(filepath.Join(dir, e.Name()))
			}
		}
		return nil, fmt.Errorf("the title's content %s isn't there", want)
	}
	// the clusters: contents by their index
	byIndex := map[uint16]nusContent{}
	maxIdx := 0
	for _, c := range tmd.contents {
		byIndex[c.index] = c
		maxIdx = max(maxIdx, int(c.index))
	}
	f.readers = make([]*nusReader, maxIdx+1)
	for idx, c := range byIndex {
		h, err := open(c.id)
		if err != nil {
			if idx == 0 {
				return nil, err
			}
			continue // files in it will fail to open
		}
		f.files = append(f.files, h)
		f.readers[idx] = &nusReader{f: h, c: c}
	}
	if f.readers[0] == nil {
		return nil, errors.New("the title has no file table")
	}
	// the title key: whichever key turns content 0 into a file table
	var iv [16]byte
	binary.BigEndian.PutUint64(iv[:], tmd.titleID)
	var fst []byte
	for _, ck := range readKeys(append([]string{dir}, keyDirs...)...) {
		cb, _ := aes.NewCipher(ck[:])
		var tk [16]byte
		cipher.NewCBCDecrypter(cb, iv[:]).CryptBlocks(tk[:], encKey[:])
		tc, _ := aes.NewCipher(tk[:])
		r := f.readers[0]
		r.ciph = tc
		head, err := r.readData(0, 16)
		if err != nil || string(head[:4]) != "FST\x00" {
			continue
		}
		if fst, err = r.readData(0, int(min(r.c.size, 64<<20))); err != nil {
			return nil, err
		}
		for _, x := range f.readers {
			if x != nil {
				x.ciph = tc
			}
		}
		break
	}
	if fst == nil {
		return nil, ErrNoKeys
	}
	if f.root, err = parseFST(fst); err != nil {
		return nil, err
	}
	ok = true
	return f, nil
}

func (f *nusFS) Close() error {
	for _, h := range f.files {
		h.Close()
	}
	f.files = nil
	return nil
}

func (f *nusFS) lookup(name string) (*nusNode, error) {
	if !fs.ValidPath(name) {
		return nil, fs.ErrInvalid
	}
	n := f.root
	if name == "." {
		return n, nil
	}
	for _, part := range strings.Split(name, "/") {
		if !n.dir {
			return nil, fs.ErrNotExist
		}
		k := n.kids[strings.ToLower(part)]
		if k == nil {
			return nil, fs.ErrNotExist
		}
		n = k
	}
	return n, nil
}

// Sub gives a folder of the title as a file system ("code", "content").
func (f *nusFS) Sub(dir string) (fs.FS, error) {
	n, err := f.lookup(dir)
	if err != nil {
		return nil, err
	}
	if !n.dir {
		return nil, fs.ErrNotExist
	}
	return &nusFS{root: n, readers: f.readers}, nil
}

func (f *nusFS) ReadFile(name string) ([]byte, error) {
	n, err := f.lookup(name)
	if err != nil {
		return nil, &fs.PathError{Op: "read", Path: name, Err: err}
	}
	if n.dir {
		return nil, &fs.PathError{Op: "read", Path: name, Err: errors.New("is a folder")}
	}
	if n.content >= len(f.readers) || f.readers[n.content] == nil {
		return nil, &fs.PathError{Op: "read", Path: name, Err: errors.New("its content is missing")}
	}
	if n.size == 0 {
		return []byte{}, nil
	}
	return f.readers[n.content].readData(n.off, int(n.size))
}

func (f *nusFS) Open(name string) (fs.File, error) {
	n, err := f.lookup(name)
	if err != nil {
		return nil, &fs.PathError{Op: "open", Path: name, Err: err}
	}
	if n.dir {
		return &nusDir{n: n}, nil
	}
	if n.content >= len(f.readers) || f.readers[n.content] == nil {
		return nil, &fs.PathError{Op: "open", Path: name, Err: errors.New("its content is missing")}
	}
	return &nusFile{n: n, r: f.readers[n.content]}, nil
}

type nusInfo struct{ n *nusNode }

func (i nusInfo) Name() string {
	if i.n.name == "" {
		return "."
	}
	return i.n.name
}
func (i nusInfo) Size() int64 { return i.n.size }
func (i nusInfo) Mode() fs.FileMode {
	if i.n.dir {
		return fs.ModeDir | 0o555
	}
	return 0o444
}
func (i nusInfo) ModTime() time.Time         { return time.Time{} }
func (i nusInfo) IsDir() bool                { return i.n.dir }
func (i nusInfo) Sys() any                   { return nil }
func (i nusInfo) Type() fs.FileMode          { return i.Mode().Type() }
func (i nusInfo) Info() (fs.FileInfo, error) { return i, nil }

// nusFile decrypts as it's read.
type nusFile struct {
	n   *nusNode
	r   *nusReader
	pos int64
}

func (f *nusFile) Stat() (fs.FileInfo, error) { return nusInfo{f.n}, nil }
func (f *nusFile) Close() error               { return nil }

func (f *nusFile) ReadAt(p []byte, off int64) (int, error) {
	if off >= f.n.size {
		return 0, io.EOF
	}
	k := int(min(int64(len(p)), f.n.size-off))
	b, err := f.r.readData(f.n.off+off, k)
	if err != nil {
		return 0, err
	}
	copy(p, b)
	if k < len(p) {
		return k, io.EOF
	}
	return k, nil
}

func (f *nusFile) Read(p []byte) (int, error) {
	n, err := f.ReadAt(p, f.pos)
	f.pos += int64(n)
	if err == io.EOF && n > 0 {
		err = nil
	}
	return n, err
}

type nusDir struct {
	n    *nusNode
	read int
}

func (d *nusDir) Stat() (fs.FileInfo, error) { return nusInfo{d.n}, nil }
func (d *nusDir) Read([]byte) (int, error)   { return 0, errors.New("is a folder") }
func (d *nusDir) Close() error               { return nil }

func (d *nusDir) ReadDir(count int) ([]fs.DirEntry, error) {
	var all []fs.DirEntry
	for _, k := range d.n.kids {
		all = append(all, nusInfo{k})
	}
	sort.Slice(all, func(i, j int) bool { return all[i].Name() < all[j].Name() })
	all = all[d.read:]
	if count > 0 {
		if len(all) == 0 {
			return nil, io.EOF
		}
		all = all[:min(count, len(all))]
	}
	d.read += len(all)
	return all, nil
}

// TitleFromNUS opens an encrypted title: its program (the .rpx cos.xml
// names), the program's name, and the title's folders.
func TitleFromNUS(dir string, keyDirs ...string) (rpx []byte, name string, files *FileSystem, err error) {
	t, err := OpenNUS(dir, keyDirs...)
	if err != nil {
		return nil, "", nil, err
	}
	files = NewFileSystem()
	for _, d := range []string{"code", "content", "meta"} {
		if sub, err := t.Sub(d); err == nil {
			files.Mount("/vol/"+d, sub)
		}
	}
	if b, err := files.ReadCode("app.xml"); err == nil {
		if id, ok := xmlHex(b, "title_id"); ok {
			files.TitleID = id
		}
	}
	rpx, name, err = titleProgram(files)
	if err != nil {
		return nil, "", nil, err
	}
	return rpx, name, files, nil
}

// titleProgram reads a title's program: the .rpx its cos.xml names (or the
// only one in its code folder).
func titleProgram(files *FileSystem) (rpx []byte, name string, err error) {
	if b, err := files.ReadCode("cos.xml"); err == nil {
		s := string(b)
		if i := strings.Index(s, "<argstr"); i >= 0 {
			if j := strings.Index(s[i:], ">"); j >= 0 {
				rest := s[i+j+1:]
				if k := strings.Index(rest, "<"); k >= 0 {
					name = strings.TrimSpace(rest[:k])
				}
			}
		}
	}
	if name == "" {
		ents, _ := files.ReadDir("/vol/code")
		for _, e := range ents {
			if strings.EqualFold(path.Ext(e.Name), ".rpx") {
				name = e.Name
				break
			}
		}
	}
	if name == "" {
		return nil, "", errors.New("the title has no program (.rpx) in its code folder")
	}
	if rpx, err = files.ReadCode(name); err != nil {
		return nil, "", fmt.Errorf("reading %s: %v", name, err)
	}
	return rpx, name, nil
}
