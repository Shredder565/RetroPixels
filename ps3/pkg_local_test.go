package ps3

import (
	"os"
	"testing"
)

func TestPKGLocal(t *testing.T) {
	path := os.Getenv("PS3_PKG")
	if path == "" {
		t.Skip("PS3_PKG not set")
	}
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	p, err := OpenPKG(f)
	if err != nil {
		t.Fatal(err)
	}
	t.Log(p.ContentID)
	for _, e := range p.Files {
		t.Logf("%08x %10d %-5v %s", e.Type, e.Size, e.Dir, e.Name)
	}
	if out := os.Getenv("PS3_OUT"); out != "" {
		if err := p.Extract(out, nil); err != nil {
			t.Fatal(err)
		}
	}
}

func TestSELFLocal(t *testing.T) {
	dir := os.Getenv("PS3_OUT")
	if dir == "" {
		t.Skip("PS3_OUT not set")
	}
	for _, n := range []string{"EBOOT.BIN", "TMNT_r.sprx"} {
		b, err := os.ReadFile(dir + "/USRDIR/" + n)
		if err != nil {
			t.Fatal(err)
		}
		e, err := DecryptSELF(b, nil)
		if err != nil {
			t.Fatal(n, err)
		}
		os.WriteFile(dir+"/"+n+".elf", e, 0o644)
		t.Log(n, len(e))
	}
}

func TestKlicLocal(t *testing.T) {
	dir := os.Getenv("PS3_OUT")
	if dir == "" {
		t.Skip("PS3_OUT not set")
	}
	elf, _ := os.ReadFile(dir + "/EBOOT.BIN.elf")
	f, _ := os.Open(dir + "/USRDIR/TMNT.BF.EDAT")
	h := make([]byte, 0x80)
	f.ReadAt(h, 0)
	k, ok := FindKlicensee(elf, h)
	t.Logf("%x %v", k, ok)
	if !ok {
		t.Fatal("no klic")
	}
	e, err := OpenEDAT(f, k)
	if err != nil {
		t.Fatal(err)
	}
	b := make([]byte, 256)
	e.ReadAt(b, 0)
	t.Logf("%x %q", b[:64], b)
}

func TestSPRXLocal(t *testing.T) {
	dir := os.Getenv("PS3_OUT")
	if dir == "" {
		t.Skip("PS3_OUT not set")
	}
	elf, _ := os.ReadFile(dir + "/EBOOT.BIN.elf")
	b, _ := os.ReadFile(dir + "/USRDIR/TMNT_r.sprx")
	// NPD at control info
	i := 0
	for ; i < 0x1000; i++ {
		if string(b[i:i+4]) == "NPD\x00" {
			break
		}
	}
	k, ok := FindKlicensee(elf, b[i:i+0x80])
	t.Logf("%x %v", k, ok)
	e, err := DecryptSELF(b, k)
	if err != nil {
		t.Fatal(err)
	}
	os.WriteFile(dir+"/TMNT_r.sprx.elf", e, 0o644)
	if out := os.Getenv("PS3_BF"); out != "" {
		f, _ := os.Open(dir + "/USRDIR/TMNT.BF.EDAT")
		ed, err := OpenEDAT(f, k)
		if err != nil {
			t.Fatal(err)
		}
		buf := make([]byte, ed.Size)
		ed.ReadAt(buf, 0)
		os.WriteFile(out, buf, 0o644)
	}
}
