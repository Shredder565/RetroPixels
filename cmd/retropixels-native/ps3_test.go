package main

import (
	"testing"

	"retropixels/internal/rpcs3"
)

func TestPS3FileKind(t *testing.T) {
	for p, want := range map[string]string{
		`D:\PS3\game.PKG`: "pkg", "/x/XX0000-TEST00001_00-RETROPIXELSTEST1.rap": "licence", "a.EDAT": "licence",
		"EBOOT.BIN": "", "game.iso": "", "swtrilgy.zip": "",
	} {
		if got := ps3FileKind(p); got != want {
			t.Errorf("%s: %q, want %q", p, got, want)
		}
	}
}

func TestPS3GameLabel(t *testing.T) {
	g := rpcs3.Game{Title: "Tom & Jerry", TitleID: "TEST00001"}
	if got := ps3GameLabel(g); got != "Tom && Jerry  (TEST00001)" {
		t.Error(got)
	}
	g.Licence = rpcs3.LicenceCheck{ContentID: "XX0000-TEST00001_00-RETROPIXELSTEST1", Needed: true}
	if got := ps3GameLabel(g); got != "Tom && Jerry  (TEST00001) - needs its licence" {
		t.Error(got)
	}
}
