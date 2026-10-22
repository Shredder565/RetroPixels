package wiiu

// The shaders RetroPixels' GX2 test program uses, made with the shader
// assembler and kept in testdata/progs/gx2shaders.h (regenerate with
// WIIU_UPDATE=1 go test -run GX2Shaders ./wiiu).

import (
	"fmt"
	"os"
	"strings"
	"testing"
)

func testShaders() map[string]*shaderAsm {
	// vertex shader: R1 = position, R2 = colour or texture coordinate;
	// the position is scaled and moved by uniform c0 (xy scale, zw offset)
	vs := &shaderAsm{}
	vs.CF(cfCALL_FS, 0, 0, 0, 0)
	vs.ALU(8, [2]kcacheSpec{}, aluGroupSpec{insts: []aluInst{
		op3i(0x10, 1, 0, gprSrc(1, 0), cfileSrc(0, 0), cfileSrc(0, 2)),
		op3i(0x10, 1, 1, gprSrc(1, 1), cfileSrc(0, 1), cfileSrc(0, 3)),
	}})
	vs.Export(true, 1, 60, 1, 1, [4]uint32{0, 1, 2, 3})
	vs.Export(true, 2, 0, 2, 1, [4]uint32{0, 1, 2, 3})
	vs.End()

	// pixel shader: the interpolated colour
	ps := &shaderAsm{}
	ps.Export(true, 0, 0, 0, 1, [4]uint32{0, 1, 2, 3})
	ps.End()

	// pixel shader: texture 0 times uniform c0
	pt := &shaderAsm{}
	pt.TEX(texSpec{inst: 0x10, srcGPR: 0, srcSel: [4]uint32{0, 1, 4, 4}, dstGPR: 1, dstSel: [4]uint32{0, 1, 2, 3},
		normalized: [4]bool{true, true, true, true}})
	var mul []aluInst
	for c := uint32(0); c < 4; c++ {
		mul = append(mul, op2(0x02, 1, c, gprSrc(1, c), cfileSrc(0, c)))
	}
	pt.ALU(8, [2]kcacheSpec{}, aluGroupSpec{insts: mul})
	pt.Export(true, 0, 0, 1, 1, [4]uint32{0, 1, 2, 3})
	pt.End()
	return map[string]*shaderAsm{"vs_main": vs, "ps_color": ps, "ps_tex": pt}
}

func shadersHeader() string {
	var b strings.Builder
	b.WriteString("/* Made by gx2shaders_test.go: R700 shader code for gx2test.c, as the GPU\n   reads it (little-endian words). */\n")
	for _, name := range []string{"vs_main", "ps_color", "ps_tex"} {
		w := testShaders()[name].Words()
		fmt.Fprintf(&b, "static const uint32_t %s[] __attribute__((aligned(256))) = {", name)
		for i, v := range w {
			if i%6 == 0 {
				b.WriteString("\n\t")
			}
			fmt.Fprintf(&b, "0x%08x, ", bswap32(v))
		}
		b.WriteString("\n};\n")
	}
	return b.String()
}

func TestGX2Shaders(t *testing.T) {
	want := shadersHeader()
	const path = "testdata/progs/gx2shaders.h"
	if os.Getenv("WIIU_UPDATE") != "" {
		if err := os.WriteFile(path, []byte(want), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	got, err := os.ReadFile(path)
	if err != nil || string(got) != want {
		t.Fatalf("%s is out of date: run WIIU_UPDATE=1 go test -run GX2Shaders ./wiiu", path)
	}
}
