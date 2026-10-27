package ps3

// A stand-in for the host GPU: it keeps textures in Go and draws with the
// shader interpreter (dxbcvm_test.go), following Direct3D 11's rules, so
// the GPU renderer can be tested without one.

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"runtime"
	"sync"

	"retropixels/wiiu/d3d"
)

type fakeTex struct {
	w, h    int
	format  uint32
	kind    int
	px      []vec4    // colour: r, g, b, a
	z       []float32 // depth
	stencil []uint8
}

type fakeGPU struct {
	tex  map[gpuHandle]*fakeTex
	ps   map[gpuHandle][]dxOp
	next gpuHandle
}

func newFakeGPU() *fakeGPU {
	return &fakeGPU{tex: map[gpuHandle]*fakeTex{}, ps: map[gpuHandle][]dxOp{}}
}

func (fg *fakeGPU) handle() gpuHandle { fg.next++; return fg.next }

func (fg *fakeGPU) CreateTexture(w, h int, format uint32, kind int, data []byte, pitch int) (gpuHandle, error) {
	if w <= 0 || h <= 0 || w > 16384 || h > 16384 {
		return 0, fmt.Errorf("bad size %dx%d", w, h)
	}
	t := &fakeTex{w: w, h: h, format: format, kind: kind}
	if kind == gpuTexDepth {
		t.z = make([]float32, w*h)
		t.stencil = make([]uint8, w*h)
	} else {
		t.px = make([]vec4, w*h)
	}
	h2 := fg.handle()
	fg.tex[h2] = t
	if data != nil {
		fg.UpdateTexture(h2, data, pitch)
	}
	return h2, nil
}

func le32(b []byte) uint32 { return binary.LittleEndian.Uint32(b) }

func (fg *fakeGPU) UpdateTexture(h gpuHandle, data []byte, pitch int) {
	t := fg.tex[h]
	switch t.format {
	case dxgiBC1, dxgiBC2, dxgiBC3:
		rt := rsxTexture{w: t.w, h: t.h, data: data, ok: true}
		rt.fmt = map[uint32]uint32{dxgiBC1: texDXT1, dxgiBC2: texDXT23, dxgiBC3: texDXT45}[t.format]
		for y := 0; y < t.h; y++ {
			for x := 0; x < t.w; x++ {
				c := rt.dxtTexel(x, y)
				t.px[y*t.w+x] = vec4{c[1], c[2], c[3], c[0]}
			}
		}
		return
	}
	for y := 0; y < t.h; y++ {
		row := data[y*pitch:]
		for x := 0; x < t.w; x++ {
			i := y*t.w + x
			switch t.format {
			case d3d.FormatRGBA8Unorm:
				p := row[4*x:]
				t.px[i] = vec4{float32(p[0]) / 255, float32(p[1]) / 255, float32(p[2]) / 255, float32(p[3]) / 255}
			case d3d.FormatRGBA16Float:
				p := row[8*x:]
				for k := 0; k < 4; k++ {
					t.px[i][k] = halfToF32(binary.LittleEndian.Uint16(p[2*k:]))
				}
			case d3d.FormatRGBA32Float:
				p := row[16*x:]
				for k := 0; k < 4; k++ {
					t.px[i][k] = math.Float32frombits(le32(p[4*k:]))
				}
			case d3d.FormatR32Float:
				t.px[i] = vec4{math.Float32frombits(le32(row[4*x:])), 0, 0, 1}
			case d3d.FormatR24G8Typeless:
				v := le32(row[4*x:])
				t.z[i] = float32(v&0xffffff) / 0xffffff
				t.stencil[i] = uint8(v >> 24)
			case d3d.FormatR16Typeless:
				t.z[i] = float32(binary.LittleEndian.Uint16(row[2*x:])) / 0xffff
			}
		}
	}
}

func (fg *fakeGPU) ReadTexture(h gpuHandle) ([]byte, int, error) {
	t := fg.tex[h]
	bpp := 4
	switch t.format {
	case d3d.FormatRGBA16Float:
		bpp = 8
	case d3d.FormatRGBA32Float:
		bpp = 16
	case d3d.FormatR16Typeless:
		bpp = 2
	}
	out := make([]byte, t.w*t.h*bpp)
	for i := 0; i < t.w*t.h; i++ {
		o := out[i*bpp:]
		switch t.format {
		case d3d.FormatRGBA8Unorm:
			for k := 0; k < 4; k++ {
				o[k] = byte(u8round(t.px[i][k]))
			}
		case d3d.FormatRGBA16Float:
			for k := 0; k < 4; k++ {
				binary.LittleEndian.PutUint16(o[2*k:], f32ToHalf(t.px[i][k]))
			}
		case d3d.FormatRGBA32Float:
			for k := 0; k < 4; k++ {
				binary.LittleEndian.PutUint32(o[4*k:], math.Float32bits(t.px[i][k]))
			}
		case d3d.FormatR32Float:
			binary.LittleEndian.PutUint32(o, math.Float32bits(t.px[i][0]))
		case d3d.FormatR24G8Typeless:
			binary.LittleEndian.PutUint32(o, uint32(math.Round(float64(t.z[i])*0xffffff))|uint32(t.stencil[i])<<24)
		case d3d.FormatR16Typeless:
			binary.LittleEndian.PutUint16(o, uint16(math.Round(float64(t.z[i])*0xffff)))
		}
	}
	return out, t.w * bpp, nil
}

func u8round(x float32) uint32 {
	if !(x > 0) {
		return 0
	}
	if x >= 1 {
		return 255
	}
	return uint32(x*255 + 0.5)
}

func (fg *fakeGPU) ReadTextureLate(h gpuHandle) ([]byte, int, error) { return fg.ReadTexture(h) }

func (fg *fakeGPU) CopyTexture(dst, src gpuHandle) {
	d, s := fg.tex[dst], fg.tex[src]
	copy(d.px, s.px)
	copy(d.z, s.z)
	copy(d.stencil, s.stencil)
}

func (fg *fakeGPU) DepthToRGBA8(dst, src gpuHandle) error {
	d, s := fg.tex[dst], fg.tex[src]
	for i := range s.z {
		v := uint32(math.Round(float64(s.z[i]) * 0xffffff))
		d.px[i] = vec4{float32(v>>8&0xff) / 255, float32(v&0xff) / 255, 0, float32(v>>16) / 255}
	}
	return nil
}

func (fg *fakeGPU) Release(h gpuHandle) { delete(fg.tex, h); delete(fg.ps, h) }

func (fg *fakeGPU) CreatePS(code []byte) (gpuHandle, error) {
	p, err := dxParse(code)
	if err != nil {
		return 0, err
	}
	h := fg.handle()
	fg.ps[h] = p
	return h, nil
}

func (fg *fakeGPU) CreateVS(code []byte) (gpuHandle, error) { return fg.CreatePS(code) }

func (fg *fakeGPU) ClearTarget(h gpuHandle, rgba [4]float32) {
	t := fg.tex[h]
	c := vec4(rgba)
	if t.format == d3d.FormatRGBA8Unorm {
		for k := range c {
			c[k] = float32(u8round(c[k])) / 255
		}
	}
	for i := range t.px {
		t.px[i] = c
	}
}

func (fg *fakeGPU) ClearDepth(h gpuHandle, flags uint32, depth float32, stencil uint8) {
	t := fg.tex[h]
	for i := range t.z {
		if flags&d3d.ClearDepth != 0 {
			t.z[i] = depth
		}
		if flags&d3d.ClearStencil != 0 {
			t.stencil[i] = stencil
		}
	}
}

func (fg *fakeGPU) Close() {}

type fakeVert struct {
	pos  vec4 // window x, y, z and 1/w
	attr [gpuVertRegs]vec4
}

// Draw rasterises the triangles the way Direct3D 11 does (pixel centres,
// perspective-correct TEXCOORDs, the extra register screen-linear).
func (fg *fakeGPU) Draw(d *gpuDraw) error {
	if os.Getenv("PS3_NULLGPU") != "" { // draw nothing (to time the rest)
		return nil
	}
	ps := fg.ps[d.PS]
	if ps == nil {
		return fmt.Errorf("no pixel shader")
	}
	var rt, ds *fakeTex
	if d.Target != 0 {
		rt = fg.tex[d.Target]
	}
	if d.Depth != 0 {
		ds = fg.tex[d.Depth]
	}
	cb := make([][4]uint32, (len(d.PSConst)+3)/4)
	for i, v := range d.PSConst {
		cb[i/4][i%4] = math.Float32bits(v)
	}
	vert := func(i uint32) [gpuVertRegs]vec4 {
		var r [gpuVertRegs]vec4
		for k := 0; k < gpuVertRegs; k++ {
			copy(r[k][:], d.Verts[(int(i)*gpuVertRegs+k)*4:])
		}
		return r
	}
	if d.VS != 0 { // run the vertex shader
		vcb := make([][4]uint32, (len(d.VSConst)+3)/4)
		for i, v := range d.VSConst {
			vcb[i/4][i%4] = math.Float32bits(v)
		}
		vm := &dxVM{prog: fg.ps[d.VS], r: make([][4]uint32, vxTemps), cb: vcb}
		vm.ld = func(unit int, i uint32) [4]uint32 {
			var v [4]uint32
			for j := 0; j < 4; j++ {
				if int(i)*4+j < len(d.Verts) {
					v[j] = math.Float32bits(d.Verts[int(i)*4+j])
				}
			}
			return v
		}
		cache := map[uint32][gpuVertRegs]vec4{}
		vert = func(i uint32) [gpuVertRegs]vec4 {
			if v, ok := cache[i]; ok {
				return v
			}
			vm.in[0] = [4]uint32{i, 0, 0, 0}
			vm.run()
			var r [gpuVertRegs]vec4
			for k := 0; k < gpuVertRegs; k++ {
				r[k] = vec4{f(vm.out[k][0]), f(vm.out[k][1]), f(vm.out[k][2]), f(vm.out[k][3])}
			}
			cache[i] = r
			return r
		}
	}
	var tris [][3]fakeVert
	for i := 0; i+2 < len(d.Index); i += 3 {
		in := [3][gpuVertRegs]vec4{vert(d.Index[i]), vert(d.Index[i+1]), vert(d.Index[i+2])}
		poly := clipW(in[:])
		for k := 2; k < len(poly); k++ {
			tri := [3][gpuVertRegs]vec4{poly[0], poly[k-1], poly[k]}
			var fv [3]fakeVert
			for j := 0; j < 3; j++ {
				p := tri[j][0]
				iw := 1 / p[3]
				fv[j].pos = vec4{(p[0]*iw + 1) * float32(d.W) / 2, (1 - p[1]*iw) * float32(d.H) / 2, p[2] * iw, iw}
				fv[j].attr = tri[j]
			}
			tris = append(tris, fv)
		}
	}
	for _, tri := range tris {
		fg.raster(d, ps, cb, rt, ds, &tri)
	}
	return nil
}

// clipW clips a polygon to w > 0.
func clipW(in [][gpuVertRegs]vec4) [][gpuVertRegs]vec4 {
	const eps = 1e-5
	ok := true
	for _, v := range in {
		if !(v[0][3] > eps) {
			ok = false
		}
	}
	if ok {
		return in
	}
	var out [][gpuVertRegs]vec4
	for i := range in {
		p, q := in[i], in[(i+1)%len(in)]
		pw, qw := p[0][3], q[0][3]
		if pw > eps {
			out = append(out, p)
		}
		if (pw > eps) != (qw > eps) {
			t := (eps - pw) / (qw - pw)
			var o [gpuVertRegs]vec4
			for k := range o {
				for j := 0; j < 4; j++ {
					o[k][j] = p[k][j] + (q[k][j]-p[k][j])*t
				}
			}
			out = append(out, o)
		}
	}
	return out
}

func (fg *fakeGPU) raster(d *gpuDraw, ps []dxOp, cb [][4]uint32, rt, ds *fakeTex, t *[3]fakeVert) {
	a, b, c := &t[0], &t[1], &t[2]
	area := (b.pos[0]-a.pos[0])*(c.pos[1]-a.pos[1]) - (c.pos[0]-a.pos[0])*(b.pos[1]-a.pos[1])
	if area == 0 || area != area {
		return
	}
	cw := area > 0
	front := cw == (d.Raster.FrontCCW == 0)
	if d.Raster.Cull == d3d.CullFront && front || d.Raster.Cull == d3d.CullBack && !front {
		return
	}
	x0 := max(int(math.Floor(float64(min(a.pos[0], b.pos[0], c.pos[0])))), int(d.Scissor.Left), 0)
	x1 := min(int(math.Ceil(float64(max(a.pos[0], b.pos[0], c.pos[0])))), int(d.Scissor.Right)-1, d.W-1)
	y0 := max(int(math.Floor(float64(min(a.pos[1], b.pos[1], c.pos[1])))), int(d.Scissor.Top), 0)
	y1 := min(int(math.Ceil(float64(max(a.pos[1], b.pos[1], c.pos[1])))), int(d.Scissor.Bottom)-1, d.H-1)
	if x0 > x1 || y0 > y1 {
		return
	}
	inv := 1 / area
	rows := y1 - y0 + 1
	n := 1
	if rows*(x1-x0+1) > 4096 {
		n = runtime.NumCPU()
	}
	var wg sync.WaitGroup
	for k := 0; k < n; k++ {
		ya, yb := y0+rows*k/n, y0+rows*(k+1)/n-1
		wg.Add(1)
		go func() {
			defer wg.Done()
			fg.rows(d, ps, cb, rt, ds, a, b, c, inv, ya, yb, x0, x1)
		}()
	}
	wg.Wait()
}

func (fg *fakeGPU) rows(d *gpuDraw, ps []dxOp, cb [][4]uint32, rt, ds *fakeTex, a, b, c *fakeVert, inv float32, ya, yb, x0, x1 int) {
	vm := &dxVM{prog: ps, r: make([][4]uint32, 256), cb: cb}
	vm.sample = func(unit int, u, v float32) [4]float32 { return fg.sample(d, unit, u, v) }
	for py := ya; py <= yb; py++ {
		fy := float32(py) + 0.5
		for px := x0; px <= x1; px++ {
			fx := float32(px) + 0.5
			w0 := ((b.pos[0]-fx)*(c.pos[1]-fy) - (c.pos[0]-fx)*(b.pos[1]-fy)) * inv
			w1 := ((c.pos[0]-fx)*(a.pos[1]-fy) - (a.pos[0]-fx)*(c.pos[1]-fy)) * inv
			w2 := 1 - w0 - w1
			if w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6 {
				continue
			}
			z := w0*a.pos[2] + w1*b.pos[2] + w2*c.pos[2]
			z = min(max(z, 0), 1)
			i := py*d.W + px
			if ds != nil && d.DepthState.DepthEnable != 0 {
				zn := uint32(math.Round(float64(z) * 0xffffff))
				zo := uint32(math.Round(float64(ds.z[i]) * 0xffffff))
				if ds.format == d3d.FormatR16Typeless {
					zn = uint32(math.Round(float64(z) * 0xffff))
					zo = uint32(math.Round(float64(ds.z[i]) * 0xffff))
				}
				if !depthPass(d.DepthState.DepthFunc-1+0x200, zn, zo) {
					continue
				}
			}
			// inputs
			iw := w0*a.pos[3] + w1*b.pos[3] + w2*c.pos[3]
			rw := 1 / iw
			for k := range vm.in {
				vm.in[k] = [4]uint32{}
			}
			vm.in[0] = [4]uint32{fb(fx), fb(fy), fb(z), fb(iw)}
			for k := 1; k < dxPosReg; k++ {
				for j := 0; j < 4; j++ {
					v := (w0*a.attr[k][j]*a.pos[3] + w1*b.attr[k][j]*b.pos[3] + w2*c.attr[k][j]*c.pos[3]) * rw
					vm.in[k][j] = fb(v)
				}
			}
			for j := 0; j < 4; j++ {
				vm.in[dxPosReg][j] = fb(w0*a.attr[dxPosReg][j] + w1*b.attr[dxPosReg][j] + w2*c.attr[dxPosReg][j])
			}
			for k := range vm.r {
				vm.r[k] = [4]uint32{}
			}
			vm.killed = false
			vm.out = [24][4]uint32{}
			vm.run()
			if vm.killed {
				continue
			}
			if ds != nil && d.DepthState.DepthEnable != 0 && d.DepthState.DepthWriteMask != 0 {
				ds.z[i] = z
			}
			for t := 0; t < 4; t++ {
				rt, bl := rt, &d.Blend
				if t > 0 {
					if d.MRT[t-1] == 0 {
						break
					}
					rt, bl = fg.tex[d.MRT[t-1]], &d.MRTBlend[t-1]
				}
				if rt == nil || bl.WriteMask == 0 {
					continue
				}
				src := vec4{f(vm.out[t][0]), f(vm.out[t][1]), f(vm.out[t][2]), f(vm.out[t][3])}
				unorm := rt.format == d3d.FormatRGBA8Unorm
				if unorm {
					src = sat(src)
				}
				dst := rt.px[i]
				col := src
				if bl.Enable != 0 {
					col = fakeBlend(bl, src, dst, vec4(d.BlendFactor))
				}
				if unorm {
					for k := range col {
						col[k] = float32(u8round(col[k])) / 255
					}
				}
				for k := 0; k < 4; k++ {
					if bl.WriteMask>>k&1 != 0 {
						dst[k] = col[k]
					}
				}
				rt.px[i] = dst
			}
		}
	}
}

func fakeFactor(f uint32, s, d, k vec4, i int) float32 {
	switch f {
	case d3d.BlendZero:
		return 0
	case d3d.BlendOne:
		return 1
	case d3d.BlendSrcColor:
		return s[i]
	case d3d.BlendInvSrcColor:
		return 1 - s[i]
	case d3d.BlendSrcAlpha:
		return s[3]
	case d3d.BlendInvSrcAlpha:
		return 1 - s[3]
	case d3d.BlendDestAlpha:
		return d[3]
	case d3d.BlendInvDestAlpha:
		return 1 - d[3]
	case d3d.BlendDestColor:
		return d[i]
	case d3d.BlendInvDestColor:
		return 1 - d[i]
	case d3d.BlendSrcAlphaSat:
		if i == 3 {
			return 1
		}
		return min(s[3], 1-d[3])
	case d3d.BlendFactor:
		return k[i]
	case d3d.BlendInvBlendFactor:
		return 1 - k[i]
	}
	return 1
}

func fakeBlend(b *d3d.BlendTarget, s, d, k vec4) vec4 {
	var o vec4
	for i := 0; i < 4; i++ {
		sf, df, op := b.Src, b.Dst, b.Op
		if i == 3 {
			sf, df, op = b.SrcAlpha, b.DstAlpha, b.OpAlpha
		}
		x := s[i] * fakeFactor(sf, s, d, k, i)
		y := d[i] * fakeFactor(df, s, d, k, i)
		switch op {
		case d3d.BlendOpSub:
			o[i] = x - y
		case d3d.BlendOpRevSub:
			o[i] = y - x
		case d3d.BlendOpMin:
			o[i] = min(s[i], d[i])
		case d3d.BlendOpMax:
			o[i] = max(s[i], d[i])
		default:
			o[i] = x + y
		}
	}
	return o
}

// sample reads unit's texture with its sampler, bilinear or point.
func (fg *fakeGPU) sample(d *gpuDraw, unit int, u, v float32) [4]float32 {
	t := fg.tex[d.Tex[unit]]
	if t == nil {
		return [4]float32{}
	}
	s := &d.Samp[unit]
	fetch := func(x, y int) vec4 {
		xx, ok1 := fakeWrap(x, t.w, s.AddressU)
		yy, ok2 := fakeWrap(y, t.h, s.AddressV)
		if !ok1 || !ok2 {
			return vec4(s.Border)
		}
		i := yy*t.w + xx
		if t.z != nil {
			return vec4{t.z[i], 0, 0, 1}
		}
		return t.px[i]
	}
	x, y := u*float32(t.w), v*float32(t.h)
	if s.Filter == 0 {
		p := fetch(int(math.Floor(float64(x))), int(math.Floor(float64(y))))
		return [4]float32(p)
	}
	x -= 0.5
	y -= 0.5
	fx0, fy0 := float32(math.Floor(float64(x))), float32(math.Floor(float64(y)))
	ix, iy := int(fx0), int(fy0)
	ax, ay := x-fx0, y-fy0
	p00, p10, p01, p11 := fetch(ix, iy), fetch(ix+1, iy), fetch(ix, iy+1), fetch(ix+1, iy+1)
	var o [4]float32
	for k := 0; k < 4; k++ {
		top := p00[k] + (p10[k]-p00[k])*ax
		bot := p01[k] + (p11[k]-p01[k])*ax
		o[k] = top + (bot-top)*ay
	}
	return o
}

func fakeWrap(i, n int, mode uint32) (int, bool) {
	switch mode {
	case d3d.AddressWrap:
		return wrapCoord(i, n, 1)
	case d3d.AddressMirror:
		return wrapCoord(i, n, 2)
	case d3d.AddressBorder:
		return wrapCoord(i, n, 4)
	case d3d.AddressMirrorOnce:
		return wrapCoord(i, n, 6)
	}
	return wrapCoord(i, n, 3)
}
