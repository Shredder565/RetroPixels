//go:build windows

package wiiu

// Runs translated shaders on Direct3D 11 and compares them with the shader
// interpreter. Skipped when there's no Direct3D 11 device (under Wine the
// tests run on its software renderer).

import (
	"encoding/binary"
	"math"
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

// interp runs the vertex shader in the interpreter.
func (r *dxVSRun) interp(t *testing.T) [][]uint32 {
	g := newTestGPU()
	for a, w := range r.mem {
		g.mem.Write32(a, bswap32(w))
	}
	e := &stageEnv{g: g, stage: stageVS, fsAddr: r.fs}
	out := make([][]uint32, len(r.in))
	for base := 0; base < len(r.in); base += nl {
		s := &shaderRun{env: e}
		var outs [nl]vtxOut
		for l := 0; l < nl && base+l < len(r.in); l++ {
			e.vtx[l] = &outs[l]
			s.valid[l] = true
			s.gpr[0][0][l] = uint32(base + l)
			for g := 0; g < r.nGPR; g++ {
				for c := 0; c < 4; c++ {
					s.gpr[1+g][c][l] = r.in[base+l][4*g+c]
				}
			}
		}
		s.run(r.vs)
		if s.Err != "" {
			t.Fatal(s.Err)
		}
		for l := 0; l < nl && base+l < len(r.in); l++ {
			o := make([]uint32, 4*r.nOut)
			for c := 0; c < 4; c++ {
				o[c] = f2u(outs[l].pos[c])
			}
			for p := 1; p < r.nOut; p++ {
				copy(o[4*p:4*p+4], outs[l].param[p-1][:])
			}
			out[base+l] = o
		}
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
