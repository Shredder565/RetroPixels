package nes

import (
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"math"
	"os/exec"
	"path/filepath"
	"testing"
)

// TestTablesMatchJS checks the computed tables (palette, sound chip tables)
// against the web version's. The palette must match exactly. The sound tables
// are correctly rounded here, as in current browsers (V8 with LLVM's libc);
// older engines such as Node 22's use fdlibm, which can be one unit in the
// last place away, so up to 1 ulp is accepted (that is ~1e-16 relative, far
// below what reaches the float32 audio output).
func TestTablesMatchJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	emu, _ := filepath.Abs("../../web/emu")
	out, err := exec.Command(node, "testdata/tables.js", emu).Output()
	if err != nil {
		t.Fatal(err)
	}
	var js struct {
		Palette                                 []uint32
		Vol5b, Ksl, Sin, DbToAmp, Decay, Attack []string
	}
	if err := json.Unmarshal(out, &js); err != nil {
		t.Fatal(err)
	}
	initPalette()
	initChipTables()
	for i, c := range js.Palette {
		if palette[i] != c {
			t.Errorf("palette %d: js %06x go %06x", i, c, palette[i])
		}
	}
	check := func(name string, want []string, got []float64) {
		if len(want) != len(got) {
			t.Fatalf("%s: length %d vs %d", name, len(want), len(got))
		}
		ulp1 := 0
		for i, h := range want {
			b, _ := hex.DecodeString(h)
			w := math.Float64frombits(binary.LittleEndian.Uint64(b))
			d := int64(math.Float64bits(w)) - int64(math.Float64bits(got[i]))
			switch {
			case d == 0:
			case d == 1 || d == -1:
				ulp1++
			default:
				t.Errorf("%s[%d]: js %v go %v", name, i, w, got[i])
			}
		}
		if ulp1 > 0 {
			t.Logf("%s: %d of %d values 1 ulp from this JS engine's", name, ulp1, len(want))
		}
	}
	v5 := make([]float64, len(vol5B))
	for i, v := range vol5B {
		v5[i] = float64(v)
	}
	check("vol5b", js.Vol5b, v5)
	check("ksl", js.Ksl, kslDB[:])
	check("sin", js.Sin, fmSin[:])
	check("dbToAmp", js.DbToAmp, dbToAmp[:])
	check("decay", js.Decay, fmDecay[:])
	check("attack", js.Attack, fmAttack[:])
}

func BenchmarkTables(b *testing.B) {
	for i := 0; i < b.N; i++ {
		buildPalette()
		buildChipTables()
	}
}
