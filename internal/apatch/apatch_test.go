package apatch

// Tests for the bsdiff4 decoder and the Archipelago patch reader. The files in
// testdata/archipelago (at the top of the repository) are made-up data, patched with Archipelago's own patch
// classes by tools/ap-testdata.py.

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func apTestFile(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "testdata", "archipelago", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestBsdiff4(t *testing.T) {
	old, want := apTestFile(t, "old.bin"), apTestFile(t, "new.bin")
	got, err := BSDiff4(old, apTestFile(t, "old_new.bsdiff4"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("patched file differs (%d bytes, want %d)", len(got), len(want))
	}
	// a patch against an empty file is all "extra" bytes
	got, err = BSDiff4(nil, apTestFile(t, "empty_new.bsdiff4"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want[:777]) {
		t.Fatal("patch from nothing differs")
	}
}

func TestBsdiff4Damaged(t *testing.T) {
	old, patch := apTestFile(t, "old.bin"), apTestFile(t, "old_new.bsdiff4")
	if _, err := BSDiff4(old, []byte("not a patch at all, really not")); err == nil {
		t.Fatal("accepted a non-patch")
	}
	// every truncation must fail cleanly, never panic
	for n := 0; n < len(patch); n += 7 {
		if _, err := BSDiff4(old, patch[:n]); err == nil {
			t.Fatalf("accepted a patch cut to %d bytes", n)
		}
	}
	// a huge claimed output size is refused before anything is allocated
	big := append([]byte(nil), patch...)
	binary.LittleEndian.PutUint64(big[24:], 1<<40)
	if _, err := BSDiff4(old, big); err == nil {
		t.Fatal("accepted a 1 TB output")
	}
	// flipped bytes in the compressed parts: an error or a result, but no panic
	for i := 32; i < len(patch); i += 3 {
		bad := append([]byte(nil), patch...)
		bad[i] ^= 0x5a
		_, _ = BSDiff4(old, bad)
	}
}

func TestBsdiffInt(t *testing.T) {
	for _, c := range []struct {
		b []byte
		v int64
	}{
		{[]byte{5, 0, 0, 0, 0, 0, 0, 0}, 5},
		{[]byte{5, 0, 0, 0, 0, 0, 0, 0x80}, -5},
		{[]byte{0, 1, 0, 0, 0, 0, 0, 0}, 256},
	} {
		if got := bsdiffInt(c.b); got != c.v {
			t.Errorf("%x: %d, want %d", c.b, got, c.v)
		}
	}
}

type apExpected struct {
	MD5   string `json:"procedure_result_md5"`
	Len   int    `json:"procedure_result_len"`
	SMCRC string `json:"sm_crc_bytes"`
}

func apExpect(t *testing.T) apExpected {
	var e apExpected
	if err := json.Unmarshal(apTestFile(t, "expected.json"), &e); err != nil {
		t.Fatal(err)
	}
	return e
}

func TestAPProcedurePatch(t *testing.T) {
	p, err := Read(apTestFile(t, "AP_test_P1_Tester.aprptest"), "AP_test_P1_Tester.aprptest")
	if err != nil {
		t.Fatal(err)
	}
	if p.Game != "RetroPixels Test" || p.Player != 1 || p.PlayerName != "Tester" || p.Server != "localhost:38281" ||
		p.System != "snes" || p.Unsupported != "" || len(p.steps) != 3 || len(p.BaseMD5) != 1 {
		t.Fatalf("manifest read wrong: %+v steps %v", p, p.steps)
	}
	base := apTestFile(t, "procedure_base.bin")
	rom, err := p.Apply(base)
	if err != nil {
		t.Fatal(err)
	}
	e := apExpect(t)
	if len(rom) != e.Len || MD5Hex(rom) != e.MD5 {
		t.Fatalf("patched ROM differs from Archipelago's: %d bytes md5 %s, want %d %s", len(rom), MD5Hex(rom), e.Len, e.MD5)
	}
	// the same ROM with a copier header in front is accepted (and the header dropped)
	headered := append(make([]byte, 512), base...)
	rom2, err := p.Apply(headered)
	if err != nil || MD5Hex(rom2) != e.MD5 {
		t.Fatalf("headered base: %v", err)
	}
	// a different ROM is refused as the wrong base
	other := append([]byte(nil), base...)
	other[100] ^= 1
	_, err = p.Apply(other)
	var wb *WrongBaseError
	if !errors.As(err, &wb) {
		t.Fatalf("wrong base accepted: %v", err)
	}
}

func TestAPDeltaPatchWithoutProcedure(t *testing.T) {
	p, err := Read(apTestFile(t, "AP_old_P2_Other.aprpold"), "AP_old_P2_Other.aprpold")
	if err != nil {
		t.Fatal(err)
	}
	if p.System != "gb" || p.Unsupported != "" || len(p.steps) != 1 || p.steps[0].Name != "apply_bsdiff4" || p.PlayerName != "Other" {
		t.Fatalf("old delta patch read wrong: %+v %v", p, p.steps)
	}
	rom, err := p.Apply(apTestFile(t, "old.bin"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(rom, apTestFile(t, "new.bin")) {
		t.Fatal("old delta patch made the wrong file")
	}
}

func TestAPCustomStepUnsupported(t *testing.T) {
	p, err := Read(apTestFile(t, "AP_custom.aprpcustom"), "AP_custom.aprpcustom")
	if err != nil {
		t.Fatal(err)
	}
	if p.System != "gba" || !strings.Contains(p.Unsupported, "shuffle_enemies") {
		t.Fatalf("custom step not reported: %+v", p)
	}
	if _, err := p.Apply([]byte("anything")); err == nil {
		t.Fatal("applied a patch with an unknown step")
	}
}

func TestAPNotAPatch(t *testing.T) {
	if _, err := Read([]byte("hello"), "x.apsm"); err == nil {
		t.Fatal("read a non-zip")
	}
	if _, err := Read(apTestFile(t, "old.bin"), "x.apsm"); err == nil {
		t.Fatal("read random bytes")
	}
}

func TestAPTokens(t *testing.T) {
	tok := func(kind byte, off uint32, arg []byte) []byte {
		b := []byte{kind, 0, 0, 0, 0, 0, 0, 0, 0}
		binary.LittleEndian.PutUint32(b[1:], off)
		binary.LittleEndian.PutUint32(b[5:], uint32(len(arg)))
		return append(b, arg...)
	}
	pair := func(a, b uint32) []byte {
		x := make([]byte, 8)
		binary.LittleEndian.PutUint32(x, a)
		binary.LittleEndian.PutUint32(x[4:], b)
		return x
	}
	data := []byte{4, 0, 0, 0}
	data = append(data, tok(tokWrite, 1, []byte{0xaa, 0xbb})...)
	data = append(data, tok(tokRLE, 4, pair(3, 0x11))...)
	data = append(data, tok(tokCopy, 8, pair(2, 1))...)
	data = append(data, tok(tokXor, 0, []byte{0xff})...)
	rom := make([]byte, 10)
	got, err := ApplyTokens(rom, data)
	if err != nil {
		t.Fatal(err)
	}
	want := []byte{0xff, 0xaa, 0xbb, 0, 0x11, 0x11, 0x11, 0, 0xaa, 0xbb}
	if !bytes.Equal(got, want) {
		t.Fatalf("got %x want %x", got, want)
	}
	// cut short: an error, not a panic
	for n := 0; n < len(data); n++ {
		_, _ = ApplyTokens(make([]byte, 10), data[:n])
	}
}

func TestSNESChecksums(t *testing.T) {
	e := apExpect(t)
	rom, err := SNESMirrorCRC(append([]byte(nil), apTestFile(t, "crc_odd.bin")...))
	if err != nil {
		t.Fatal(err)
	}
	if hex.EncodeToString(rom[0x7fdc:0x7fe0]) != e.SMCRC {
		t.Fatalf("Super Metroid checksum %x, want %s", rom[0x7fdc:0x7fe0], e.SMCRC)
	}
	// calc_snes_crc: the checksum and its complement add up across the header
	r := make([]byte, 0x8000)
	for i := range r {
		r[i] = byte(i * 7)
	}
	r, _ = SNESHeaderCRC(r)
	crc := int(r[0x7fde]) | int(r[0x7fdf])<<8
	inv := int(r[0x7fdc]) | int(r[0x7fdd])<<8
	sum := 0
	for _, b := range r {
		sum += int(b)
	}
	if crc^inv != 0xffff || sum&0xffff != crc {
		t.Fatalf("calc_snes_crc: crc %04x inv %04x sum %04x", crc, inv, sum&0xffff)
	}
}

func TestBaseCandidates(t *testing.T) {
	nes := append([]byte("NES\x1a"), make([]byte, 12+0x8000)...)
	nes[100] = 1
	p := &Patch{System: "nes", BaseMD5: []string{MD5Hex(nes[16:])}}
	got, err := p.PickBase(nes)
	if err != nil || len(got) != len(nes) {
		t.Fatalf("NES: the headered ROM should be used as it is (%v)", err)
	}
	v64 := []byte{0x37, 0x80, 0x40, 0x12, 1, 2, 3, 4}
	c := BaseCandidates(v64, "n64")
	if len(c) != 2 || !bytes.Equal(c[1], []byte{0x80, 0x37, 0x12, 0x40, 2, 1, 4, 3}) {
		t.Fatalf("v64 to z64: %x", c)
	}
}

// TestAPRealPatches checks patch files made by a real Archipelago generation
// against Archipelago's own patcher. Set RP_AP_PATCH_DIR to a folder holding,
// for each patch file X, X.base (the base ROM the patch's base_checksum names)
// and X.result (what Archipelago made from them). No ROMs are kept in the repo.
func TestAPRealPatches(t *testing.T) {
	dir := os.Getenv("RP_AP_PATCH_DIR")
	if dir == "" {
		t.Skip("set RP_AP_PATCH_DIR to compare with Archipelago's own patcher")
	}
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	n := 0
	for _, e := range ents {
		name := e.Name()
		if strings.HasSuffix(name, ".base") || strings.HasSuffix(name, ".result") {
			continue
		}
		read := func(s string) []byte {
			b, err := os.ReadFile(filepath.Join(dir, s))
			if err != nil {
				t.Fatal(err)
			}
			return b
		}
		p, err := Read(read(name), name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		rom, err := p.Apply(read(name + ".base"))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if want := read(name + ".result"); !bytes.Equal(rom, want) {
			t.Fatalf("%s: RetroPixels made a different ROM (%d bytes, md5 %s; Archipelago: %d bytes, md5 %s)", name, len(rom), MD5Hex(rom), len(want), MD5Hex(want))
		}
		t.Logf("%s (%s, %s): same as Archipelago's, md5 %s", name, p.Game, p.System, MD5Hex(rom))
		n++
	}
	if n == 0 {
		t.Fatal("no patch files there")
	}
}

func TestIsPatchName(t *testing.T) {
	for name, want := range map[string]bool{
		"AP_1_P1_Me.apsm": true, `C:\\x\\AP_1_P2_You.APRED`: true, "a.apemerald": true, "a.ap": false,
		"game.gba": false, "game.zip": false, "apsm": false,
	} {
		if IsPatchName(name) != want {
			t.Errorf("IsPatchName(%q) = %v", name, !want)
		}
	}
}

func TestUnzipBase(t *testing.T) {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for name, n := range map[string]int{"readme.txt": 10, "game.sfc": 5000, "x/": 0} {
		w, _ := zw.Create(name)
		w.Write(bytes.Repeat([]byte{byte(n)}, n))
	}
	zw.Close()
	got, err := UnzipBase(buf.Bytes())
	if err != nil || len(got) != 5000 {
		t.Fatalf("zipped ROM: %d bytes, %v", len(got), err)
	}
	plain := []byte("not a zip")
	if got, err := UnzipBase(plain); err != nil || !bytes.Equal(got, plain) {
		t.Fatal("a plain ROM should come back as it is")
	}
	p := &Patch{Game: "Pokemon Red and Blue", PatchEnding: ".apred"}
	if p.BaseKey() != "Pokemon Red and Blue|.apred" {
		t.Fatal(p.BaseKey())
	}
}
