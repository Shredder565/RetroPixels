// Tests for the 7z extractor. Archives come from three places: a small
// writer in helpers_test.go, independent compressors (Python's lzma module,
// py7zr, xz) when installed, and two small fixtures in testdata.

package sevenzip

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"hash/crc32"
	"io"
	"io/fs"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"time"
)

// sampleFiles returns a mix of compressible, random and repetitive data.
func sampleFiles(seed int64) [][]byte {
	r := rand.New(rand.NewSource(seed))
	words := []string{"dolphin ", "gamecube ", "wii ", "retro\n", "pixels ", "emulator "}
	var text bytes.Buffer
	for text.Len() < 200000 {
		text.WriteString(words[r.Intn(len(words))])
	}
	random := make([]byte, 70000)
	r.Read(random)
	return [][]byte{text.Bytes(), random, bytes.Repeat([]byte{0xAB}, 5000), []byte("x")}
}

// x86Sample returns real x86-64 machine code: the test binary itself on
// amd64, plus $SEVENZIP_TEST_EXE if set, else synthetic code-like bytes.
func x86Sample(t *testing.T) []byte {
	var out []byte
	if p := os.Getenv("SEVENZIP_TEST_EXE"); p != "" {
		b, err := os.ReadFile(p)
		if err != nil {
			t.Fatal(err)
		}
		out = append(out, b...)
	}
	if exe, err := os.Executable(); err == nil {
		if b, err := os.ReadFile(exe); err == nil {
			if len(b) > 4<<20 {
				b = b[:4<<20]
			}
			out = append(out, b...)
		}
	}
	// Synthetic: random bytes with dense CALL/JMP/Jcc patterns.
	r := rand.New(rand.NewSource(3))
	for i := 0; i < 200000; i++ {
		switch r.Intn(6) {
		case 0:
			out = append(out, 0xE8, byte(r.Intn(256)), byte(r.Intn(256)), 0, 0)
		case 1:
			out = append(out, 0xE9, byte(r.Intn(256)), 0xFF, 0xFF, 0xFF)
		case 2:
			out = append(out, 0x0F, 0x80|byte(r.Intn(16)), byte(r.Intn(256)), 0, 0, 0)
		case 3:
			out = append(out, 0xE8, 0xE8, 0xE9, 0, 0xFF, 0xE8)
		default:
			out = append(out, byte(r.Intn(256)))
		}
	}
	return out
}

// checkTree compares every file under got with want (relative path -> data).
func checkTree(t *testing.T, got string, want map[string][]byte) {
	t.Helper()
	seen := map[string]bool{}
	filepath.WalkDir(got, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			t.Error(err)
			return nil
		}
		rel, _ := filepath.Rel(got, p)
		rel = filepath.ToSlash(rel)
		if d.IsDir() {
			return nil
		}
		if strings.Contains(d.Name(), ".rp7z-") {
			t.Errorf("leftover temporary file %s", rel)
		}
		data, err := os.ReadFile(p)
		if err != nil {
			t.Error(err)
			return nil
		}
		w, ok := want[rel]
		if !ok {
			t.Errorf("unexpected file %s", rel)
		} else if !bytes.Equal(data, w) {
			t.Errorf("%s: content differs (got %d bytes, want %d)", rel, len(data), len(w))
		}
		seen[rel] = true
		return nil
	})
	for k := range want {
		if !seen[k] {
			t.Errorf("missing file %s", k)
		}
	}
}

func ft(t time.Time) uint64 { return uint64(t.UnixNano()/100) + 116444736000000000 }

func TestSafePath(t *testing.T) {
	dest := t.TempDir()
	good := map[string]string{
		"a.txt":        "a.txt",
		"dir/b.txt":    "dir/b.txt",
		`dir\win.txt`:  "dir/win.txt",
		"./x/./y":      "x/y",
		"a..b/c":       "a..b/c",
		"日本語/ファイル.txt": "日本語/ファイル.txt",
	}
	for in, want := range good {
		p, err := safePath(dest, in)
		if err != nil {
			t.Errorf("%q: %v", in, err)
			continue
		}
		if p != filepath.Join(dest, filepath.FromSlash(want)) {
			t.Errorf("%q -> %q", in, p)
		}
	}
	for _, in := range []string{"../x", "a/../../x", "a/..", `..\x`, "/etc/passwd", `\x`, `C:\x`, "C:x", "a/...", "a/. .", "x\x00y", "", "a/b:stream"} {
		if _, err := safePath(dest, in); err == nil {
			t.Errorf("%q was accepted", in)
		}
	}
}

func TestCopyArchive(t *testing.T) {
	files := sampleFiles(1)
	mt := time.Date(2024, 5, 6, 7, 8, 9, 0, time.UTC)
	for _, encode := range []bool{false, true} {
		entries := []tentry{
			{name: "docs", dir: true, mtime: ft(mt)},
			{name: "docs/text.txt", mtime: ft(mt)},
			{name: "bin/random.bin", mtime: ft(mt)},
			{name: "empty.txt", empty: true, mtime: ft(mt)},
			{name: "Sys/Ünïcødé 🎮.dat", mtime: ft(mt)},
			{name: `win\style\path.txt`, mtime: ft(mt)},
			{name: "deep/empty/dir", dir: true, mtime: ft(mt)},
		}
		// Two folders: one solid with three files, one with a single file.
		ar := build7z([]tfolder{copyFolder(files[0], files[1], files[2]), copyFolder(files[3])}, entries, encode)
		path := writeArchive(t, ar)
		dest := t.TempDir()
		var last, tot int64
		if err := ExtractAll(path, dest, func(d, n int64) { last, tot = d, n }); err != nil {
			t.Fatal(err)
		}
		checkTree(t, dest, map[string][]byte{
			"docs/text.txt":      files[0],
			"bin/random.bin":     files[1],
			"empty.txt":          {},
			"Sys/Ünïcødé 🎮.dat":  files[2],
			"win/style/path.txt": files[3],
		})
		if want := int64(len(files[0]) + len(files[1]) + len(files[2]) + len(files[3])); last != want || tot != want {
			t.Errorf("progress ended at %d/%d, want %d", last, tot, want)
		}
		for _, p := range []string{"docs/text.txt", "docs", "empty.txt"} {
			st, err := os.Stat(filepath.Join(dest, p))
			if err != nil || !st.ModTime().Equal(mt) {
				t.Errorf("%s: mtime %v, want %v (%v)", p, st.ModTime(), mt, err)
			}
		}
		if st, err := os.Stat(filepath.Join(dest, "deep/empty/dir")); err != nil || !st.IsDir() {
			t.Errorf("empty dir not created: %v", err)
		}
		list, err := List(path)
		if err != nil || len(list) != len(entries) || list[5].Name != "win/style/path.txt" || list[1].Size != uint64(len(files[0])) || !list[0].IsDir {
			t.Errorf("List: %v %+v", err, list)
		}
	}
}

func TestEmptyArchive(t *testing.T) {
	path := writeArchive(t, build7z(nil, []tentry{{name: "only", dir: true}, {name: "only/e", empty: true}}, true))
	dest := t.TempDir()
	if err := ExtractAll(path, dest, nil); err != nil {
		t.Fatal(err)
	}
	checkTree(t, dest, map[string][]byte{"only/e": {}})
}

func TestUnsafeArchive(t *testing.T) {
	for _, name := range []string{"../evil.txt", `..\evil.txt`, "/abs.txt", `C:\evil.txt`, "ok/../../evil.txt"} {
		ar := build7z([]tfolder{copyFolder([]byte("a"), []byte("b"))}, []tentry{{name: "fine.txt"}, {name: name}}, false)
		dir := t.TempDir()
		dest := filepath.Join(dir, "out")
		err := ExtractAll(writeArchive(t, ar), dest, nil)
		if err == nil || !strings.Contains(err.Error(), "unsafe path") {
			t.Errorf("%q: got %v", name, err)
		}
		// Nothing may be written, not even the safe file.
		if _, err := os.Stat(filepath.Join(dest, "fine.txt")); err == nil {
			t.Errorf("%q: files were written before refusing", name)
		}
		if _, err := os.Stat(filepath.Join(dir, "evil.txt")); err == nil {
			t.Errorf("%q: escaped", name)
		}
	}
}

func TestCorruptDataLeavesNoFile(t *testing.T) {
	files := sampleFiles(2)
	ar := build7z([]tfolder{copyFolder(files[0], files[1])}, []tentry{{name: "a.txt"}, {name: "b.bin"}}, false)
	ar[32+len(files[0])+100] ^= 0x55 // damage the second file
	dest := t.TempDir()
	err := ExtractAll(writeArchive(t, ar), dest, nil)
	if err == nil || !strings.Contains(err.Error(), "checksum") {
		t.Fatalf("got %v", err)
	}
	checkTree(t, dest, map[string][]byte{"a.txt": files[0]})

	// A truncated archive fails cleanly too.
	ar = build7z([]tfolder{copyFolder(files[0])}, []tentry{{name: "a.txt"}}, false)
	if err := ExtractAll(writeArchive(t, ar[:len(ar)/2]), t.TempDir(), nil); err == nil {
		t.Error("truncated archive accepted")
	}
	if _, err := List(writeArchive(t, []byte("not a 7z file at all, just text"))); err == nil {
		t.Error("non-7z accepted")
	}
}

func TestUnsupportedMethods(t *testing.T) {
	f := copyFolder([]byte("abc"))
	f.coders[0].id = []byte{0x03, 0x04, 0x01}
	err := ExtractAll(writeArchive(t, build7z([]tfolder{f}, []tentry{{name: "a"}}, false)), t.TempDir(), nil)
	if err == nil || !strings.Contains(err.Error(), "compression method RetroPixels can't unpack: PPMd (030401)") {
		t.Errorf("PPMd: %v", err)
	}
	f.coders[0].id = []byte{0x06, 0xF1, 0x07, 0x01}
	err = ExtractAll(writeArchive(t, build7z([]tfolder{f}, []tentry{{name: "a"}}, false)), t.TempDir(), nil)
	if !errors.Is(err, ErrEncrypted) {
		t.Errorf("AES: %v", err)
	}
}

// TestBCJ2 checks the BCJ2 decoder against the independent test encoder,
// inside a real four-stream folder like the ones 7-Zip writes.
func TestBCJ2(t *testing.T) {
	code := x86Sample(t)
	main, call, jump, rc := bcj2Encode(code)
	if len(call) == 0 || len(jump) == 0 {
		t.Fatal("sample has no converted branches")
	}
	t.Logf("BCJ2 sample %d bytes: main %d, call %d, jump %d, rc %d", len(code), len(main), len(call), len(jump), len(rc))

	build := func(codec []byte, props []byte, pack func([]byte) []byte) []byte {
		bcj2 := coderInfo{id: []byte{0x03, 0x03, 0x01, 0x1B}, numIn: 4, numOut: 1}
		c := func() coderInfo { return coderInfo{id: codec, numIn: 1, numOut: 1, props: props} }
		f := tfolder{
			// Coder 0 is BCJ2 (in-streams 0-3), coders 1-3 decode main,
			// call and jump (in-streams 4-6); the range-coded stream is
			// stored as is.
			coders:  []coderInfo{bcj2, c(), c(), c()},
			bind:    []bindPair{{in: 0, out: 1}, {in: 1, out: 2}, {in: 2, out: 3}},
			packed:  []int{4, 5, 6, 3},
			packs:   [][]byte{pack(main), pack(call), pack(jump), rc},
			sizes:   []uint64{uint64(len(code)), uint64(len(main)), uint64(len(call)), uint64(len(jump))},
			streams: [][]byte{code[:1000], code[1000:]},
		}
		return build7z([]tfolder{f}, []tentry{{name: "head.bin"}, {name: "prog.exe"}}, true)
	}
	run := func(name string, ar []byte) {
		dest := t.TempDir()
		if err := ExtractAll(writeArchive(t, ar), dest, nil); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		checkTree(t, dest, map[string][]byte{"head.bin": code[:1000], "prog.exe": code[1000:]})
	}
	run("copy", build([]byte{0}, nil, func(b []byte) []byte { return b }))

	if python() != "" {
		lz := func(b []byte) []byte {
			return pyRaw(t, "[{'id':lzma.FILTER_LZMA1,'dict_size':1<<20,'lc':3,'lp':0,'pb':2}]", b)
		}
		run("lzma", build([]byte{3, 1, 1}, []byte{0x5D, 0, 0, 0x10, 0}, lz))
	}
}

// TestBCJ2Edges covers branch opcodes at the very end of the data.
func TestBCJ2Edges(t *testing.T) {
	for _, code := range [][]byte{
		{0xE8},
		{0x90, 0xE8, 1, 2, 3},
		{0xE8, 1, 0, 0, 0},
		{0xE8, 1, 0, 0},
		{0x0F, 0x85, 0x10, 0, 0, 0, 0xE9, 0xF0, 0xFF, 0xFF, 0xFF, 0xE8},
	} {
		main, call, jump, rc := bcj2Encode(code)
		r, err := newBCJ2Reader([]io.Reader{bytes.NewReader(main), bytes.NewReader(call), bytes.NewReader(jump), bytes.NewReader(rc)}, int64(len(code)))
		if err != nil {
			t.Fatal(err)
		}
		got, err := io.ReadAll(r)
		if err != nil || !bytes.Equal(got, code) {
			t.Errorf("% x: got % x, %v", code, got, err)
		}
	}
}

// TestLZMAWithPython decodes raw LZMA and LZMA2 made by liblzma, with
// several lc/lp/pb settings and a dictionary smaller than the data so the
// window wraps.
func TestLZMAWithPython(t *testing.T) {
	if python() == "" {
		t.Skip("python3 not found")
	}
	files := sampleFiles(5)
	data := bytes.Join(append(files, x86Sample(t)[:500000], files[0]), nil)
	for _, c := range []struct{ lc, lp, pb, dict int }{{3, 0, 2, 1 << 16}, {0, 2, 0, 1 << 20}, {4, 0, 4, 4096}, {1, 3, 1, 1 << 24}} {
		filt := func(id string) string {
			return "[{'id':lzma." + id + ",'dict_size':" + itoa(c.dict) + ",'lc':" + itoa(c.lc) + ",'lp':" + itoa(c.lp) + ",'pb':" + itoa(c.pb) + "}]"
		}
		props := []byte{byte((c.pb*5+c.lp)*9 + c.lc), 0, 0, 0, 0}
		binary.LittleEndian.PutUint32(props[1:], uint32(c.dict))
		ar := build7z([]tfolder{oneCoder([]byte{3, 1, 1}, props, pyRaw(t, filt("FILTER_LZMA1"), data), files[0], data[len(files[0]):])},
			[]tentry{{name: "a"}, {name: "b"}}, false)
		dest := t.TempDir()
		if err := ExtractAll(writeArchive(t, ar), dest, nil); err != nil {
			t.Fatalf("lzma %+v: %v", c, err)
		}
		checkTree(t, dest, map[string][]byte{"a": files[0], "b": data[len(files[0]):]})

		if c.lc+c.lp <= 4 {
			ar = build7z([]tfolder{oneCoder([]byte{0x21}, []byte{lzma2Props(c.dict)}, pyRaw(t, filt("FILTER_LZMA2"), data), data)},
				[]tentry{{name: "all"}}, true)
			dest = t.TempDir()
			if err := ExtractAll(writeArchive(t, ar), dest, nil); err != nil {
				t.Fatalf("lzma2 %+v: %v", c, err)
			}
			checkTree(t, dest, map[string][]byte{"all": data})
		}
	}
}

// TestFiltersWithPython checks x86 BCJ and Delta against liblzma's encoders.
func TestFiltersWithPython(t *testing.T) {
	if python() == "" {
		t.Skip("python3 not found")
	}
	code := x86Sample(t)
	lz2 := "{'id':lzma.FILTER_LZMA2,'dict_size':1<<22}"
	ar := build7z([]tfolder{filterFolder([]byte{3, 3, 1, 3}, nil, []byte{0x21}, []byte{lzma2Props(1 << 22)},
		pyRaw(t, "[{'id':lzma.FILTER_X86},"+lz2+"]", code), code[:12345], code[12345:])},
		[]tentry{{name: "a.exe"}, {name: "b.exe"}}, false)
	dest := t.TempDir()
	if err := ExtractAll(writeArchive(t, ar), dest, nil); err != nil {
		t.Fatalf("bcj: %v", err)
	}
	checkTree(t, dest, map[string][]byte{"a.exe": code[:12345], "b.exe": code[12345:]})

	var audio []byte
	for i := 0; i < 300000; i++ {
		audio = append(audio, byte(i*7/3), byte(i/50), byte(i%13))
	}
	for _, dist := range []int{1, 3, 256} {
		ar := build7z([]tfolder{filterFolder([]byte{3}, []byte{byte(dist - 1)}, []byte{0x21}, []byte{lzma2Props(1 << 22)},
			pyRaw(t, "[{'id':lzma.FILTER_DELTA,'dist':"+itoa(dist)+"},"+lz2+"]", audio), audio)},
			[]tentry{{name: "audio.raw"}}, false)
		dest := t.TempDir()
		if err := ExtractAll(writeArchive(t, ar), dest, nil); err != nil {
			t.Fatalf("delta %d: %v", dist, err)
		}
		checkTree(t, dest, map[string][]byte{"audio.raw": audio})
	}
}

// TestARM64WithXZ checks the ARM64 filter against xz's encoder, which uses
// the same filter definition as 7-Zip.
func TestARM64WithXZ(t *testing.T) {
	xz, err := exec.LookPath("xz")
	if err != nil || exec.Command(xz, "--format=raw", "--arm64", "--lzma2", "-c", os.DevNull).Run() != nil {
		t.Skip("xz with ARM64 support not found")
	}
	r := rand.New(rand.NewSource(9))
	var code []byte
	for i := 0; i < 400000; i++ {
		var ins uint32
		switch r.Intn(4) {
		case 0:
			ins = 0x94000000 | r.Uint32()&0x03FFFFFF // BL
		case 1:
			ins = 0x90000000 | r.Uint32()&0x60FFFFFF // ADRP, any range
		case 2:
			// ADRP within +-512 MiB, the converted range
			imm := uint32(r.Intn(1<<18)) - 1<<17
			ins = 0x90000000 | (imm&3)<<29 | (imm>>2&0x7FFFF)<<5 | uint32(r.Intn(32))
		default:
			ins = r.Uint32()
		}
		code = binary.LittleEndian.AppendUint32(code, ins)
	}
	code = append(code, 0x94, 1, 2) // unaligned tail
	cmd := exec.Command(xz, "--format=raw", "--arm64", "--lzma2=dict=1MiB", "-c")
	cmd.Stdin = bytes.NewReader(code)
	packed, err := cmd.Output()
	if err != nil {
		t.Fatal(err)
	}
	ar := build7z([]tfolder{filterFolder([]byte{0x0A}, nil, []byte{0x21}, []byte{lzma2Props(1 << 20)}, packed, code)},
		[]tentry{{name: "arm64.bin"}}, false)
	dest := t.TempDir()
	if err := ExtractAll(writeArchive(t, ar), dest, nil); err != nil {
		t.Fatal(err)
	}
	checkTree(t, dest, map[string][]byte{"arm64.bin": code})
}

// TestDamagedHeaders feeds randomly damaged headers (with fixed-up
// checksums) through ExtractAll; it must fail cleanly, never panic.
func TestDamagedHeaders(t *testing.T) {
	code := x86Sample(t)[:100000]
	main, call, jump, rc := bcj2Encode(code)
	bcj2 := coderInfo{id: []byte{0x03, 0x03, 0x01, 0x1B}, numIn: 4, numOut: 1}
	c := coderInfo{id: []byte{0}, numIn: 1, numOut: 1}
	f := tfolder{
		coders:  []coderInfo{bcj2, c, c, c},
		bind:    []bindPair{{in: 0, out: 1}, {in: 1, out: 2}, {in: 2, out: 3}},
		packed:  []int{4, 5, 6, 3},
		packs:   [][]byte{main, call, jump, rc},
		sizes:   []uint64{uint64(len(code)), uint64(len(main)), uint64(len(call)), uint64(len(jump))},
		streams: [][]byte{code[:1000], code[1000:]},
	}
	files := sampleFiles(1)
	orig := build7z([]tfolder{f, copyFolder(files[0], files[3])}, []tentry{{name: "d", dir: true}, {name: "a"}, {name: "e", empty: true}, {name: "b"}, {name: "c"}, {name: "x"}}, false)
	hsize := int(binary.LittleEndian.Uint64(orig[20:]))
	hoff := 32 + int(binary.LittleEndian.Uint64(orig[12:]))
	r := rand.New(rand.NewSource(2))
	dir := t.TempDir()
	if err := ExtractAll(writeArchive(t, orig), dir, nil); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2000; i++ {
		b := append([]byte(nil), orig...)
		for k := 1 + r.Intn(3); k > 0; k-- {
			p := hoff + r.Intn(hsize)
			switch r.Intn(3) {
			case 0:
				b[p] ^= byte(1 << uint(r.Intn(8)))
			case 1:
				b[p] = byte(r.Intn(256))
			default:
				b[p] = []byte{0, 1, 0xFF, 0x80, 0x7F}[r.Intn(5)]
			}
		}
		binary.LittleEndian.PutUint32(b[28:], crc32.ChecksumIEEE(b[hoff:hoff+hsize]))
		binary.LittleEndian.PutUint32(b[8:], crc32.ChecksumIEEE(b[12:32]))
		p := filepath.Join(dir, "m.7z")
		os.WriteFile(p, b, 0o644)
		func() {
			defer func() {
				if e := recover(); e != nil {
					t.Fatalf("panic %v", e)
				}
			}()
			ExtractAll(p, filepath.Join(dir, "out"), nil)
		}()
	}
}

// The testdata archives were made with py7zr 1.1.4 (LZMA2 preset 9, and
// BCJ + LZMA), both with compressed headers. code/x86.bin is 48 KiB of a
// Windows build of RetroPixels. These hashes are of the original files.
var fixtureFiles = map[string]string{
	"readme.txt":   "9febadb65c9782106086a397e3d5073e910f7d8f3d517d9b0c611cd0ef159247",
	"empty.txt":    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	"random.bin":   "c08d6b1ad504249e07990d447da98a49f2e4302eafc76c8d8aa02d66d2b092b2",
	"日本語/ファイル.txt": "6800dd0a198915e4ac71dac4f597bb53e29f928fa1488b31ef964f9fb155ab72",
	"code/x86.bin": "b9d83cee4ac368a641c72f50e03d1875e100a7402ee15c3787f001965d26b221",
}

func TestFixtures(t *testing.T) {
	for _, name := range []string{"lzma2.7z", "bcj_lzma.7z"} {
		dest := t.TempDir()
		if err := ExtractAll(filepath.Join("testdata", name), dest, nil); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		for rel, want := range fixtureFiles {
			b, err := os.ReadFile(filepath.Join(dest, filepath.FromSlash(rel)))
			sum := sha256.Sum256(b)
			if err != nil || hex.EncodeToString(sum[:]) != want {
				t.Errorf("%s: %s: %v sha256 %x", name, rel, err, sum)
			}
		}
		if st, err := os.Stat(filepath.Join(dest, "emptydir")); err != nil || !st.IsDir() {
			t.Errorf("%s: emptydir missing", name)
		}
	}
}

// TestPy7zr builds archives with py7zr, an independent 7z implementation, and
// compares extraction byte for byte. Set SEVENZIP_BIG=1 for a 40 MB run that
// also reports speed.
func TestPy7zr(t *testing.T) {
	py := py7zrPython()
	if py == "" {
		t.Skip("py7zr not installed (set SEVENZIP_PYTHON to a python that has it)")
	}
	work := t.TempDir()
	script := filepath.Join(t.TempDir(), "gen.py")
	os.WriteFile(script, []byte(py7zrScript), 0o644)
	exe := filepath.Join(work, "x86.bin")
	os.WriteFile(exe, x86Sample(t), 0o644)
	big := "1000000"
	if os.Getenv("SEVENZIP_BIG") != "" {
		big = "40000000"
	}
	cmd := exec.Command(py, "-I", script, work, big, exe)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("py7zr: %v\n%s", err, out)
	}
	want := map[string][]byte{}
	src := filepath.Join(work, "src")
	filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			rel, _ := filepath.Rel(src, p)
			want[filepath.ToSlash(rel)], _ = os.ReadFile(p)
		}
		return nil
	})
	archives, _ := filepath.Glob(filepath.Join(work, "*.7z"))
	if len(archives) < 9 {
		t.Fatalf("py7zr made only %d archives", len(archives))
	}
	for _, a := range archives {
		name := filepath.Base(a)
		dest := t.TempDir()
		start := time.Now()
		err := ExtractAll(a, dest, nil)
		if name == "aes.7z" {
			if !errors.Is(err, ErrEncrypted) {
				t.Errorf("aes.7z: got %v", err)
			}
			continue
		}
		if err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		var total int
		for _, b := range want {
			total += len(b)
		}
		el := time.Since(start)
		t.Logf("%-16s %6.1f MB in %v (%.0f MB/s)", name, float64(total)/1e6, el.Round(time.Millisecond), float64(total)/1e6/el.Seconds())
		checkTree(t, dest, want)
		if st, err := os.Stat(filepath.Join(dest, "emptydir", "nested")); err != nil || !st.IsDir() {
			t.Errorf("%s: empty dir missing", name)
		}
	}
}

// TestReal7Zip makes archives with a real 7-Zip (7zz, 7z or 7za on the PATH,
// or $SEVENZIP_7Z) using the settings people use, including -mx9, which picks
// BCJ2 for .exe and .dll files.
func TestReal7Zip(t *testing.T) {
	sz := os.Getenv("SEVENZIP_7Z")
	for _, n := range []string{"7zz", "7z", "7za"} {
		if sz == "" {
			sz, _ = exec.LookPath(n)
		}
	}
	if sz == "" {
		t.Skip("7-Zip not found (set SEVENZIP_7Z)")
	}
	// p7zip 16.02 (Ubuntu 22.04's 7z) has no ARM64 filter and keeps only
	// whole seconds of a file's time; the test needs 7-Zip 21 or later.
	banner, _ := exec.Command(sz).CombinedOutput()
	if m := regexp.MustCompile(`7-Zip[^\n]*? (\d+)\.\d+`).FindSubmatch(banner); m != nil {
		if v, _ := strconv.Atoi(string(m[1])); v < 21 {
			t.Skipf("%s is 7-Zip %s; this test needs 7-Zip 21 or later (set SEVENZIP_7Z)", sz, m[1])
		}
	}
	work := t.TempDir()
	tree := filepath.Join(work, "Dolphin-x64")
	files := sampleFiles(8)
	code := x86Sample(t)
	want := map[string][]byte{
		"Dolphin-x64/Dolphin.exe":                 code,
		"Dolphin-x64/Qt6Core.dll":                 code[len(code)/3:],
		"Dolphin-x64/Sys/GameSettings/GALE01.ini": files[0],
		"Dolphin-x64/Sys/random.bin":              files[1],
		"Dolphin-x64/Sys/empty.txt":               {},
		"Dolphin-x64/Languages/日本語/ファイル 🎮.txt":    files[3],
	}
	mt := time.Date(2025, 6, 1, 12, 30, 45, 123456700, time.UTC)
	for rel, data := range want {
		p := filepath.Join(work, filepath.FromSlash(rel))
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, data, 0o644)
		os.Chtimes(p, mt, mt)
	}
	os.MkdirAll(filepath.Join(tree, "User", "Empty"), 0o755)

	cases := []struct {
		name string
		args string
		fail string
	}{
		{"mx9", "-mx9", ""},
		{"bcj2", "-mx9 -m0=BCJ2 -m1=LZMA2:d=64m -m2=LZMA:d=1m -m3=LZMA:d=1m -mb0:1 -mb0s1:2 -mb0s2:3", ""},
		{"mx5", "-mx5", ""},
		{"mx1mt", "-mx1 -mmt=4", ""},
		{"lzma2chunks", "-m0=LZMA2:d=1m:c=256k -mmt=4", ""},
		{"nonsolid", "-mx7 -ms=off", ""},
		{"plainhdr", "-mx5 -mhc=off", ""},
		{"lzma", "-m0=LZMA:d=64k", ""},
		{"bcj", "-m0=BCJ -m1=LZMA:lc=0:lp=2:pb=1", ""},
		{"arm64", "-m0=ARM64 -m1=LZMA2", ""},
		{"delta", "-m0=Delta:4 -m1=LZMA", ""},
		{"copy", "-mx0", ""},
		{"deflate", "-m0=Deflate", ""},
		{"bzip2", "-m0=BZip2", ""},
		{"ppmd", "-m0=PPMd", "PPMd"},
		{"aes", "-psecret", "password"},
		{"aeshdr", "-psecret -mhe=on", "password"},
	}
	for _, c := range cases {
		ar := filepath.Join(work, c.name+".7z")
		args := append([]string{"a", "-bd", "-y"}, strings.Fields(c.args)...)
		cmd := exec.Command(sz, append(args, ar, tree)...)
		cmd.Dir = work
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("%s: 7-Zip: %v\n%s", c.name, err, out)
		}
		dest := t.TempDir()
		start := time.Now()
		err := ExtractAll(ar, dest, nil)
		if c.fail != "" {
			if err == nil || !strings.Contains(err.Error(), c.fail) {
				t.Errorf("%s: got %v, want an error about %s", c.name, err, c.fail)
			}
			continue
		}
		if err != nil {
			t.Errorf("%s: %v", c.name, err)
			continue
		}
		t.Logf("%-12s %v", c.name, time.Since(start).Round(time.Millisecond))
		checkTree(t, dest, want)
		if st, err := os.Stat(filepath.Join(dest, "Dolphin-x64", "User", "Empty")); err != nil || !st.IsDir() {
			t.Errorf("%s: empty folder missing", c.name)
		}
		st, err := os.Stat(filepath.Join(dest, "Dolphin-x64", "Dolphin.exe"))
		if err != nil || !st.ModTime().Equal(mt) {
			t.Errorf("%s: mtime %v, want %v", c.name, st.ModTime(), mt)
		}
	}
}

const py7zrScript = `
import os, sys, random, math, py7zr
out, big, exe = sys.argv[1], int(sys.argv[2]), sys.argv[3]
src = os.path.join(out, "src")
rnd = random.Random(7)
def w(rel, data):
    p = os.path.join(src, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)
words = [b"dolphin", b"gamecube", b"wii", b"retro", b"pixel", b"\n", b" ", b"emulator"]
w("readme.txt", b"".join(rnd.choice(words) for _ in range(20000)))
w("empty.bin", b"")
os.makedirs(os.path.join(src, "emptydir", "nested"), exist_ok=True)
w("sub/dir/random.bin", rnd.randbytes(300000))
w("日本語/ファイル.txt", "こんにちは".encode() * 1000)
w("émoji 🎮.txt", b"x" * 12345)
w("tiny.txt", b"a")
mix = bytearray()
while len(mix) < big:
    if rnd.random() < 0.5: mix += rnd.randbytes(rnd.randrange(1, 5000))
    else: mix += b"".join(rnd.choice(words) for _ in range(rnd.randrange(1, 2000)))
w("big/mixed.bin", bytes(mix[:big]))
w("audio.raw", b"".join(int(1000*math.sin(i/30)).to_bytes(2, "little", signed=True) for i in range(100000)))
w("prog.exe", open(exe, "rb").read())
F = py7zr
sets = {
  "lzma2": [{"id": F.FILTER_LZMA2, "preset": 6}],
  "lzma": [{"id": F.FILTER_LZMA, "preset": 6}],
  "bcj_lzma2": [{"id": F.FILTER_X86}, {"id": F.FILTER_LZMA2, "preset": 9}],
  "bcj_lzma": [{"id": F.FILTER_X86}, {"id": F.FILTER_LZMA}],
  "delta_lzma2": [{"id": F.FILTER_DELTA, "dist": 2}, {"id": F.FILTER_LZMA2}],
  "copy": [{"id": F.FILTER_COPY}],
  "bzip2": [{"id": F.FILTER_BZIP2}],
  "deflate": [{"id": F.FILTER_DEFLATE}],
}
def add(z):
    for e in sorted(os.listdir(src)):
        z.writeall(os.path.join(src, e), arcname=e)
for name, filt in sets.items():
    with py7zr.SevenZipFile(os.path.join(out, name + ".7z"), "w", filters=filt) as z:
        add(z)
with py7zr.SevenZipFile(os.path.join(out, "plainhdr.7z"), "w", filters=sets["lzma2"]) as z:
    z.set_encoded_header_mode(False)
    add(z)
with py7zr.SevenZipFile(os.path.join(out, "aes.7z"), "w", password="secret") as z:
    add(z)
`

// ---------------- small helpers ----------------

var itoa = strconv.Itoa

// oneCoder is a single-coder folder.
func oneCoder(id, props, packed []byte, files ...[]byte) tfolder {
	all := bytes.Join(files, nil)
	return tfolder{
		coders:  []coderInfo{{id: id, numIn: 1, numOut: 1, props: props}},
		packed:  []int{0},
		packs:   [][]byte{packed},
		sizes:   []uint64{uint64(len(all))},
		streams: files,
	}
}

// lzma2Props encodes a dictionary size (a power of two here) as the LZMA2
// property byte.
func lzma2Props(dict int) byte {
	for p := byte(0); p < 40; p++ {
		if (2|uint32(p)&1)<<(p/2+11) >= uint32(dict) {
			return p
		}
	}
	return 40
}
