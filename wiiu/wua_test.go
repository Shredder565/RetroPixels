package wiiu

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// writeWUA builds a ZArchive from files (path -> bytes). Blocks are stored
// raw when full and as uncompressed Zstandard frames otherwise.
func writeWUA(t *testing.T, path string, files map[string][]byte) {
	t.Helper()
	type node struct {
		name string
		kids map[string]*node
		data []byte
		file bool
		off  uint64
		idx  int
	}
	root := &node{kids: map[string]*node{}}
	var stream []byte
	keys := make([]string, 0, len(files))
	for k := range files {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		parts := strings.Split(k, "/")
		n := root
		for _, p := range parts[:len(parts)-1] {
			if n.kids[p] == nil {
				n.kids[p] = &node{name: p, kids: map[string]*node{}}
			}
			n = n.kids[p]
		}
		n.kids[parts[len(parts)-1]] = &node{name: parts[len(parts)-1], file: true, data: files[k], off: uint64(len(stream))}
		stream = append(stream, files[k]...)
	}
	// blocks
	var comp, recs []byte
	for b := 0; b*zaBlock < len(stream) || b == 0; b++ {
		if b%16 == 0 {
			recs = binary.BigEndian.AppendUint64(recs, uint64(len(comp)))
			recs = append(recs, make([]byte, 32)...)
		}
		chunk := stream[b*zaBlock : min(len(stream), (b+1)*zaBlock)]
		var enc []byte
		if len(chunk) == zaBlock {
			enc = chunk
		} else {
			// frame: magic, descriptor (single segment, 4-byte size), size, one last raw block
			enc = binary.LittleEndian.AppendUint32(nil, 0xFD2FB528)
			enc = append(enc, 0xa0)
			enc = binary.LittleEndian.AppendUint32(enc, uint32(len(chunk)))
			h := uint32(len(chunk))<<3 | 1
			enc = append(enc, byte(h), byte(h>>8), byte(h>>16))
			enc = append(enc, chunk...)
		}
		r := len(recs) - 40
		binary.BigEndian.PutUint16(recs[r+8+2*(b%16):], uint16(len(enc)-1))
		comp = append(comp, enc...)
	}
	// tree, breadth first so each folder's entries are together
	var names []byte
	nameOff := func(s string) uint32 {
		o := uint32(len(names))
		names = append(append(names, byte(len(s))), s...)
		return o
	}
	var order []*node
	order = append(order, root)
	type rng struct{ first, count int }
	ranges := map[*node]rng{}
	for i := 0; i < len(order); i++ {
		n := order[i]
		if n.file {
			continue
		}
		var ks []string
		for k := range n.kids {
			ks = append(ks, k)
		}
		sort.Strings(ks)
		ranges[n] = rng{len(order), len(ks)}
		for _, k := range ks {
			order = append(order, n.kids[k])
		}
	}
	var tree []byte
	for _, n := range order {
		no := uint32(0x7fffffff)
		if n != root {
			no = nameOff(n.name)
		}
		if n.file {
			tree = binary.BigEndian.AppendUint32(tree, no|1<<31)
			tree = binary.BigEndian.AppendUint32(tree, uint32(n.off))
			tree = binary.BigEndian.AppendUint32(tree, uint32(len(n.data)))
			tree = binary.BigEndian.AppendUint16(tree, uint16(len(n.data)>>32))
			tree = binary.BigEndian.AppendUint16(tree, uint16(n.off>>32))
		} else {
			r := ranges[n]
			tree = binary.BigEndian.AppendUint32(tree, no)
			tree = binary.BigEndian.AppendUint32(tree, uint32(r.first))
			tree = binary.BigEndian.AppendUint32(tree, uint32(r.count))
			tree = binary.BigEndian.AppendUint32(tree, 0)
		}
	}
	out := append([]byte(nil), comp...)
	var secs [6][2]uint64
	for i, part := range [][]byte{nil, recs, names, tree, nil, nil} {
		if i == 0 {
			secs[0] = [2]uint64{0, uint64(len(comp))}
			continue
		}
		secs[i] = [2]uint64{uint64(len(out)), uint64(len(part))}
		out = append(out, part...)
	}
	for _, s := range secs {
		out = binary.BigEndian.AppendUint64(out, s[0])
		out = binary.BigEndian.AppendUint64(out, s[1])
	}
	out = append(out, make([]byte, 32)...)
	out = binary.BigEndian.AppendUint64(out, uint64(len(out)+16))
	out = binary.BigEndian.AppendUint32(out, zaVersion)
	out = binary.BigEndian.AppendUint32(out, zaMagic)
	if err := os.WriteFile(path, out, 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestWUA(t *testing.T) {
	big := make([]byte, 200000)
	for i := range big {
		big[i] = byte(i*13 + i>>9)
	}
	game, upd, dlc := "0005000010abcd00_v0/", "0005000e10abcd00_v32/", "0005000c10abcd00_v16/"
	files := map[string][]byte{
		game + "code/cos.xml":       []byte("<app><argstr type=\"string\">play.rpx</argstr></app>"),
		game + "code/play.rpx":      []byte("old program"),
		game + "content/big.bin":    big,
		game + "content/Data/a.txt": []byte("game a"),
		game + "content/Data/b.txt": []byte("game b"),
		upd + "code/cos.xml":        []byte("<app><argstr type=\"string\">play.rpx</argstr></app>"),
		upd + "code/play.rpx":       []byte("new program"),
		upd + "content/Data/b.txt":  []byte("update b"),
		upd + "content/Data/c.txt":  []byte("update c"),
		dlc + "content/extra.bin":   []byte("dlc"),
	}
	path := filepath.Join(t.TempDir(), "game.wua")
	writeWUA(t, path, files)
	rpx, name, fsys, err := TitleFromWUA(path)
	if err != nil {
		t.Fatal(err)
	}
	if name != "play.rpx" || string(rpx) != "new program" {
		t.Errorf("program %s: %q (want the update's)", name, rpx)
	}
	if fsys.TitleID != 0x0005000010abcd00 {
		t.Errorf("title %x", fsys.TitleID)
	}
	want := map[string]string{"/vol/content/data/A.TXT": "game a", "/vol/content/Data/b.txt": "update b",
		"/vol/content/Data/c.txt": "update c", "/vol/aoc0005000c10abcd00/extra.bin": "dlc"}
	for p, w := range want {
		if b, err := fsys.ReadFile(p); err != nil || string(b) != w {
			t.Errorf("%s: %q %v", p, b, err)
		}
	}
	if b, err := fsys.ReadFile("/vol/content/big.bin"); err != nil || !bytes.Equal(b, big) {
		t.Errorf("big.bin: %d bytes, %v", len(b), err)
	}
	ents, err := fsys.ReadDir("/vol/content/Data")
	if err != nil || len(ents) != 3 {
		t.Errorf("listing: %v %v", ents, err)
	}
	// reading part of a file as the program asks
	src, n, _, err := fsys.OpenRead("/vol/content/big.bin")
	if err != nil || n != int64(len(big)) {
		t.Fatal(n, err)
	}
	part := make([]byte, 1000)
	if _, err := src.ReadAt(part, 130000); err != nil || !bytes.Equal(part, big[130000:131000]) {
		t.Errorf("part of big.bin: %v", err)
	}
}
