package main

import (
	"archive/zip"
	"os"
	"path/filepath"
	"testing"
)

// The program's folder is found wherever the zip keeps it, and moved whole.
func TestPlace(t *testing.T) {
	dir := t.TempDir()
	zp := filepath.Join(dir, "a.zip")
	f, _ := os.Create(zp)
	zw := zip.NewWriter(f)
	for _, n := range []string{"azahar-windows-msys2-2126.2/azahar.exe", "azahar-windows-msys2-2126.2/Qt6Core.dll", "azahar-windows-msys2-2126.2\\plugins\\x.dll"} {
		w, _ := zw.Create(n)
		w.Write([]byte("x"))
	}
	zw.Close()
	f.Close()
	dst := filepath.Join(dir, "out", "Azahar")
	if err := place(zp, filepath.Join(dir, "tmp"), dst, []string{"azahar.exe"}); err != nil {
		t.Fatal(err)
	}
	for _, n := range []string{"azahar.exe", "Qt6Core.dll", "plugins/x.dll"} {
		if _, err := os.Stat(filepath.Join(dst, n)); err != nil {
			t.Error(err)
		}
	}
	if err := place(zp, filepath.Join(dir, "tmp2"), dst, []string{"melonDS.exe"}); err == nil {
		t.Error("placed a zip without the program")
	}
}
