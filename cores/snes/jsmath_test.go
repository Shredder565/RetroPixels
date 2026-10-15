package snes

import (
	"encoding/binary"
	"encoding/json"
	"math"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// TestJSMath checks jsmath.go against Node's Math functions, bit for bit, on
// the inputs this core uses (whole tables of angles) and on random ones.
func TestJSMath(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	type rec struct{ op, a, b float64 }
	var in []rec
	turn := 2 * math.Pi / 65536
	for k := -70000; k <= 70000; k++ {
		for op := 0; op < 3; op++ {
			in = append(in, rec{float64(op), float64(k) * turn, 0})
		}
	}
	for i := 0; i < 512; i++ {
		in = append(in, rec{0, 2 * math.Pi * float64(i) / 512, 0}, rec{1, 2 * math.Pi * float64(i) / 512, 0})
	}
	for a := -1024; a <= 1024; a++ {
		in = append(in, rec{0, -float64(a) * (2 * math.Pi / 128), 0}, rec{1, -float64(a) * (2 * math.Pi / 128), 0})
	}
	rng := rand.New(rand.NewSource(1))
	for i := 0; i < 300000; i++ {
		x := (rng.Float64()*2 - 1) * math.Ldexp(1, rng.Intn(24)-4)
		in = append(in, rec{float64(rng.Intn(3)), x, 0})
		in = append(in, rec{3, (rng.Float64()*2 - 1) * math.Ldexp(1, rng.Intn(12)-6), 0})
		y := math.Ldexp(rng.Float64()+0.5, rng.Intn(200)-100)
		if rng.Intn(4) == 0 {
			y = math.Ldexp(1, rng.Intn(100)-50) * (1 + float64(rng.Intn(5)-2)*0x1p-52)
		}
		in = append(in, rec{4, y, 0})
		in = append(in, rec{5, float64(rng.Intn(65536) - 32768), float64(rng.Intn(65536) - 32768)})
		in = append(in, rec{6, float64(rng.Intn(20000)-10000) / 4, 0})
		if x := rng.Intn(65536) - 32768; x != 0 {
			in = append(in, rec{7, float64(rng.Intn(65536)-32768) / float64(x), 0})
		}
		in = append(in, rec{7, (rng.Float64()*2 - 1) * math.Ldexp(1, rng.Intn(60)-30), 0})
	}
	for f := 0; f < 256; f++ {
		tt := float64(f) / 256
		for _, d := range []float64{1 + tt, tt, 1 - tt, 2 - tt} {
			in = append(in, rec{3, -(d * d) / (2 * 0.62 * 0.62), 0})
		}
	}
	dir := t.TempDir()
	buf := make([]byte, 24*len(in))
	for i, r := range in {
		binary.LittleEndian.PutUint64(buf[24*i:], math.Float64bits(r.op))
		binary.LittleEndian.PutUint64(buf[24*i+8:], math.Float64bits(r.a))
		binary.LittleEndian.PutUint64(buf[24*i+16:], math.Float64bits(r.b))
	}
	ip, op := filepath.Join(dir, "in.bin"), filepath.Join(dir, "out.bin")
	os.WriteFile(ip, buf, 0o644)
	if b, err := exec.Command(node, "testdata/jsmath.js", ip, op).CombinedOutput(); err != nil {
		t.Fatalf("node: %v %s", err, b)
	}
	out, err := os.ReadFile(op)
	if err != nil {
		t.Fatal(err)
	}
	names := []string{"sin", "cos", "tan", "exp", "log2", "hypot", "round", "atan"}
	fns := []func(a, b float64) float64{
		func(a, _ float64) float64 { return jsSin(a) },
		func(a, _ float64) float64 { return jsCos(a) },
		func(a, _ float64) float64 { return jsTan(a) },
		func(a, _ float64) float64 { return jsExp(a) },
		func(a, _ float64) float64 { return jsLog2(a) },
		jsHypot,
		func(a, _ float64) float64 { return jsRound(a) },
		func(a, _ float64) float64 { return jsAtan(a) },
	}
	bad := map[string]int{}
	for i, r := range in {
		want := math.Float64frombits(binary.LittleEndian.Uint64(out[8*i:]))
		got := fns[int(r.op)](r.a, r.b)
		if math.Float64bits(got) != math.Float64bits(want) && !(got == 0 && want == 0) {
			n := names[int(r.op)]
			if bad[n] < 3 {
				t.Errorf("%s(%v, %v) = %v, node %v", n, r.a, r.b, got, want)
			}
			bad[n]++
		}
	}
	if len(bad) > 0 {
		t.Errorf("differences: %v (of %d)", bad, len(in))
	}
}

// TestKernMatchesJS checks the S-DSP interpolation kernel against sdsp.js's.
func TestKernMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	out, err := exec.Command(node, "testdata/kern.js", "../../web/emu/sdsp.js").Output()
	if err != nil {
		t.Fatal(err)
	}
	var js []int
	if err := json.Unmarshal(out, &js); err != nil {
		t.Fatal(err)
	}
	if len(js) != len(dspKern) {
		t.Fatalf("kernel size %d, js %d", len(dspKern), len(js))
	}
	for i := range js {
		if js[i] != dspKern[i] {
			t.Fatalf("kernel[%d] = %d, js %d", i, dspKern[i], js[i])
		}
	}
}
