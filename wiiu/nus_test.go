package wiiu

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

// makeNUS writes an encrypted title (made up, with a made-up common key)
// to dir: code holds files[code/...] in content 1 (plain CBC), and content
// holds the rest in content 2 (hashed).
func makeNUS(t *testing.T, dir string, common, titleKey [16]byte, titleID uint64, files map[string][]byte, order []string) {
	t.Helper()
	// the file table: root, code/, content/, files
	type ent struct {
		typ          byte
		name         string
		a, b         uint32
		flags, clust uint16
	}
	var ents []ent
	var names []byte
	addName := func(s string) uint32 {
		o := uint32(len(names))
		names = append(append(names, s...), 0)
		return o
	}
	addName("")
	data := [3][]byte{}
	ents = append(ents, ent{typ: 1})
	for _, folder := range []string{"code", "content"} {
		di := len(ents)
		ents = append(ents, ent{typ: 1, name: folder})
		cl := uint16(1)
		if folder == "content" {
			cl = 2
		}
		for _, p := range order {
			if filepath.Dir(p) != folder {
				continue
			}
			b := files[p]
			for len(data[cl])%0x20 != 0 {
				data[cl] = append(data[cl], 0)
			}
			off := uint32(len(data[cl]) / 0x20)
			data[cl] = append(data[cl], b...)
			ents = append(ents, ent{typ: 0, name: filepath.Base(p), a: off, b: uint32(len(b)), clust: cl})
		}
		ents[di].b = uint32(len(ents))
	}
	ents[0].b = uint32(len(ents))
	fst := []byte("FST\x00")
	fst = binary.BigEndian.AppendUint32(fst, 0x20)
	fst = binary.BigEndian.AppendUint32(fst, 3)
	fst = append(fst, make([]byte, 0x14)...)
	fst = append(fst, make([]byte, 3*0x20)...)
	for _, e := range ents {
		no := addName(e.name)
		if e.name == "" {
			no = 0
		}
		fst = binary.BigEndian.AppendUint32(fst, uint32(e.typ)<<24|no)
		fst = binary.BigEndian.AppendUint32(fst, e.a)
		fst = binary.BigEndian.AppendUint32(fst, e.b)
		fst = binary.BigEndian.AppendUint16(fst, e.flags)
		fst = binary.BigEndian.AppendUint16(fst, e.clust)
	}
	fst = append(fst, names...)
	data[0] = fst
	tc, _ := aes.NewCipher(titleKey[:])
	pad := func(b []byte, n int) []byte {
		for len(b)%n != 0 {
			b = append(b, 0)
		}
		return b
	}
	types := []uint16{1, 1, 3}
	var sizes [3]uint64
	for i := range data {
		var enc []byte
		if types[i]&2 == 0 {
			plain := pad(append([]byte(nil), data[i]...), 16)
			var iv [16]byte
			binary.BigEndian.PutUint16(iv[:], uint16(i))
			enc = make([]byte, len(plain))
			cipher.NewCBCEncrypter(tc, iv[:]).CryptBlocks(enc, plain)
		} else {
			plain := pad(append([]byte(nil), data[i]...), 0xfc00)
			for b := 0; b*0xfc00 < len(plain); b++ {
				chunk := plain[b*0xfc00 : (b+1)*0xfc00]
				hashes := make([]byte, 0x400)
				// only H0 matters here: the hash of each 0x400 piece
				for k := 0; k < 16; k++ {
					h := sha1.Sum(chunk[k*0x400 : (k+1)*0x400])
					copy(hashes[k*20:], h[:])
				}
				var zero [16]byte
				eh := make([]byte, 0x400)
				cipher.NewCBCEncrypter(tc, zero[:]).CryptBlocks(eh, hashes)
				ed := make([]byte, 0xfc00)
				cipher.NewCBCEncrypter(tc, hashes[(b%16)*20:][:16]).CryptBlocks(ed, chunk)
				enc = append(append(enc, eh...), ed...)
			}
		}
		sizes[i] = uint64(len(enc))
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("%08x.app", 0x10+i)), enc, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	// title.tmd
	tmd := binary.BigEndian.AppendUint32(nil, 0x10004)
	tmd = append(tmd, make([]byte, 0x140-4+0x9c4)...)
	o := 0x140
	binary.BigEndian.PutUint64(tmd[o+0x4c:], titleID)
	binary.BigEndian.PutUint16(tmd[o+0x9e:], 3)
	for i := 0; i < 3; i++ {
		r := make([]byte, 0x30)
		binary.BigEndian.PutUint32(r, uint32(0x10+i))
		binary.BigEndian.PutUint16(r[4:], uint16(i))
		binary.BigEndian.PutUint16(r[6:], types[i])
		binary.BigEndian.PutUint64(r[8:], sizes[i])
		tmd = append(tmd, r...)
	}
	os.WriteFile(filepath.Join(dir, "title.tmd"), tmd, 0o644)
	// title.tik
	tik := binary.BigEndian.AppendUint32(nil, 0x10004)
	tik = append(tik, make([]byte, 0x140-4+0x100)...)
	cc, _ := aes.NewCipher(common[:])
	var iv [16]byte
	binary.BigEndian.PutUint64(iv[:], titleID)
	cipher.NewCBCEncrypter(cc, iv[:]).CryptBlocks(tik[o+0x7f:o+0x8f], titleKey[:])
	binary.BigEndian.PutUint64(tik[o+0x9c:], titleID)
	os.WriteFile(filepath.Join(dir, "title.tik"), tik, 0o644)
}

func TestNUS(t *testing.T) {
	dir := t.TempDir()
	common := [16]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
	titleKey := [16]byte{0xaa, 0xbb, 0xcc, 1, 2, 3}
	big := make([]byte, 300000)
	for i := range big {
		big[i] = byte(i * 7 / 3)
	}
	files := map[string][]byte{
		"code/app.xml":      []byte("<app><title_id type=\"hexBinary\" length=\"8\">0005000010ABCD00</title_id></app>"),
		"code/cos.xml":      []byte("<app><argstr type=\"string\" length=\"4096\">game.rpx</argstr></app>"),
		"code/game.rpx":     []byte("not really an rpx, but the bytes come back"),
		"content/big.bin":   big,
		"content/Small.TXT": []byte("hello"),
		"content/empty.dat": nil,
	}
	order := []string{"code/app.xml", "code/cos.xml", "code/game.rpx", "content/Small.TXT", "content/big.bin", "content/empty.dat"}
	makeNUS(t, dir, common, titleKey, 0x0005000010abcd00, files, order)

	if _, _, _, err := TitleFromNUS(dir); !errors.Is(err, ErrNoKeys) {
		t.Fatalf("without keys: %v", err)
	}
	keyDir := t.TempDir()
	os.WriteFile(filepath.Join(keyDir, "keys.txt"), []byte("# my keys\n"+
		"00112233445566778899aabbccddeeff # something else\n"+hex.EncodeToString(common[:])+" # common\n"), 0o644)
	rpx, name, fsys, err := TitleFromNUS(dir, keyDir)
	if err != nil {
		t.Fatal(err)
	}
	if name != "game.rpx" || !bytes.Equal(rpx, files["code/game.rpx"]) {
		t.Fatalf("program %q: %q", name, rpx)
	}
	if fsys.TitleID != 0x0005000010abcd00 {
		t.Errorf("title ID %x", fsys.TitleID)
	}
	for _, p := range order[3:] {
		b, err := fsys.ReadFile("/vol/" + p)
		if err != nil {
			t.Fatalf("%s: %v", p, err)
		}
		if !bytes.Equal(b, files[p]) {
			t.Errorf("%s: %d bytes, not what was written", p, len(b))
		}
	}
	if b, err := fsys.ReadFile("/vol/content/small.txt"); err != nil || string(b) != "hello" {
		t.Errorf("case-insensitive read: %q %v", b, err)
	}
	ents, err := fsys.ReadDir("/vol/content")
	if err != nil || len(ents) != 3 {
		t.Fatalf("listing: %v %v", ents, err)
	}
	if size, dir, err := fsys.Stat("/vol/content/big.bin"); err != nil || dir || size != int64(len(big)) {
		t.Errorf("stat: %d %v %v", size, dir, err)
	}
}
