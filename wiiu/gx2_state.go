package wiiu

// gx2 drawing: render targets, clears and copies, textures and samplers,
// shaders, uniforms, vertex buffers, draws, the fixed-function state,
// display lists and context states. Each call turns into the GPU register
// writes and packets it stands for.

import "math"

// GX2ColorBuffer / GX2DepthBuffer / GX2Texture fields (after the surface)
const (
	cbViewMip    = 0x74
	cbViewSlice  = 0x78
	cbViewSlices = 0x7c
	cbRegs       = 0x88

	dbViewMip    = 0x74
	dbViewSlice  = 0x78
	dbViewSlices = 0x7c
	dbClearDepth = 0x88
	dbClearSten  = 0x8c
	dbRegs       = 0x90

	texViewMip    = 0x74
	texViewMips   = 0x78
	texViewSlice  = 0x7c
	texViewSlices = 0x80
	texCompMap    = 0x84
	texRegs       = 0x88
)

// cbNumberType: CB NUMBER_TYPE from a GX2 surface format
func cbNumberType(f uint32) uint32 {
	switch f >> 8 & 0xf {
	case 1:
		return 1 // snorm
	case 2:
		return 4 // uint
	case 3:
		return 5 // sint
	case 4:
		return 6 // srgb
	case 8:
		return 7 // float
	}
	return 0
}

// colourRegs: CB_COLOR_BASE, SIZE, INFO, VIEW for a colour buffer
func (m *Machine) colourRegs(cb uint32) (base, size, info, view uint32, w, h uint32) {
	s := m.readSurface(cb)
	mip := m.Mem.Read32(cb + cbViewMip)
	lay, lw, lh := s.level(mip)
	size = (lay.Pitch/8-1)&0x3ff | ((lay.Pitch*lay.Height/64-1)&0xfffff)<<10
	info = (s.format&0x3f)<<2 | lay.Mode<<8 | cbNumberType(s.format)<<12
	slice := m.Mem.Read32(cb + cbViewSlice)
	n := max(m.Mem.Read32(cb+cbViewSlices), 1)
	view = slice&0x7ff | ((slice+n-1)&0x7ff)<<13
	return (lay.Base | lay.Swizzle) >> 8, size, info, view, lw, lh
}

func depthFormat(f uint32) uint32 {
	switch f {
	case 0x005: // UNORM_R16
		return 1
	case 0x80e: // FLOAT_R32
		return 6
	case 0x81c: // FLOAT_D24_S8 (32-bit float depth, 8-bit stencil)
		return 7
	case 0x811: // FLOAT_R24_X8? (24-bit float depth)
		return 5
	}
	return 3 // UNORM_R24_X8: 24-bit depth, 8-bit stencil
}

func (m *Machine) depthRegs(db uint32) (base, size, info, view uint32) {
	s := m.readSurface(db)
	s.use |= 4
	mip := m.Mem.Read32(db + dbViewMip)
	lay, _, _ := s.level(mip)
	size = (lay.Pitch/8-1)&0x3ff | ((lay.Pitch*lay.Height/64-1)&0xfffff)<<10
	info = depthFormat(s.format) | lay.Mode<<15
	slice := m.Mem.Read32(db + dbViewSlice)
	n := max(m.Mem.Read32(db+dbViewSlices), 1)
	view = slice&0x7ff | ((slice+n-1)&0x7ff)<<13
	return (lay.Base | lay.Swizzle) >> 8, size, info, view
}

// texWords: the resource words of a GX2Texture
func (m *Machine) texWords(tex uint32) [7]uint32 {
	s := m.readSurface(tex)
	mem := m.Mem
	info := s.info()
	l0 := info.Levels[0]
	pitch := s.pitch
	if pitch == 0 {
		pitch = l0.Pitch
	}
	if formatCompressed(s.format) {
		pitch *= 4
	}
	depth := s.d
	if s.dim == 3 { // cube
		depth = max(depth, 6)
	}
	mode := l0.Mode
	f := s.format
	w0 := s.dim&7 | mode<<3 | ((pitch/8-1)&0x7ff)<<8 | ((s.w-1)&0x1fff)<<19
	w1 := (s.h-1)&0x3fff | ((depth-1)&0x1fff)<<14 | (f&0x3f)<<26
	var comp, numFmt uint32
	switch f >> 8 & 0xf {
	case 1: // snorm
		comp = 0x55
	case 2: // uint
		numFmt = 1
	case 3: // sint
		comp, numFmt = 0x55, 1
	}
	degamma := b2u(f>>8&0xf == 4)
	cm := mem.Read32(tex + texCompMap)
	sel := [4]uint32{cm >> 24 & 7, cm >> 16 & 7, cm >> 8 & 7, cm & 7}
	firstMip := mem.Read32(tex + texViewMip)
	nMips := max(mem.Read32(tex+texViewMips), 1)
	firstSlice := mem.Read32(tex + texViewSlice)
	nSlices := max(mem.Read32(tex+texViewSlices), 1)
	w4 := comp | numFmt<<8 | degamma<<11 | sel[0]<<16 | sel[1]<<19 | sel[2]<<22 | sel[3]<<25 | (firstMip&0xf)<<28
	lastMip := min(firstMip+nMips-1, s.mips-1)
	w5 := lastMip&0xf | (firstSlice&0x1fff)<<4 | ((firstSlice+nSlices-1)&0x1fff)<<17
	w6 := uint32(2) << 30
	sw := uint32(0)
	if isMacroTiled(mode) {
		sw = s.swizzle & 0x700
	}
	w2 := (s.image | sw) >> 8
	w3 := (s.mipmaps | sw) >> 8
	return [7]uint32{w0, w1, w2, w3, w4, w5, w6}
}

// samplers: GX2Sampler is three register words
func samplerWord0(clamp [3]uint32, mag, min, z, mip uint32) uint32 {
	return clamp[0]&7 | (clamp[1]&7)<<3 | (clamp[2]&7)<<6 | (mag&7)<<9 | (min&7)<<12 | (z&3)<<15 | (mip&3)<<17
}

func lodFixed(v float32, signed bool) uint32 {
	x := int32(math.Round(float64(v) * 64))
	if signed {
		return uint32(x) & 0xfff
	}
	return uint32(max(min(x, 0x3ff), 0))
}

// attribute formats (GX2AttribFormat's low bits) to GPU formats
var attribFormats = [...]uint32{fmt8, fmt4_4, fmt16, fmt16F, fmt8_8, fmt32, fmt32F, fmt16_16, fmt16_16F,
	fmt10_11_11F, fmt8_8_8_8, fmt2_10_10_10, fmt32_32, fmt32_32F, fmt16x4, fmt16x4F, fmt32_32_32, fmt32_32_32F,
	fmt32x4, fmt32x4F}

// attribute component sizes (for the default byte swap)
func attribSwap(t uint32) uint32 {
	switch t {
	case 0, 1, 4, 0xa:
		return 0
	case 2, 3, 7, 8, 0xe, 0xf:
		return 1
	}
	return 2
}

// buildFetchShader writes a fetch shader for the attribute streams.
func (m *Machine) buildFetchShader(buf, count, attribs uint32) (size uint32, divisors []uint32) {
	mem := m.Mem
	p := &shaderAsm{}
	var fetches []vtxSpec
	divIndex := map[uint32]uint32{}
	for i := uint32(0); i < count; i++ {
		a := attribs + i*0x20
		loc := mem.Read32(a)
		buffer := mem.Read32(a + 4)
		offset := mem.Read32(a + 8)
		format := mem.Read32(a + 0xc)
		typ := mem.Read32(a + 0x10)
		div := mem.Read32(a + 0x14)
		mask := mem.Read32(a + 0x18)
		endian := mem.Read32(a + 0x1c)
		ft := format & 0x3f
		hw := uint32(fmt32x4F)
		if int(ft) < len(attribFormats) {
			hw = attribFormats[ft]
		}
		v := vtxSpec{semantic: true, semanticID: loc & 0xff, buffer: 160 + buffer, offset: offset, format: hw,
			dstSel: [4]uint32{mask >> 24 & 7, mask >> 16 & 7, mask >> 8 & 7, mask & 7}}
		if format&0x100 != 0 {
			v.numFormat = 1
		} else if format&0x800 != 0 && ft != 3 && ft != 6 && ft != 8 && ft != 9 && ft != 0xd && ft != 0xf && ft != 0x11 && ft != 0x13 {
			v.numFormat = 2
		}
		v.signed = format&0x200 != 0
		if endian == 3 || endian > 3 {
			endian = attribSwap(ft)
		}
		v.endian = endian
		if typ == 1 { // per instance
			v.fetchType = 1
			v.srcSel = 3 // R0.w: the instance
			if div > 1 {
				k, ok := divIndex[div]
				if !ok && len(divisors) < 2 {
					k = uint32(len(divisors))
					divIndex[div] = k
					divisors = append(divisors, div)
				}
				v.srcSel = 1 + k // R0.y or R0.z: instance / divisor
			}
		}
		fetches = append(fetches, v)
	}
	if len(divisors) > 0 {
		var groups []aluGroupSpec
		for k, d := range divisors {
			magic := uint32((uint64(1)<<32 + uint64(d) - 1) / uint64(d))
			groups = append(groups, aluGroupSpec{insts: []aluInst{op2(0x76, 0, uint32(1+k), gprSrc(0, 3), litSrc(0))}, lits: []uint32{magic}})
		}
		p.ALU(8, [2]kcacheSpec{}, groups...)
	}
	for len(fetches) > 0 {
		n := min(len(fetches), 16)
		p.VTX(fetches[:n]...)
		fetches = fetches[n:]
	}
	p.CF(cfRETURN, 0, 0, 0, 0)
	b := p.Bytes()
	mem.WriteBytes(buf, b)
	return uint32(len(b)), divisors
}

func fetchShaderSize(count uint32) uint32 {
	return 0x40 + 0x40 + count*16 + (count/16+2)*8 + 0x40
}

// drawing defaults, as after GX2Init
func (m *Machine) gx2DefaultState() {
	b := &pm4{}
	b.set(regCB_COLOR_CONTROL, 0xcc<<16|1<<7)
	b.set(regCB_TARGET_MASK, 0xffffffff)
	for i := uint32(0); i < 8; i++ {
		b.set(regCB_BLEND0_CONTROL+i*4, 1|1<<16) // one, zero
	}
	b.set(regDB_DEPTH_CONTROL, 1<<1|1<<2|3<<4)
	b.set(regDB_STENCILREFMASK, 0xffff00)
	b.set(regDB_STENCILREFMASK_BF, 0xffff00)
	b.set(regPA_SU_SC_MODE_CNTL, 0)
	b.set(regPA_CL_VTE_CNTL, 0x43f)
	b.set(regPA_CL_CLIP_CNTL, 0)
	b.set(regPA_SC_GENERIC_SCISSOR_TL, 1<<31, 8192|8192<<16)
	b.set(regPA_SC_WINDOW_SCISSOR_TL, 1<<31, 8192|8192<<16)
	b.set(regPA_SC_SCREEN_SCISSOR_TL, 0, 8192|8192<<16)
	b.set(regPA_SC_VPORT_SCISSOR_0_TL, 1<<31, 8192|8192<<16)
	b.set(regPA_SC_VPORT_ZMIN_0, f2u(0), f2u(1))
	b.set(regSPI_INTERP_CONTROL_0, 1)
	b.set(regPA_SU_POINT_SIZE, 8|8<<16)
	b.set(regPA_SU_LINE_CNTL, 8)
	b.set(regVGT_MULTI_PRIM_IB_RESET_EN, 0)
	b.set(regVGT_MULTI_PRIM_IB_RESET_INDX, 0xffffffff)
	b.set(regSX_ALPHA_TEST_CONTROL, 0)
	m.emit(b)
}

// one-call helpers
func (m *Machine) setRegs(addr uint32, v ...uint32) {
	b := &pm4{}
	b.set(addr, v...)
	m.emit(b)
}

func (m *Machine) packet(op uint32, d ...uint32) {
	b := &pm4{}
	b.packet(op, d...)
	m.emit(b)
}

// updateRegField changes some bits of a register (read from the GPU's
// current value, so it works while a display list records too).
func (m *Machine) setRegBits(addr, mask, v uint32) {
	m.setRegs(addr, m.gpu.reg(addr)&^mask|v&mask)
}

// packSurf: one level and slice of a GX2Surface for the copy packets
func (m *Machine) packSurf(s uint32, mip, slice uint32, depth bool) packedSurf {
	gs := m.readSurface(s)
	if depth {
		gs.use |= 4
	}
	lay, w, h := gs.level(mip)
	return packedSurf{lay: lay, format: gs.format, width: w, height: h, slice: slice}
}

func init() {
	x := "gx2"
	reg(x, "GX2SetDefaultState", func(m *Machine, cpu *CPU, t *Thread) { m.gx2DefaultState() })

	// ---- render targets ----
	reg(x, "GX2InitColorBufferRegs", func(m *Machine, cpu *CPU, t *Thread) {
		cb := arg(cpu, 0)
		_, size, info, view, _, _ := m.colourRegs(cb)
		m.Mem.Write32(cb+cbRegs, size)
		m.Mem.Write32(cb+cbRegs+4, info)
		m.Mem.Write32(cb+cbRegs+8, view)
		m.Mem.Write32(cb+cbRegs+12, 0)
		m.Mem.Write32(cb+cbRegs+16, 0)
	})
	reg(x, "GX2InitDepthBufferRegs", func(m *Machine, cpu *CPU, t *Thread) {
		db := arg(cpu, 0)
		_, size, info, view := m.depthRegs(db)
		m.Mem.Write32(db+dbRegs, size)
		m.Mem.Write32(db+dbRegs+4, view)
		m.Mem.Write32(db+dbRegs+8, info)
	})
	reg(x, "GX2SetColorBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		cb, target := arg(cpu, 0), arg(cpu, 1)
		if target > 7 {
			return
		}
		base, size, info, view, _, _ := m.colourRegs(cb)
		o := target * 4
		b := &pm4{}
		b.set(regCB_COLOR0_BASE+o, base)
		b.set(regCB_COLOR0_SIZE+o, size)
		b.set(regCB_COLOR0_INFO+o, info)
		b.set(regCB_COLOR0_VIEW+o, view)
		m.emit(b)
	})
	reg(x, "GX2SetDepthBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		base, size, info, view := m.depthRegs(arg(cpu, 0))
		b := &pm4{}
		b.set(regDB_DEPTH_SIZE, size, view)
		b.set(regDB_DEPTH_BASE, base, info)
		m.emit(b)
	})
	reg(x, "GX2SetColorBufferTarget", func(m *Machine, cpu *CPU, t *Thread) {})

	// ---- clears ----
	clearColor := func(m *Machine, cb uint32, c [4]float32) {
		if cb == 0 {
			return
		}
		base, size, info, view, _, _ := m.colourRegs(cb)
		first, last := view&0x7ff, view>>13&0x7ff
		for s := first; s <= last; s++ {
			m.packet(itHLE_CLEAR_COLOR, base, size, info, s, f2u(c[0]), f2u(c[1]), f2u(c[2]), f2u(c[3]))
		}
	}
	clearDepth := func(m *Machine, db uint32, z float32, st, flags uint32) {
		if db == 0 {
			return
		}
		base, size, info, view := m.depthRegs(db)
		first, last := view&0x7ff, view>>13&0x7ff
		for s := first; s <= last; s++ {
			m.packet(itHLE_CLEAR_DEPTH, base, size, info, s, f2u(z), st&0xff, flags)
		}
	}
	reg(x, "GX2ClearColor", func(m *Machine, cpu *CPU, t *Thread) {
		clearColor(m, arg(cpu, 0), [4]float32{argF32(cpu, 0), argF32(cpu, 1), argF32(cpu, 2), argF32(cpu, 3)})
	})
	reg(x, "GX2SetClearDepthStencil", func(m *Machine, cpu *CPU, t *Thread) {
		db := arg(cpu, 0)
		m.Mem.WriteF32(db+dbClearDepth, argF32(cpu, 0))
		m.Mem.Write32(db+dbClearSten, arg(cpu, 1))
	})
	reg(x, "GX2SetClearDepth", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.WriteF32(arg(cpu, 0)+dbClearDepth, argF32(cpu, 0)) })
	reg(x, "GX2SetClearStencil", func(m *Machine, cpu *CPU, t *Thread) { m.Mem.Write32(arg(cpu, 0)+dbClearSten, arg(cpu, 1)) })
	reg(x, "GX2ClearDepthStencilEx", func(m *Machine, cpu *CPU, t *Thread) {
		clearDepth(m, arg(cpu, 0), argF32(cpu, 0), arg(cpu, 1), arg(cpu, 2))
	})
	reg(x, "GX2ClearDepthStencil", func(m *Machine, cpu *CPU, t *Thread) {
		db := arg(cpu, 0)
		clearDepth(m, db, m.Mem.ReadF32(db+dbClearDepth), m.Mem.Read32(db+dbClearSten), arg(cpu, 1))
	})
	reg(x, "GX2ClearBuffersEx", func(m *Machine, cpu *CPU, t *Thread) {
		clearColor(m, arg(cpu, 0), [4]float32{argF32(cpu, 0), argF32(cpu, 1), argF32(cpu, 2), argF32(cpu, 3)})
		clearDepth(m, arg(cpu, 1), argF32(cpu, 4), arg(cpu, 2), arg(cpu, 3))
	})
	reg(x, "GX2ClearBuffers", func(m *Machine, cpu *CPU, t *Thread) {
		clearColor(m, arg(cpu, 0), [4]float32{argF32(cpu, 0), argF32(cpu, 1), argF32(cpu, 2), argF32(cpu, 3)})
		db := arg(cpu, 1)
		if db != 0 {
			clearDepth(m, db, m.Mem.ReadF32(db+dbClearDepth), m.Mem.Read32(db+dbClearSten), arg(cpu, 2))
		}
	})

	// ---- copies ----
	reg(x, "GX2CopyColorBufferToScanBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		cb := arg(cpu, 0)
		base, size, info, view, w, h := m.colourRegs(cb)
		m.packet(itHLE_COPY_TO_SCAN, base, size, info, view&0x7ff, w, h, arg(cpu, 1))
	})
	reg(x, "GX2CopySurface", func(m *Machine, cpu *CPU, t *Thread) {
		src := m.packSurf(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), false)
		dst := m.packSurf(arg(cpu, 3), arg(cpu, 4), arg(cpu, 5), false)
		m.packet(itHLE_COPY_SURFACE, append(src.pack(), dst.pack()...)...)
	})
	reg(x, "GX2CopySurfaceEx", func(m *Machine, cpu *CPU, t *Thread) {
		src := m.packSurf(arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), false)
		dst := m.packSurf(arg(cpu, 3), arg(cpu, 4), arg(cpu, 5), false)
		m.packet(itHLE_COPY_SURFACE, append(src.pack(), dst.pack()...)...)
	})
	reg(x, "GX2ResolveAAColorBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		cb := arg(cpu, 0)
		src := m.packSurf(cb, m.Mem.Read32(cb+cbViewMip), m.Mem.Read32(cb+cbViewSlice), false)
		dst := m.packSurf(arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), false)
		m.packet(itHLE_COPY_SURFACE, append(src.pack(), dst.pack()...)...)
	})
	reg(x, "GX2ConvertDepthBufferToTextureSurface", func(m *Machine, cpu *CPU, t *Thread) {
		db := arg(cpu, 0)
		src := m.packSurf(db, m.Mem.Read32(db+dbViewMip), m.Mem.Read32(db+dbViewSlice), true)
		dst := m.packSurf(arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), false)
		m.packet(itHLE_DEPTH_TO_TEX, append(src.pack(), dst.pack()...)...)
	})

	// ---- textures and samplers ----
	reg(x, "GX2InitTextureRegs", func(m *Machine, cpu *CPU, t *Thread) {
		tex := arg(cpu, 0)
		w := m.texWords(tex)
		for i, v := range []uint32{w[0], w[1], w[4], w[5], w[6]} {
			m.Mem.Write32(tex+texRegs+uint32(i)*4, v)
		}
	})
	setTex := func(stage int) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			tex, unit := arg(cpu, 0), arg(cpu, 1)
			if tex == 0 || unit > 17 {
				return
			}
			w := m.texWords(tex)
			// the program's own register words (from GX2InitTextureRegs or
			// a texture file) win for everything but the addresses
			r := func(i uint32) uint32 { return m.Mem.Read32(tex + texRegs + i*4) }
			if r(4)>>30 == 2 {
				w[0], w[1], w[4], w[5], w[6] = r(0), r(1), r(2), r(3), r(4)
			}
			m.setRegs(regSQ_FETCH_RESOURCE+(stageResBase[stage]+unit)*28, w[:]...)
		}
	}
	reg(x, "GX2SetPixelTexture", setTex(stagePS))
	reg(x, "GX2SetVertexTexture", setTex(stageVS))
	reg(x, "GX2SetGeometryTexture", setTex(stageGS))
	samp := func(f func(m *Machine, cpu *CPU, s uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) { f(m, cpu, arg(cpu, 0)) }
	}
	reg(x, "GX2InitSampler", samp(func(m *Machine, cpu *CPU, s uint32) {
		c, f := arg(cpu, 1), arg(cpu, 2)
		m.Mem.Write32(s, samplerWord0([3]uint32{c, c, c}, f, f, 0, 1))
		m.Mem.Write32(s+4, lodFixed(0, false)|lodFixed(15, false)<<10)
		m.Mem.Write32(s+8, 1<<31)
	}))
	reg(x, "GX2InitSamplerClamping", samp(func(m *Machine, cpu *CPU, s uint32) {
		v := m.Mem.Read32(s)
		m.Mem.Write32(s, v&^0x1ff|arg(cpu, 1)&7|(arg(cpu, 2)&7)<<3|(arg(cpu, 3)&7)<<6)
	}))
	reg(x, "GX2InitSamplerXYFilter", samp(func(m *Machine, cpu *CPU, s uint32) {
		mag, min, aniso := arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		if aniso != 0 {
			mag, min = mag|2, min|2
		}
		v := m.Mem.Read32(s)
		m.Mem.Write32(s, v&^(0x3f<<9)&^(7<<19)|(mag&7)<<9|(min&7)<<12|(aniso&7)<<19)
	}))
	reg(x, "GX2InitSamplerZMFilter", samp(func(m *Machine, cpu *CPU, s uint32) {
		v := m.Mem.Read32(s)
		m.Mem.Write32(s, v&^(0xf<<15)|(arg(cpu, 1)&3)<<15|(arg(cpu, 2)&3)<<17)
	}))
	reg(x, "GX2InitSamplerLOD", samp(func(m *Machine, cpu *CPU, s uint32) {
		m.Mem.Write32(s+4, lodFixed(argF32(cpu, 0), false)|lodFixed(argF32(cpu, 1), false)<<10|lodFixed(argF32(cpu, 2), true)<<20)
	}))
	reg(x, "GX2InitSamplerLODAdjust", samp(func(m *Machine, cpu *CPU, s uint32) {}))
	reg(x, "GX2InitSamplerBorderType", samp(func(m *Machine, cpu *CPU, s uint32) {
		v := m.Mem.Read32(s)
		m.Mem.Write32(s, v&^(3<<22)|(arg(cpu, 1)&3)<<22)
	}))
	reg(x, "GX2InitSamplerDepthCompare", samp(func(m *Machine, cpu *CPU, s uint32) {
		v := m.Mem.Read32(s)
		m.Mem.Write32(s, v&^(7<<26)|(arg(cpu, 1)&7)<<26)
	}))
	reg(x, "GX2InitSamplerFilterAdjust", samp(func(m *Machine, cpu *CPU, s uint32) {}))
	reg(x, "GX2InitSamplerRoundingMode", samp(func(m *Machine, cpu *CPU, s uint32) {}))
	setSampler := func(stage int) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			s, unit := arg(cpu, 0), arg(cpu, 1)
			if s == 0 || unit > 17 {
				return
			}
			m.setRegs(regSQ_TEX_SAMPLER_WORD0_0+(stageSamplerBase[stage]+unit)*12, m.Mem.Read32(s), m.Mem.Read32(s+4), m.Mem.Read32(s+8))
		}
	}
	reg(x, "GX2SetPixelSampler", setSampler(stagePS))
	reg(x, "GX2SetVertexSampler", setSampler(stageVS))
	reg(x, "GX2SetGeometrySampler", setSampler(stageGS))
	reg(x, "GX2SetSamplerBorderColor", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2SetPixelSamplerBorderColor", func(m *Machine, cpu *CPU, t *Thread) {})

	// ---- shaders ----
	reg(x, "GX2CalcFetchShaderSizeEx", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, fetchShaderSize(arg(cpu, 0))) })
	reg(x, "GX2CalcFetchShaderSize", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, fetchShaderSize(arg(cpu, 0))) })
	initFS := func(m *Machine, cpu *CPU) {
		fs, buf, count, attribs := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		size, divs := m.buildFetchShader(buf, count, attribs)
		mem := m.Mem
		mem.Write32(fs, arg(cpu, 4))
		mem.Write32(fs+4, 2) // SQ_PGM_RESOURCES_FS: GPRs
		mem.Write32(fs+8, size)
		mem.Write32(fs+0xc, buf)
		mem.Write32(fs+0x10, count)
		mem.Write32(fs+0x14, uint32(len(divs)))
		for i := 0; i < 2; i++ {
			d := uint32(0)
			if i < len(divs) {
				d = divs[i]
			}
			mem.Write32(fs+0x18+uint32(i)*4, d)
		}
	}
	reg(x, "GX2InitFetchShaderEx", func(m *Machine, cpu *CPU, t *Thread) { initFS(m, cpu) })
	reg(x, "GX2InitFetchShader", func(m *Machine, cpu *CPU, t *Thread) { initFS(m, cpu) })
	reg(x, "GX2SetFetchShader", func(m *Machine, cpu *CPU, t *Thread) {
		fs := arg(cpu, 0)
		mem := m.Mem
		b := &pm4{}
		b.set(regSQ_PGM_START_FS, mem.Read32(fs+0xc)>>8)
		b.set(regSQ_PGM_RESOURCES_FS, mem.Read32(fs+4))
		b.set(regVGT_INSTANCE_STEP_RATE_0, mem.Read32(fs+0x18), mem.Read32(fs+0x1c))
		m.emit(b)
	})
	reg(x, "GX2SetVertexShader", func(m *Machine, cpu *CPU, t *Thread) {
		vs := arg(cpu, 0)
		mem := m.Mem
		r := func(i uint32) uint32 { return mem.Read32(vs + i*4) }
		b := &pm4{}
		prog := mem.Read32(vs + 0xd4)
		b.set(regSQ_PGM_START_VS, prog>>8)
		b.set(regSQ_PGM_RESOURCES_VS, r(0))
		b.set(regSPI_VS_OUT_CONFIG, r(2))
		ids := make([]uint32, 10)
		for i := uint32(0); i < 10; i++ {
			ids[i] = 0xffffffff
			if i < r(3) {
				ids[i] = r(4 + i)
			}
		}
		b.set(regSPI_VS_OUT_ID_0, ids...)
		b.set(regPA_CL_VS_OUT_CNTL, r(14))
		sem := make([]uint32, 32)
		for i := uint32(0); i < 32; i++ {
			sem[i] = 0xff
			if i < r(16) {
				sem[i] = r(17 + i)
			}
		}
		b.set(regSQ_VTX_SEMANTIC_0, sem...)
		// loop constants
		n, lv := mem.Read32(vs+0xf4), mem.Read32(vs+0xf8)
		for i := uint32(0); i < n && i < 32; i++ {
			off, val := mem.Read32(lv+i*8), mem.Read32(lv+i*8+4)
			b.set(regSQ_LOOP_CONST_0+(32+off/4%32)*4, val)
		}
		m.emit(b)
	})
	reg(x, "GX2SetPixelShader", func(m *Machine, cpu *CPU, t *Thread) {
		ps := arg(cpu, 0)
		mem := m.Mem
		r := func(i uint32) uint32 { return mem.Read32(ps + i*4) }
		b := &pm4{}
		b.set(regSQ_PGM_START_PS, mem.Read32(ps+0xa8)>>8)
		b.set(regSQ_PGM_RESOURCES_PS, r(0), r(1))
		b.set(regSPI_PS_IN_CONTROL_0, r(2), r(3))
		in := make([]uint32, 32)
		for i := uint32(0); i < 32; i++ {
			if i < r(4) {
				in[i] = r(5 + i)
			}
		}
		b.set(regSPI_PS_INPUT_CNTL_0, in...)
		b.set(regCB_SHADER_MASK, r(37))
		b.set(regDB_SHADER_CONTROL, r(39))
		b.set(regSPI_INPUT_Z, r(40))
		n, lv := mem.Read32(ps+0xc8), mem.Read32(ps+0xcc)
		for i := uint32(0); i < n && i < 32; i++ {
			off, val := mem.Read32(lv+i*8), mem.Read32(lv+i*8+4)
			b.set(regSQ_LOOP_CONST_0+(off/4%32)*4, val)
		}
		m.emit(b)
	})
	reg(x, "GX2SetGeometryShader", func(m *Machine, cpu *CPU, t *Thread) {
		if arg(cpu, 0) != 0 {
			m.warnOnce("gx2: geometry shaders aren't supported yet")
		}
	})

	// ---- uniforms ----
	uniformReg := func(stage int) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			off, count, vals := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
			count = min(count, 1024)
			v := make([]uint32, count)
			for i := range v {
				v[i] = m.Mem.Read32(vals + uint32(i)*4)
			}
			if len(v) > 0 {
				m.setRegs(regSQ_ALU_CONSTANT0_0+(stageConstBase[stage]*4+off)*4, v...)
			}
		}
	}
	reg(x, "GX2SetVertexUniformReg", uniformReg(stageVS))
	reg(x, "GX2SetPixelUniformReg", uniformReg(stagePS))
	uniformBlock := func(stage int) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			loc, size, data := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
			if loc > 15 {
				return
			}
			b := &pm4{}
			b.set(kcacheRegs[stage]+loc*4, data>>8)
			sizeRegs := [3]uint32{regSQ_ALU_CONST_BUFFER_SIZE_PS_0, regSQ_ALU_CONST_BUFFER_SIZE_VS_0, regSQ_ALU_CONST_BUFFER_SIZE_GS_0}
			b.set(sizeRegs[stage]+loc*4, (size+255)>>8)
			b.set(regSQ_FETCH_RESOURCE+(stageResBase[stage]+128+loc)*28, data, max(size, 1)-1, 16<<8|fmt32x4F<<20, 0, 0, 0, 3<<30)
			m.emit(b)
		}
	}
	reg(x, "GX2SetVertexUniformBlock", uniformBlock(stageVS))
	reg(x, "GX2SetPixelUniformBlock", uniformBlock(stagePS))
	reg(x, "GX2SetGeometryUniformBlock", uniformBlock(stageGS))

	// ---- vertex data and draws ----
	reg(x, "GX2SetAttribBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		idx, size, stride, buf := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		if idx > 15 {
			return
		}
		m.setRegs(regSQ_FETCH_RESOURCE+(stageResBase[stageVS]+160+idx)*28, buf, max(size, 1)-1, (stride&0x7ff)<<8, 0, 0, 0, 3<<30)
	})
	drawAuto := func(m *Machine, mode, count, first, inst, baseInst uint32) {
		b := &pm4{}
		b.set(regVGT_PRIMITIVE_TYPE, mode)
		b.set(regSQ_VTX_BASE_VTX_LOC, first, baseInst)
		b.packet(itNUM_INSTANCES, max(inst, 1))
		b.packet(itDRAW_INDEX_AUTO, count, 2)
		m.emit(b)
	}
	drawIndexed := func(m *Machine, mode, count, typ, idx, baseVtx, inst, baseInst uint32) {
		b := &pm4{}
		b.set(regVGT_PRIMITIVE_TYPE, mode)
		b.set(regSQ_VTX_BASE_VTX_LOC, baseVtx, baseInst)
		b.packet(itINDEX_TYPE, typ)
		b.packet(itNUM_INSTANCES, max(inst, 1))
		b.packet(itDRAW_INDEX_2, count, idx, 0, count, 0)
		m.emit(b)
	}
	reg(x, "GX2DrawEx", func(m *Machine, cpu *CPU, t *Thread) {
		drawAuto(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), 0)
	})
	reg(x, "GX2DrawEx2", func(m *Machine, cpu *CPU, t *Thread) {
		drawAuto(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4))
	})
	reg(x, "GX2Draw", func(m *Machine, cpu *CPU, t *Thread) { drawAuto(m, arg(cpu, 0), arg(cpu, 1), 0, 1, 0) })
	reg(x, "GX2DrawIndexedEx", func(m *Machine, cpu *CPU, t *Thread) {
		drawIndexed(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5), 0)
	})
	reg(x, "GX2DrawIndexedEx2", func(m *Machine, cpu *CPU, t *Thread) {
		drawIndexed(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5), arg(cpu, 6))
	})
	reg(x, "GX2DrawIndexed", func(m *Machine, cpu *CPU, t *Thread) {
		drawIndexed(m, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), 0, 1, 0)
	})
	reg(x, "GX2DrawIndexedImmediateEx", func(m *Machine, cpu *CPU, t *Thread) {
		mode, count, typ, idx, baseVtx, inst := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5)
		count = min(count, 1<<16)
		// the indices go in the packet, as native 16 or 32-bit values
		fetch := (&Latte{mem: m.Mem, indexType: typ}).indexFetcher(idx)
		var data []uint32
		if typ&3 == 1 {
			for i := uint32(0); i < count; i++ {
				data = append(data, fetch(i))
			}
		} else {
			for i := uint32(0); i < count; i += 2 {
				v := fetch(i)
				if i+1 < count {
					v |= fetch(i+1) << 16
				}
				data = append(data, v)
			}
		}
		b := &pm4{}
		b.set(regVGT_PRIMITIVE_TYPE, mode)
		b.set(regSQ_VTX_BASE_VTX_LOC, baseVtx, 0)
		b.packet(itINDEX_TYPE, typ&3)
		b.packet(itNUM_INSTANCES, max(inst, 1))
		b.packet(itDRAW_INDEX_IMMD, append([]uint32{count, 1}, data...)...)
		m.emit(b)
	})

	// ---- fixed-function state ----
	reg(x, "GX2SetViewport", func(m *Machine, cpu *CPU, t *Thread) {
		x0, y0, w, h, n, f := argF32(cpu, 0), argF32(cpu, 1), argF32(cpu, 2), argF32(cpu, 3), argF32(cpu, 4), argF32(cpu, 5)
		b := &pm4{}
		b.set(regPA_CL_VPORT_XSCALE_0, f2u(w/2), f2u(x0+w/2), f2u(-h/2), f2u(y0+h/2), f2u((f-n)/2), f2u((f+n)/2))
		b.set(regPA_SC_VPORT_ZMIN_0, f2u(min(n, f)), f2u(max(n, f)))
		m.emit(b)
	})
	reg(x, "GX2SetViewportReg", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2SetScissor", func(m *Machine, cpu *CPU, t *Thread) {
		x0, y0, w, h := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		m.setRegs(regPA_SC_GENERIC_SCISSOR_TL, x0&0x7fff|(y0&0x7fff)<<16|1<<31, (x0+w)&0x7fff|((y0+h)&0x7fff)<<16)
	})
	reg(x, "GX2SetColorControl", func(m *Machine, cpu *CPU, t *Thread) {
		rop, mask, multi, write := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		if write == 0 {
			rop = 0xaa // leave the target as it is
		}
		m.setRegs(regCB_COLOR_CONTROL, b2u(multi != 0)<<1|1<<7|(mask&0xff)<<8|(rop&0xff)<<16)
	})
	reg(x, "GX2SetBlendControl", func(m *Machine, cpu *CPU, t *Thread) {
		target := arg(cpu, 0)
		if target > 7 {
			return
		}
		v := arg(cpu, 1)&0x1f | (arg(cpu, 3)&7)<<5 | (arg(cpu, 2)&0x1f)<<8 |
			(arg(cpu, 5)&0x1f)<<16 | (arg(cpu, 7)&7)<<21 | (arg(cpu, 6)&0x1f)<<24 | b2u(arg(cpu, 4) != 0)<<29
		m.setRegs(regCB_BLEND0_CONTROL+target*4, v)
	})
	reg(x, "GX2SetBlendConstantColor", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regCB_BLEND_RED, f2u(argF32(cpu, 0)), f2u(argF32(cpu, 1)), f2u(argF32(cpu, 2)), f2u(argF32(cpu, 3)))
	})
	reg(x, "GX2SetDepthStencilControl", func(m *Machine, cpu *CPU, t *Thread) {
		a := func(i int) uint32 { return stackArg(m, cpu, i) }
		v := b2u(a(3) != 0) | b2u(a(0) != 0)<<1 | b2u(a(1) != 0)<<2 | (a(2)&7)<<4 | b2u(a(4) != 0)<<7 |
			(a(5)&7)<<8 | (a(8)&7)<<11 | (a(6)&7)<<14 | (a(7)&7)<<17 |
			(a(9)&7)<<20 | (a(12)&7)<<23 | (a(10)&7)<<26 | (a(11)&7)<<29
		m.setRegs(regDB_DEPTH_CONTROL, v)
	})
	reg(x, "GX2SetDepthOnlyControl", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regDB_DEPTH_CONTROL, b2u(arg(cpu, 0) != 0)<<1|b2u(arg(cpu, 1) != 0)<<2|(arg(cpu, 2)&7)<<4)
	})
	reg(x, "GX2SetStencilMask", func(m *Machine, cpu *CPU, t *Thread) {
		f := arg(cpu, 2)&0xff | (arg(cpu, 0)&0xff)<<8 | (arg(cpu, 1)&0xff)<<16
		bk := arg(cpu, 5)&0xff | (arg(cpu, 3)&0xff)<<8 | (arg(cpu, 4)&0xff)<<16
		m.setRegs(regDB_STENCILREFMASK, f, bk)
	})
	reg(x, "GX2SetPolygonControl", func(m *Machine, cpu *CPU, t *Thread) {
		v := (arg(cpu, 1) & 1) | (arg(cpu, 2)&1)<<1 | (arg(cpu, 0)&1)<<2 | (arg(cpu, 3)&1)<<3 |
			(arg(cpu, 4)&7)<<5 | (arg(cpu, 5)&7)<<8 | (arg(cpu, 6)&1)<<11 | (arg(cpu, 7)&1)<<12 | (stackArg(m, cpu, 8)&1)<<13
		m.setRegs(regPA_SU_SC_MODE_CNTL, v)
	})
	reg(x, "GX2SetCullOnlyControl", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegBits(regPA_SU_SC_MODE_CNTL, 7, (arg(cpu, 1)&1)|(arg(cpu, 2)&1)<<1|(arg(cpu, 0)&1)<<2)
	})
	reg(x, "GX2SetPolygonOffset", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regPA_SU_POLY_OFFSET_FRONT_SCALE, f2u(argF32(cpu, 1)*16), f2u(argF32(cpu, 0)), f2u(argF32(cpu, 3)*16), f2u(argF32(cpu, 2)))
		m.setRegs(regPA_SU_POLY_OFFSET_CLAMP, f2u(argF32(cpu, 4)))
	})
	reg(x, "GX2SetAlphaTest", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regSX_ALPHA_TEST_CONTROL, arg(cpu, 1)&7|b2u(arg(cpu, 0) != 0)<<3)
		m.setRegs(regSX_ALPHA_REF, f2u(argF32(cpu, 0)))
	})
	reg(x, "GX2SetAlphaToMask", func(m *Machine, cpu *CPU, t *Thread) {})
	reg(x, "GX2SetTargetChannelMasks", func(m *Machine, cpu *CPU, t *Thread) {
		var v uint32
		for i := 0; i < 8; i++ {
			v |= (arg(cpu, i) & 15) << (4 * i)
		}
		m.setRegs(regCB_TARGET_MASK, v)
	})
	reg(x, "GX2SetPrimitiveRestartIndex", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regVGT_MULTI_PRIM_IB_RESET_INDX, arg(cpu, 0))
		m.setRegs(regVGT_MULTI_PRIM_IB_RESET_EN, 1)
	})
	reg(x, "GX2SetLineWidth", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regPA_SU_LINE_CNTL, uint32(argF32(cpu, 0)*8)&0xffff)
	})
	reg(x, "GX2SetPointSize", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regPA_SU_POINT_SIZE, uint32(argF32(cpu, 1)*8)&0xffff|(uint32(argF32(cpu, 0)*8)&0xffff)<<16)
	})
	reg(x, "GX2SetPointLimits", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regPA_SU_POINT_MINMAX, uint32(argF32(cpu, 0)*8)&0xffff|(uint32(argF32(cpu, 1)*8)&0xffff)<<16)
	})
	reg(x, "GX2SetRasterizerClipControl", func(m *Machine, cpu *CPU, t *Thread) {
		ras, zclip := arg(cpu, 0) != 0, arg(cpu, 1) != 0
		m.setRegBits(regPA_CL_CLIP_CNTL, 1<<22|3<<26, b2u(!ras)<<22|b2u(!zclip)*3<<26)
	})
	reg(x, "GX2SetRasterizerClipControlEx", func(m *Machine, cpu *CPU, t *Thread) {
		ras, zclip, half := arg(cpu, 0) != 0, arg(cpu, 1) != 0, arg(cpu, 2) != 0
		m.setRegBits(regPA_CL_CLIP_CNTL, 1<<19|1<<22|3<<26, b2u(half)<<19|b2u(!ras)<<22|b2u(!zclip)*3<<26)
	})
	reg(x, "GX2SetRasterizerClipControlHalfZ", func(m *Machine, cpu *CPU, t *Thread) {
		ras, zclip, half := arg(cpu, 0) != 0, arg(cpu, 1) != 0, arg(cpu, 2) != 0
		m.setRegBits(regPA_CL_CLIP_CNTL, 1<<19|1<<22|3<<26, b2u(half)<<19|b2u(!ras)<<22|b2u(!zclip)*3<<26)
	})

	// ---- caches ----
	reg(x, "GX2Invalidate", func(m *Machine, cpu *CPU, t *Thread) {
		addr, size := arg(cpu, 1), arg(cpu, 2)
		if size == 0xffffffff || addr == 0 && size == 0 {
			m.gpu.tex.clear()
			return
		}
		m.gpu.tex.invalidate(addr, addr+size)
	})

	// ---- display lists ----
	begin := func(m *Machine, cpu *CPU) {
		g := m.gx2
		g.dlActive, g.dlBuf, g.dlSize, g.dlPos, g.dlOverflow = true, arg(cpu, 0), arg(cpu, 1), 0, false
	}
	reg(x, "GX2BeginDisplayList", func(m *Machine, cpu *CPU, t *Thread) { begin(m, cpu) })
	reg(x, "GX2BeginDisplayListEx", func(m *Machine, cpu *CPU, t *Thread) { begin(m, cpu) })
	reg(x, "GX2EndDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		g := m.gx2
		if !g.dlActive {
			ret(cpu, 0)
			return
		}
		// pad to 32 bytes with filler, as the GPU fetches lists in blocks
		for g.dlPos%32 != 0 && g.dlPos+4 <= g.dlSize {
			m.Mem.Write32(g.dlBuf+g.dlPos, 0x80000000)
			g.dlPos += 4
		}
		g.dlActive = false
		ret(cpu, g.dlPos)
	})
	callDL := func(m *Machine, cpu *CPU) {
		buf, size := arg(cpu, 0), arg(cpu, 1)
		if m.gx2.dlActive {
			m.packet(itINDIRECT_BUFFER_PR, buf, 0, size/4)
			return
		}
		m.gpu.execMem(buf, size/4)
	}
	reg(x, "GX2CallDisplayList", func(m *Machine, cpu *CPU, t *Thread) { callDL(m, cpu) })
	reg(x, "GX2DirectCallDisplayList", func(m *Machine, cpu *CPU, t *Thread) { callDL(m, cpu) })
	reg(x, "GX2CopyDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		buf, size := arg(cpu, 0), arg(cpu, 1)
		if m.gx2.dlActive {
			b := &pm4{}
			for i := uint32(0); i < size/4; i++ {
				b.w = append(b.w, m.Mem.Read32(buf+i*4))
			}
			m.emit(b)
			return
		}
		m.gpu.execMem(buf, size/4)
	})
	reg(x, "GX2GetCurrentDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		g := m.gx2
		if p := arg(cpu, 0); p != 0 {
			m.Mem.Write32(p, g.dlBuf)
		}
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, g.dlSize)
		}
		retBool(cpu, g.dlActive)
	})
	reg(x, "GX2GetDisplayListWriteStatus", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, m.gx2.dlActive) })

	// ---- context states ----
	reg(x, "GX2SetupContextStateEx", func(m *Machine, cpu *CPU, t *Thread) {
		m.gpu.setupContext(arg(cpu, 0))
		m.gx2DefaultState()
	})
	reg(x, "GX2SetupContextState", func(m *Machine, cpu *CPU, t *Thread) {
		m.gpu.setupContext(arg(cpu, 0))
		m.gx2DefaultState()
	})
	reg(x, "GX2SetContextState", func(m *Machine, cpu *CPU, t *Thread) {
		m.packet(itHLE_CONTEXT_STATE, arg(cpu, 0))
	})
	reg(x, "GX2GetContextStateDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		if p := arg(cpu, 1); p != 0 {
			m.Mem.Write32(p, 0)
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write32(p, 0)
		}
	})
}
