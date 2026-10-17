//go:build windows

package wiiu

// Runs translated shaders on Direct3D 11 and compares them with the shader
// interpreter. Skipped when there's no Direct3D 11 device (under Wine the
// tests run on its software renderer).

import (
	"encoding/binary"
	"fmt"
	"math"
	"math/rand"
	"os"
	"sync"
	"testing"

	"retropixels/wiiu/d3d"
)

var (
	dxTestDev     *d3d.Device
	dxTestDevErr  error
	dxTestDevOnce sync.Once
)

func dxDevice(t *testing.T) *d3d.Device {
	dxTestDevOnce.Do(func() {
		dxTestDev, dxTestDevErr = d3d.NewDevice(false)
		if dxTestDevErr != nil {
			dxTestDev, dxTestDevErr = d3d.NewDevice(true)
		}
	})
	if dxTestDevErr != nil {
		t.Skip("no Direct3D 11:", dxTestDevErr)
	}
	return dxTestDev
}

func u32bytes(w []uint32) []byte {
	b := make([]byte, 4*len(w))
	for i, v := range w {
		binary.LittleEndian.PutUint32(b[4*i:], v)
	}
	return b
}

// dxPassPS copies n flat parameters to n integer render targets.
func dxPassPS(n int) []byte {
	s := &d3d.Shader{Pixel: true, In: dxVSOutSig(n)}
	for i := 0; i < n; i++ {
		s.Out = append(s.Out, d3d.Sig{Name: "SV_Target", Index: uint32(i), SV: d3d.SVTarget, CompType: 1, Reg: uint32(i), Mask: 15})
		s.DclInputPS(1+i, 15, d3d.InterpConstant)
		s.DclOutput(i, 15)
		s.Ins(d3d.OpMov, d3d.Out(i).Mask(15), d3d.In(1+i))
	}
	s.Ins(d3d.OpRet)
	return s.Bytes()
}

// d3d runs the vertex shader on the device and returns each vertex's
// outputs.
func (r *dxVSRun) d3d(t *testing.T) [][]uint32 {
	d := dxDevice(t)
	n := len(r.in)
	cfg := &dxConfig{read32: r.read32, fsAddr: r.fs, nParams: r.nOut, testPoints: n, testGPRs: r.nGPR,
		texInfo:  func(uint32) dxTexKey { return dxTexKey{} },
		vbufInfo: func(uint32) dxFetchKey { return dxFetchKey{} }}
	p, err := translateDX(cfg, r.vs)
	if err != nil {
		t.Fatal("translate:", err)
	}
	vs, err := d.CreateVertexShader(p.code)
	if err != nil {
		t.Fatal(err)
	}
	defer vs.Release()
	ps, err := d.CreatePixelShader(dxPassPS(r.nOut))
	if err != nil {
		t.Fatal(err)
	}
	defer ps.Release()
	c := d.Ctx
	c.ClearState()
	// inputs
	var words []uint32
	for _, v := range r.in {
		words = append(words, v...)
	}
	if len(words) == 0 {
		words = []uint32{0}
	}
	ib, err := d.CreateBuffer(&d3d.BufferDesc{ByteWidth: uint32(4 * len(words)), BindFlags: d3d.BindShaderResource}, u32bytes(words))
	if err != nil {
		t.Fatal(err)
	}
	defer ib.Release()
	iv, err := d.CreateBufferSRV(ib, d3d.FormatR32Uint, 0, uint32(len(words)))
	if err != nil {
		t.Fatal(err)
	}
	defer iv.Release()
	for slot, s := range p.srv {
		if s.vbuf && s.id == dxTestBuf {
			c.VSSetShaderResources(uint32(slot), iv)
		}
	}
	mkCB := func(w []uint32) d3d.Resource {
		b, err := d.CreateBuffer(&d3d.BufferDesc{ByteWidth: uint32(4 * len(w)), BindFlags: d3d.BindConstantBuffer}, u32bytes(w))
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	sys, cf := mkCB(r.sys[:]), mkCB(r.cfile[:])
	defer sys.Release()
	defer cf.Release()
	c.VSSetConstantBuffers(0, sys, cf)
	// targets
	var rts []d3d.Resource
	var rtvs []d3d.View
	for i := 0; i < r.nOut; i++ {
		tx, err := d.CreateTexture2D(&d3d.Texture2DDesc{Width: uint32(n), Height: 1, MipLevels: 1, ArraySize: 1,
			Format: d3d.FormatRGBA32Uint, SampleCount: 1, BindFlags: d3d.BindRenderTarget}, nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Release()
		v, err := d.CreateRTV(tx, d3d.FormatRGBA32Uint)
		if err != nil {
			t.Fatal(err)
		}
		defer v.Release()
		rts = append(rts, tx)
		rtvs = append(rtvs, v)
	}
	c.OMSetRenderTargets(rtvs, d3d.View{})
	for _, v := range rtvs {
		c.ClearRenderTargetView(v, [4]float32{0x55, 0x55, 0x55, 0x55})
	}
	c.RSSetViewports(d3d.Viewport{W: float32(n), H: 1, MaxZ: 1})
	c.IASetPrimitiveTopology(d3d.TopologyPointList)
	c.VSSetShader(vs)
	c.PSSetShader(ps)
	c.Draw(uint32(n), 0)
	out := make([][]uint32, n)
	for i := range out {
		out[i] = make([]uint32, 4*r.nOut)
	}
	st, err := d.CreateTexture2D(&d3d.Texture2DDesc{Width: uint32(n), Height: 1, MipLevels: 1, ArraySize: 1,
		Format: d3d.FormatRGBA32Uint, SampleCount: 1, Usage: d3d.UsageStaging, CPUAccess: d3d.CPURead}, nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Release()
	for k, tx := range rts {
		c.CopyResource(st, tx)
		m, err := c.Map(st, 0, d3d.MapRead)
		if err != nil {
			t.Fatal(err)
		}
		px := m.Words(4 * n)
		for i := 0; i < n; i++ {
			copy(out[i][4*k:4*k+4], px[4*i:4*i+4])
		}
		c.Unmap(st, 0)
	}
	return out
}

// compare checks the two runs agree (floats within a small tolerance, or
// exactly for exact).
func (r *dxVSRun) compare(t *testing.T, exact bool) {
	want := r.interp(t)
	got := r.d3d(t)
	if testing.Verbose() {
		t.Logf("in %x", r.in)
		t.Logf("got %x", got)
		t.Logf("want %x", want)
	}
	bad := 0
	for v := range want {
		for k := range want[v] {
			a, b := got[v][k], want[v][k]
			if a == b || !exact && f32eq(u2f(a), u2f(b)) {
				continue
			}
			if bad < 10 {
				t.Errorf("vertex %d out %d.%c: got %v (%08x), want %v (%08x)", v, k/4, "xyzw"[k%4], u2f(a), a, u2f(b), b)
			}
			bad++
		}
	}
	if bad > 10 {
		t.Errorf("... %d mismatches", bad)
	}
}

// TestDXLLVM runs the LLVM-compiled shaders through the translator.
func TestDXLLVM(t *testing.T) {
	inputs := [][8]float32{
		{1.5, -2.25, 3, 7, 0.5, 2},
		{-3.75, 4.5, -1, 12, 1.25, -1},
		{0, 2, 0, 3, -2, 0.5},
		{6.25, 6.25, 100, -5, 4, 3},
		{2, 0.5, 0.75, 1, 3, 9},
		{-1, -1, -1, -1, -1, -1},
		{11, 3.5, 2, 0.25, 0.125, 7},
	}
	for _, c := range shaderCases {
		t.Run(c.name, func(t *testing.T) {
			code, err := os.ReadFile("testdata/r700/" + c.name + ".bin")
			if err != nil {
				t.Skip("no compiled shader (run TestDumpR700 with llc):", err)
			}
			r := newVSRun()
			r.load(r.vs, code)
			ret := &shaderAsm{}
			ret.CF(cfRETURN, 0, 0, 0, 0)
			r.load(r.fs, ret.Bytes())
			r.nGPR, r.nOut = 2, 1
			for _, in := range inputs {
				w := make([]uint32, 8)
				for i := range in {
					w[i] = math.Float32bits(in[i])
				}
				r.in = append(r.in, w)
			}
			r.compare(t, c.ints)
		})
	}
}

func dxSimpleRun(vs *shaderAsm, nGPR, nOut int, in ...[]float32) *dxVSRun {
	r := dxSimpleRunAny(vs, nGPR, nOut)
	for _, v := range in {
		w := make([]uint32, 4*nGPR)
		for i := range v {
			w[i] = math.Float32bits(v[i])
		}
		r.in = append(r.in, w)
	}
	return r
}

func TestDXExport(t *testing.T) {
	vs := &shaderAsm{}
	vs.Export(true, 1, 60, 1, 1, [4]uint32{0, 1, 2, 3})
	vs.Export(true, 2, 0, 2, 1, [4]uint32{3, 2, 5, 4})
	vs.End()
	r := dxSimpleRun(vs, 2, 2, []float32{1, 2, 3, 4, 5, 6, 7, 8}, []float32{-1, -2, -3, -4, 9, 10, 11, 12})
	r.compare(t, true)
}

func TestDXALU(t *testing.T) {
	vs := &shaderAsm{}
	vs.ALU(8, [2]kcacheSpec{}, aluGroupSpec{insts: []aluInst{
		op2(0x00, 3, 0, gprSrc(1, 0), gprSrc(1, 1)), // ADD
		op2(0x02, 3, 1, gprSrc(1, 2), gprSrc(2, 0)), // MUL_IEEE
		op2(0x03, 3, 2, gprSrc(1, 3), gprSrc(2, 1)), // MAX
		op2(0x19, 3, 3, gprSrc(2, 2), gprSrc(2, 2)), // MOV
	}})
	vs.Export(true, 1, 60, 3, 1, [4]uint32{0, 1, 2, 3})
	vs.End()
	r := dxSimpleRun(vs, 2, 1, []float32{1, 2, 3, 4, 5, 6, 7, 8}, []float32{-1, -2, -3, -4, 9, 10, 11, 12})
	r.compare(t, true)
}

// dxApprox: instructions whose results Direct3D only gets close to.
func dxApprox(op uint32) bool {
	switch op {
	case 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6e, 0x6f:
		return true
	}
	return false
}

// Over thousands of programs (DXRANDN=3000) a handful still differ under
// Wine's software renderer, all from its fast-math folding with NaN and
// infinities (x-x to 0, a saturated NaN to 1); real hardware follows the
// interpreter there.

// TestDXRandomALU compares random ALU programs (bit-exact instructions;
// TestDXRandomApprox does the rest).
func TestDXRandomALU(t *testing.T) { dxRandomALU(t, false) }

func TestDXRandomApprox(t *testing.T) { dxRandomALU(t, true) }

func dxRandomALU(t *testing.T, approx bool) {
	dxDevice(t)
	rng := rand.New(rand.NewSource(7))
	n := 300
	if s := os.Getenv("DXRANDN"); s != "" {
		fmt.Sscan(s, &n)
	}
	if testing.Short() {
		n = 50
	}
	saved := dxRandTrans
	defer func() { dxRandTrans = saved }()
	var exact []uint32
	for _, op := range saved {
		if dxApprox(op) == approx {
			exact = append(exact, op)
		}
	}
	dxRandTrans = exact
	tol := 1e-6
	if approx {
		tol = 2e-4
	}
	// run gives the first mismatch of a program on the inputs
	run := func(vs *shaderAsm, in [][]uint32) string {
		r := dxSimpleRunAny(vs, len(in[0])/4, 4)
		r.in = in
		got := r.d3d(t)
		want := r.interp(t)
		// the host may not flush denormals
		noFlush = true
		want2 := r.interp(t)
		noFlush, nanFlip = false, true
		want3 := r.interp(t)
		nanFlip = false
		for v := range want {
			for k := range want[v] {
				g := got[v][k]
				if !dxClose(g, want[v][k], tol) && !dxClose(g, want2[v][k], tol) && !dxClose(g, want3[v][k], tol) {
					return fmt.Sprintf("  vertex %d out %d.%c: got %v (%08x), want %v (%08x)\n  inputs %08x\n", v, k/4, "xyzw"[k%4],
						u2f(got[v][k]), got[v][k], u2f(want[v][k]), want[v][k], in[v])
				}
			}
		}
		return ""
	}
	fails, folded := 0, 0
	defer func() { t.Logf("%d programs differ only by host constant folding", folded) }()
	for i := 0; i < n && fails < 8; i++ {
		groups := 1 + rng.Intn(5)
		if approx {
			groups = 1
		}
		seed := rng.Int63()
		var in [][]uint32
		for v := 0; v < 8; v++ {
			w := make([]uint32, 32)
			for k := range w {
				w[k] = dxRandVals[rng.Intn(len(dxRandVals))]
			}
			in = append(in, w)
		}
		// mk gives the program cut down to its first g groups, and with
		// consts its literals and constants are read from registers (so
		// the host can't fold them: it may fold 0*NaN to 0, or not flush
		// constant denormals), with the inputs for that
		mk := func(g int, consts bool) (*shaderAsm, [][]uint32) {
			var c []uint32
			if consts {
				dxConstGPRs = &c
			}
			r2 := rand.New(rand.NewSource(seed))
			vs := dxRandALU(r2.Intn, groups)
			dxConstGPRs = nil
			cl := &vs.clauses[0]
			cut := 0
			for k := 0; k < g; k++ {
				cut = dxGroupEnd(cl.words, cut)
			}
			cl.words = cl.words[:cut]
			slots := uint32(len(cl.words) / 2)
			cf := &vs.cf[cl.cfIdx]
			cf[1] = cf[1]&^(0x7f<<18) | ((slots-1)&0x7f)<<18
			in2 := in
			if consts {
				in2 = nil
				for _, w := range in {
					in2 = append(in2, append(append([]uint32{}, w...), c...))
				}
			}
			return vs, in2
		}
		try := func(g int, consts bool) string { return run(mk(g, consts)) }
		if try(groups, false) == "" {
			continue
		}
		g := 1
		for ; g < groups; g++ {
			if try(g, false) != "" {
				break
			}
		}
		if try(g, true) == "" {
			folded++
			continue
		}
		fails++
		vs, in2 := mk(g, true)
		t.Errorf("program %d, fails after %d groups:\n%s%s", i, g, dxDescribe(vs), run(vs, in2))
		if os.Getenv("DXPROG") == fmt.Sprint(i) {
			r := dxSimpleRunAny(vs, 15, 4)
			cfg := &dxConfig{read32: r.read32, fsAddr: r.fs, nParams: 4, testPoints: 8, testGPRs: 15,
				texInfo: func(uint32) dxTexKey { return dxTexKey{} }, vbufInfo: func(uint32) dxFetchKey { return dxFetchKey{} }}
			p, _ := translateDX(cfg, r.vs)
			t.Log(d3d.Disasm(p.code))
		}
	}
}

// dxGroupEnd gives the word index after the ALU group starting at i
// (with its literals).
func dxGroupEnd(w []uint32, i int) int {
	lit, hi := false, false
	for i+1 < len(w) {
		w0, w1 := w[i], w[i+1]
		op3 := w1>>15&7 != 0
		srcs := []uint32{w0 & 0x1ff, w0 >> 13 & 0x1ff}
		chs := []uint32{w0 >> 10 & 3, w0 >> 23 & 3}
		if op3 {
			srcs = append(srcs, w1&0x1ff)
			chs = append(chs, w1>>10&3)
		}
		for k, sel := range srcs {
			if sel == 253 {
				lit = true
				if chs[k] >= 2 {
					hi = true
				}
			}
		}
		i += 2
		if w0>>31 != 0 {
			break
		}
	}
	if lit {
		i += 2
		if hi {
			i += 2
		}
	}
	return i
}

// dxDescribe lists a program's ALU instructions.
func dxDescribe(p *shaderAsm) string {
	s := ""
	for _, c := range p.clauses {
		if !c.alu {
			continue
		}
		w := c.words
		for i := 0; i < len(w); {
			end := dxGroupEnd(w, i)
			for j := i; j+1 < end; j += 2 {
				w0, w1 := w[j], w[j+1]
				op := aluOp{w0: w0, w1: w1, op3: w1>>15&7 != 0}
				if op.op3 {
					op.inst = w1 >> 13 & 0x1f
				} else {
					op.inst = w1 >> 7 & 0x7ff
				}
				srcs := (&shaderRun{}).srcs(op)
				s += fmt.Sprintf("  op3=%v %#x R%d.%c", op.op3, op.inst, w1>>21&0x7f, "xyzw"[w1>>29&3])
				for _, sr := range srcs {
					m := ""
					if sr.neg {
						m += "-"
					}
					if sr.abs {
						m += "|"
					}
					s += fmt.Sprintf(" %s%d.%c", m, sr.sel, "xyzw"[sr.ch])
				}
				if !op.op3 && w1>>5&3 != 0 {
					s += fmt.Sprintf(" omod%d", w1>>5&3)
				}
				if w1>>31 != 0 {
					s += " clamp"
				}
				s += "\n"
				if w0>>31 != 0 {
					break
				}
			}
			// literals
			last := i
			for j := i; j+1 < end; j += 2 {
				last = j + 2
				if w[j]>>31 != 0 {
					break
				}
			}
			if last < end {
				s += fmt.Sprintf("  literals %08x\n", w[last:end])
			}
			s += "  --\n"
			i = end
		}
	}
	return s
}

func dxAllClose(got, want [][]uint32, tol float64) bool {
	for v := range want {
		for k := range want[v] {
			if !dxClose(got[v][k], want[v][k], tol) {
				return false
			}
		}
	}
	return true
}

// TestDXOpTable runs each ALU instruction on every pair of special values
// and lists where Direct3D and the interpreter disagree (DXOPS=1 to run).
func TestDXOpTable(t *testing.T) {
	if os.Getenv("DXOPS") == "" {
		t.Skip("DXOPS not set")
	}
	dxDevice(t)
	vals := []uint32{0, 0x80000000, f2u(1), f2u(-1), f2u(0.5), f2u(-2.5), f2u(65536), 1, 7, 0xffffffff, 0x7fffffff,
		0x7f800000, 0xff800000, 0x7fc00000, f2u(1e-3), 0x3039}
	var pairs [][2]uint32
	for _, a := range vals {
		for _, b := range vals {
			pairs = append(pairs, [2]uint32{a, b})
		}
	}
	type variant struct {
		op, omod   uint32
		clamp, op3 bool
	}
	var ops []variant
	for _, op := range dxRandOp2 {
		ops = append(ops, variant{op: op})
		if op < 0x30 {
			ops = append(ops, variant{op: op, omod: 1, clamp: true}, variant{op: op, omod: 3})
		}
	}
	for _, op := range dxRandTrans {
		ops = append(ops, variant{op: op})
	}
	for _, op := range dxRandOp3 {
		ops = append(ops, variant{op: op, op3: true}, variant{op: op, op3: true, clamp: true})
	}
	for _, vr := range ops {
		op := vr.op
		lanes := uint32(4)
		if transOnly(op) {
			lanes = 1
		}
		if vr.op3 {
			lanes = 2
		}
		tol := 1e-6
		if dxApprox(op) {
			tol = 2e-4
		}
		vs := &shaderAsm{}
		var insts []aluInst
		for c := uint32(0); c < lanes; c++ {
			in := op2(op, 3, c, gprSrc(1, c), gprSrc(2, c))
			if vr.op3 {
				in = op3i(op, 3, c, gprSrc(1, c), gprSrc(2, c), gprSrc(1, c+2))
			}
			in.omod, in.clamp = vr.omod, vr.clamp
			insts = append(insts, in)
		}
		vs.ALU(8, [2]kcacheSpec{}, aluGroupSpec{insts: insts})
		vs.Export(false, 1, 60, 4, 1, [4]uint32{0, 1, 2, 3})
		vs.Export(true, 2, 0, 3, 1, [4]uint32{0, 1, 2, 3})
		vs.End()
		bad := 0
		for p := 0; p < len(pairs); {
			r := dxSimpleRunAny(vs, 4, 2)
			var used [][2]uint32
			var third []uint32
			for v := 0; v < 64 && p < len(pairs); v++ {
				w := make([]uint32, 16)
				for c := uint32(0); c < lanes && p < len(pairs); c++ {
					w[c], w[4+c] = pairs[p][0], pairs[p][1]
					if vr.op3 {
						w[c+2] = vals[(p*5+3)%len(vals)]
						third = append(third, w[c+2])
					}
					used = append(used, pairs[p])
					p++
				}
				r.in = append(r.in, w)
			}
			got := r.d3d(t)
			want := r.interp(t)
			noFlush = true
			want2 := r.interp(t)
			noFlush = false
			for i, pr := range used {
				v, c := i/int(lanes), 4+i%int(lanes)
				g, w, w2 := got[v][c], want[v][c], want2[v][c]
				if !dxClose(g, w, tol) && !dxClose(g, w2, tol) {
					if bad < 6 && vr.op3 {
						t.Errorf("op %+v (%08x, %08x, %08x): got %08x (%v), want %08x (%v)", vr, pr[0], pr[1], third[i], g, u2f(g), w, u2f(w))
					} else if bad < 6 {
						t.Errorf("op %+v (%08x, %08x): got %08x (%v), want %08x (%v)", vr, pr[0], pr[1], g, u2f(g), w, u2f(w))
					}
					bad++
				}
			}
		}
		if bad > 6 {
			t.Errorf("op %+v: %d more", vr, bad-6)
		}
	}
}
