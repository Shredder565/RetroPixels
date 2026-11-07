package romdb

import (
	"bytes"
	"testing"
)

func TestRoundTrip(t *testing.T) {
	d := New([]string{"0.200", "0.201"}, []*Machine{
		{Name: "neogeo", Desc: "Neo-Geo", BiosRoot: true, To: 1, Bios: []string{"euro", "japan"}, DefaultBios: "euro",
			Roms: []Rom{{Name: "sp-s2.sp1", CRC: 0x9036d879, Size: 0x20000, Bios: 0, To: 1}, {Name: "vs-bios.rom", CRC: 0xf0e8f27d, Size: 0x20000, Bios: 1, Bad: true, From: 1, To: 1}}},
		{Name: "mslug", Desc: "Metal Slug", Parent: "neogeo", CHD: false, From: 1, To: 1,
			Roms: []Rom{{Name: "201-p1.p1", CRC: 0x08d8daa5, Size: 0x200000, Bios: -1, From: 1, To: 1}}},
	})
	var b bytes.Buffer
	if err := d.Encode(&b); err != nil {
		t.Fatal(err)
	}
	e, err := Decode(b.Bytes())
	if err != nil {
		t.Fatal(err)
	}
	m := e.Machine("neogeo")
	if m == nil || !m.BiosRoot || m.DefaultBios != "euro" || len(m.Roms) != 2 || m.Roms[1].Bios != 1 || !m.Roms[1].Bad || m.Roms[1].From != 1 {
		t.Fatalf("%+v", m)
	}
	if s := e.Machine("MSLUG"); s == nil || s.Parent != "neogeo" || s.Roms[0].Bios != -1 || !s.In(1) || s.In(0) {
		t.Fatalf("%+v", s)
	}
	if l := e.WithCRC(0x08d8daa5); len(l) != 1 || l[0].Name != "mslug" {
		t.Fatal(l)
	}
	if _, err := Decode([]byte("junk")); err == nil {
		t.Fatal("junk decoded")
	}
}

func TestBuiltIn(t *testing.T) {
	d, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	if len(d.Versions) == 0 || d.Machine("pacman") == nil {
		t.Fatal("pacman isn't in the built-in history")
	}
}
