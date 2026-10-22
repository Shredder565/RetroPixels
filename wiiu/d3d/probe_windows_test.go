//go:build windows

package d3d

import (
	"encoding/binary"
	"math"
	"testing"
	"unsafe"
)

// the probe's shaders: the vertex shader reads its position from a buffer
// of words by vertex number and passes a constant colour on.
func probeShaders() (vs, ps []byte) {
	v := &Shader{
		In:  []Sig{{Name: "SV_VertexID", SV: SVVertexID, CompType: 1, Mask: 1}},
		Out: []Sig{{Name: "SV_Position", SV: SVPosition, Mask: 15}, {Name: "TEXCOORD", Reg: 1, Mask: 15}},
	}
	v.DclCB(0, 1, false)
	v.DclResource(0, DimBuffer, RetUint)
	v.DclInputSV(0, 1, SVVertexID)
	v.DclOutputSV(0, 15, SVPosition)
	v.DclOutput(1, 15)
	v.Temps(2)
	v.Ins(OpIshl, R(0).Mask(1), In(0).C(0), ImmU(1))
	v.Ins(OpLd, R(1).Mask(1), R(0).C(0), Tex(0).C(0))
	v.Ins(OpIadd, R(0).Mask(1), R(0).C(0), ImmU(1))
	v.Ins(OpLd, R(1).Mask(2), R(0).C(0), Tex(0).C(0))
	v.Ins(OpMov, Out(0).Mask(3), R(1).Swz(0, 1, 1, 1))
	v.Ins(OpMov, Out(0).Mask(12), ImmF(0, 0, 0.5, 1))
	v.Ins(OpMov, Out(1).Mask(15), CB(0, 0))
	v.Ins(OpRet)
	p := &Shader{Pixel: true,
		In:  []Sig{{Name: "SV_Position", SV: SVPosition, Mask: 15}, {Name: "TEXCOORD", Reg: 1, Mask: 15}},
		Out: []Sig{{Name: "SV_Target", SV: SVTarget, Mask: 15}},
	}
	p.DclInputPS(1, 15, InterpLinear)
	p.DclOutput(0, 15)
	p.Ins(OpMov, Out(0).Mask(15), In(1))
	p.Ins(OpRet)
	return v.Bytes(), p.Bytes()
}

func f32bytes(v ...float32) []byte {
	b := make([]byte, 4*len(v))
	for i, f := range v {
		binary.LittleEndian.PutUint32(b[i*4:], math.Float32bits(f))
	}
	return b
}

func probeDevice(t *testing.T) *Device {
	d, err := NewDevice(false)
	if err != nil {
		d, err = NewDevice(true)
	}
	if err != nil {
		t.Skip("no Direct3D 11:", err)
	}
	return d
}

// TestProbe draws one triangle offscreen and reads it back, which checks the
// COM vtable slots the wrapper uses.
func TestProbe(t *testing.T) {
	d := probeDevice(t)
	defer d.Close()
	const W, H = 64, 64
	rt, err := d.CreateTexture2D(&Texture2DDesc{Width: W, Height: H, MipLevels: 1, ArraySize: 1, Format: FormatRGBA8Unorm,
		SampleCount: 1, BindFlags: BindRenderTarget | BindShaderResource}, nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer rt.Release()
	rtv, err := d.CreateRTV(rt, FormatRGBA8Unorm)
	if err != nil {
		t.Fatal(err)
	}
	defer rtv.Release()
	vsCode, psCode := probeShaders()
	vs, err := d.CreateVertexShader(vsCode)
	if err != nil {
		t.Fatal(err)
	}
	defer vs.Release()
	ps, err := d.CreatePixelShader(psCode)
	if err != nil {
		t.Fatal(err)
	}
	defer ps.Release()
	// a triangle covering the left half of the target (clockwise)
	vb, err := d.CreateBuffer(&BufferDesc{ByteWidth: 24, BindFlags: BindShaderResource},
		f32bytes(-1, -1, -1, 3, 0, -1))
	if err != nil {
		t.Fatal(err)
	}
	defer vb.Release()
	vsrv, err := d.CreateBufferSRV(vb, FormatR32Uint, 0, 6)
	if err != nil {
		t.Fatal(err)
	}
	defer vsrv.Release()
	cb, err := d.CreateBuffer(&BufferDesc{ByteWidth: 16, Usage: UsageDynamic, BindFlags: BindConstantBuffer, CPUAccess: CPUWrite}, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer cb.Release()
	m, err := d.Ctx.Map(cb, 0, MapWriteDiscard)
	if err != nil {
		t.Fatal(err)
	}
	copy(unsafe.Slice((*byte)(ptr(m.Data)), 16), f32bytes(0, 1, 0, 1))
	d.Ctx.Unmap(cb, 0)

	c := d.Ctx
	c.OMSetRenderTargets([]View{rtv}, View{})
	c.ClearRenderTargetView(rtv, [4]float32{1, 0, 0, 1})
	c.RSSetViewports(Viewport{0, 0, W, H, 0, 1})
	c.IASetPrimitiveTopology(TopologyTriangleList)
	c.VSSetShader(vs)
	c.PSSetShader(ps)
	c.VSSetShaderResources(0, vsrv)
	c.VSSetConstantBuffers(0, cb)
	c.Draw(3, 0)

	st, err := d.CreateTexture2D(&Texture2DDesc{Width: W, Height: H, MipLevels: 1, ArraySize: 1, Format: FormatRGBA8Unorm,
		SampleCount: 1, Usage: UsageStaging, CPUAccess: CPURead}, nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer st.Release()
	c.CopyResource(st, rt)
	m, err = c.Map(st, 0, MapRead)
	if err != nil {
		t.Fatal(err)
	}
	px := func(x, y int) uint32 {
		return *(*uint32)(ptr(m.Data + uintptr(y)*uintptr(m.RowPitch) + uintptr(x)*4))
	}
	left, right := px(8, 32), px(56, 32)
	c.Unmap(st, 0)
	if left != 0xFF00FF00 || right != 0xFF0000FF {
		t.Fatalf("left %08x (want green ff00ff00), right %08x (want red ff0000ff)", left, right)
	}
}
