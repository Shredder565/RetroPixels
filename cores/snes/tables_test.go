package snes

import (
	"encoding/json"
	"math"
	"os/exec"
	"testing"
)

// TestTablesMatchJS checks the whole-number tables this core builds with Go's
// maths (the DSP-1 sine and cosine for every angle, the Cx4 sine and cosine
// tables) against the same tables built with Node's Math functions.
func TestTablesMatchJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	const js = `const r=[];const T=Math.PI/32768;
for(let a=-32768;a<32768;a++){r.push(Math.round(32768*Math.sin(a*T)),Math.round(32768*Math.cos(a*T)));}
for(let i=0;i<512;i++){const x=2*Math.PI*i/512;r.push(Math.round(32767*Math.sin(x))<<16>>16,Math.round(32767*Math.cos(x))<<16>>16);}
process.stdout.write(JSON.stringify(r));`
	out, err := exec.Command(node, "-e", js).Output()
	if err != nil {
		t.Fatal(err)
	}
	var want []int
	if err := json.Unmarshal(out, &want); err != nil {
		t.Fatal(err)
	}
	var got []int
	for a := -32768; a < 32768; a++ {
		x := float64(a) * math.Pi / 32768
		got = append(got, int(jsRound(32768*math.Sin(x))), int(jsRound(32768*math.Cos(x))))
	}
	for i := range 512 {
		got = append(got, cx4Sin[i], cx4Cos[i])
	}
	if len(got) != len(want) {
		t.Fatalf("%d values, node %d", len(got), len(want))
	}
	for i := range got {
		if got[i] != want[i] {
			t.Fatalf("value %d: go %d, node %d", i, got[i], want[i])
		}
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
