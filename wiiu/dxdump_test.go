package wiiu

import (
	"fmt"
	"os"
	"testing"

	"retropixels/wiiu/d3d"
)

// TestDXDump prints the translation of testdata/r700/$DXDUMP.bin.
func TestDXDump(t *testing.T) {
	name := os.Getenv("DXDUMP")
	if name == "" {
		t.Skip()
	}
	code, err := os.ReadFile("testdata/r700/" + name + ".bin")
	if err != nil {
		t.Fatal(err)
	}
	r := newVSRun()
	r.load(r.vs, code)
	ret := &shaderAsm{}
	ret.CF(cfRETURN, 0, 0, 0, 0)
	r.load(r.fs, ret.Bytes())
	cfg := &dxConfig{read32: r.read32, fsAddr: r.fs, nParams: 1, testPoints: 4, testGPRs: 2,
		texInfo: func(uint32) dxTexKey { return dxTexKey{} }, vbufInfo: func(uint32) dxFetchKey { return dxFetchKey{} }}
	p, err := translateDX(cfg, r.vs)
	if err != nil {
		t.Fatal(err)
	}
	fmt.Print(d3d.Disasm(p.code))
}
