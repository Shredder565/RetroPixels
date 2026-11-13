package mame

import (
	"archive/zip"
	"hash/crc32"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"retropixels/internal/romdb"
)

// writeZip makes a zip of name -> contents.
func writeZip(t *testing.T, path string, files map[string]string) {
	t.Helper()
	os.MkdirAll(filepath.Dir(path), 0o755)
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	z := zip.NewWriter(f)
	for n, c := range files {
		w, _ := z.Create(n)
		w.Write([]byte(c))
	}
	z.Close()
	f.Close()
}

func crc(s string) uint32 { return crc32.ChecksumIEEE([]byte(s)) }

func rom(name, data string, from, to int) romdb.Rom {
	return romdb.Rom{Name: name, CRC: crc(data), Size: uint32(len(data)), Bios: -1, From: from, To: to}
}

// testDB: versions 0.200-0.202. game1 (a, b, then c changed to c2 in 0.202),
// its clone game1b (a, b, d), the BIOS "sys" with two choices, and "child",
// which runs on it.
func testDB() *romdb.DB {
	sysRoms := []romdb.Rom{rom("bios-eu.bin", "BIOS-EU", 0, 2), rom("bios-jp.bin", "BIOS-JP", 0, 2), rom("sys-common", "SYS", 0, 2)}
	sysRoms[0].Bios, sysRoms[1].Bios = 0, 1
	return romdb.New([]string{"0.200", "0.201", "0.202"}, []*romdb.Machine{
		{Name: "game1", Desc: "Game One", From: 0, To: 2, Roms: []romdb.Rom{rom("a.1", "AAAA", 0, 2), rom("b.2", "BBBB", 0, 2), rom("c.3", "CCCC", 0, 1), rom("c.3", "CCC2", 2, 2)}},
		{Name: "game1b", Desc: "Game One (set 2)", Parent: "game1", From: 0, To: 2, Roms: []romdb.Rom{rom("a.1", "AAAA", 0, 2), rom("b.2", "BBBB", 0, 2), rom("d.4", "DDDD", 0, 2)}},
		{Name: "sys", Desc: "The System", BiosRoot: true, From: 0, To: 2, Bios: []string{"euro", "japan"}, Roms: sysRoms},
		{Name: "child", Desc: "Child Game", Parent: "sys", From: 0, To: 2, Roms: []romdb.Rom{rom("p1", "CHILD-P1", 0, 2)}},
	})
}

func fakeExe(t *testing.T, dir, ver string) Install {
	p := filepath.Join(dir, "mame"+ver, "mame.exe")
	os.MkdirAll(filepath.Dir(p), 0o755)
	os.WriteFile(p, []byte("x"), 0o644)
	return Install{Exe: p, Version: ver}
}

func TestResolveRenamedOldSet(t *testing.T) {
	dir := t.TempDir()
	db := testDB()
	// game1 as dumped for 0.201 (old c.3), files and zip renamed
	p := filepath.Join(dir, "roms", "My Game One.zip")
	writeZip(t, p, map[string]string{"first": "AAAA", "second": "BBBB", "third": "CCCC"})
	if m := Identify(db, filesOf(p), p); m == nil || m.Name != "game1" {
		t.Fatalf("identified as %v", m)
	}
	// only 0.202 here: it doesn't accept the old c.3, so 0.201 is to be downloaded
	in202 := fakeExe(t, dir, "0.202")
	res, ok := Resolve(db, p, []Install{in202}, in202.Exe, Pool{}, func(string) bool { return true })
	if !ok || res.Install != nil || res.Download != "0.201" {
		t.Fatalf("got %+v", res)
	}
	// with 0.200 installed, that one plays it, from a set put together
	in200 := fakeExe(t, dir, "0.200")
	res, ok = Resolve(db, p, []Install{in202, in200}, in202.Exe, Pool{}, nil)
	if !ok || res.Install == nil || res.Install.Version != "0.200" || res.AsIs {
		t.Fatalf("got %+v", res)
	}
	out, err := res.Build(filepath.Join(dir, "sets"))
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(out) != "game1.zip" {
		t.Fatal(out)
	}
	got := map[string]uint32{}
	for _, f := range filesOf(out) {
		got[f.Name] = f.CRC
	}
	if len(got) != 3 || got["a.1"] != crc("AAAA") || got["c.3"] != crc("CCCC") {
		t.Fatalf("built set has %v", got)
	}
	// building again keeps it
	st1, _ := os.Stat(out)
	res.Build(filepath.Join(dir, "sets"))
	st2, _ := os.Stat(out)
	if !st1.ModTime().Equal(st2.ModTime()) {
		t.Fatal("rebuilt an identical set")
	}
}

func TestResolveSplitCloneAndBios(t *testing.T) {
	dir := t.TempDir()
	db := testDB()
	roms := filepath.Join(dir, "roms")
	writeZip(t, filepath.Join(roms, "game1.zip"), map[string]string{"a.1": "AAAA", "b.2": "BBBB", "c.3": "CCC2"})
	clone := filepath.Join(roms, "game1b.zip")
	writeZip(t, clone, map[string]string{"d.4": "DDDD"})
	// the BIOS in another folder, with only the Japanese choice
	writeZip(t, filepath.Join(dir, "bios", "sys.zip"), map[string]string{"bios-jp.bin": "BIOS-JP", "sys-common": "SYS"})
	child := filepath.Join(roms, "child.zip")
	writeZip(t, child, map[string]string{"p1": "CHILD-P1"})
	in := fakeExe(t, dir, "0.202")
	pool := NewPool([]string{roms, filepath.Join(dir, "bios")})

	res, ok := Resolve(db, clone, []Install{in}, in.Exe, pool, nil)
	if !ok || res.Machine.Name != "game1b" || res.Install == nil || res.AsIs || len(res.Files) != 3 {
		t.Fatalf("clone: %+v", res)
	}
	res, ok = Resolve(db, child, []Install{in}, in.Exe, pool, nil)
	if !ok || res.Install == nil || res.Bios != "japan" || len(res.Files) != 3 {
		t.Fatalf("child: %+v", res)
	}
	// the complete parent plays as it is
	res, _ = Resolve(db, filepath.Join(roms, "game1.zip"), []Install{in}, in.Exe, pool, nil)
	if !res.AsIs {
		t.Fatalf("parent: %+v", res)
	}
}

func TestResolveMissing(t *testing.T) {
	dir := t.TempDir()
	db := testDB()
	p := filepath.Join(dir, "game1b.zip")
	writeZip(t, p, map[string]string{"d.4": "DDDD", "a.1": "AAAA"})
	in := fakeExe(t, dir, "0.202")
	res, ok := Resolve(db, p, []Install{in}, in.Exe, Pool{}, func(string) bool { return true })
	if !ok || res.Install != nil || res.Download != "" || len(res.Missing) != 1 || res.Missing[0].Name != "b.2" {
		t.Fatalf("got %+v", res)
	}
	if txt := res.MissingText(); !strings.Contains(txt, "b.2") || !strings.Contains(txt, "game1.zip") {
		t.Fatal(txt)
	}
}

func TestVersionIndex(t *testing.T) {
	db := testDB()
	for v, want := range map[string]int{"0.201": 1, "0.250": 2, "0.150": -1, "": -1} {
		if got := versionIndex(db, v); got != want {
			t.Errorf("%s: %d, want %d", v, got, want)
		}
	}
}

func TestParseDeviceRoms(t *testing.T) {
	x := `<?xml version="1.0"?><mame><machine name="digdug"><rom name="dd1a.1" crc="a80ec984"/><device_ref name="namco51"/></machine>
<machine name="namco51" isdevice="yes"><rom name="51xx.bin" crc="c2f57ef8"/></machine>
<machine name="speaker" isdevice="yes"></machine>
<machine name="opt" isdevice="yes"><rom name="o.bin" crc="12345678" optional="yes"/><rom name="nd.bin" status="nodump"/></machine></mame>`
	l, err := parseDeviceRoms(x)
	if err != nil || len(l) != 1 || l[0].Device != "namco51" || l[0].CRC != 0xc2f57ef8 {
		t.Fatalf("%v %+v", err, l)
	}
}
