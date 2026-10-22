package wiiu

// Latte: the Wii U's GPU.
//
// GX2 (the graphics library, implemented in Go in gx2.go) talks to the GPU
// the way the real one does: it writes PM4 command packets that set the
// GPU's registers and start draws. Here the packets run as soon as they're
// written (or are stored in memory when the program is recording a display
// list, and run when it's called). The registers then drive a software
// renderer: the shader interpreter (latte_shader.go), the rasterizer
// (latte_draw.go) and texture sampling (latte_tex.go).

import "fmt"

// register addresses (byte addresses, as in AMD's R600-family documents)
const (
	// config registers
	regVGT_PRIMITIVE_TYPE = 0x8958
	regVGT_INDEX_TYPE     = 0x895c
	regVGT_NUM_INDICES    = 0x8970
	regVGT_NUM_INSTANCES  = 0x8974

	// context registers
	regDB_DEPTH_SIZE                 = 0x28000
	regDB_DEPTH_VIEW                 = 0x28004
	regDB_DEPTH_BASE                 = 0x2800c
	regDB_DEPTH_INFO                 = 0x28010
	regDB_HTILE_DATA_BASE            = 0x28014
	regPA_SC_SCREEN_SCISSOR_TL       = 0x28030
	regPA_SC_SCREEN_SCISSOR_BR       = 0x28034
	regCB_COLOR0_BASE                = 0x28040
	regCB_COLOR0_SIZE                = 0x28060
	regCB_COLOR0_VIEW                = 0x28080
	regCB_COLOR0_INFO                = 0x280a0
	regCB_COLOR0_TILE                = 0x280c0
	regCB_COLOR0_FRAG                = 0x280e0
	regCB_COLOR0_MASK                = 0x28100
	regSQ_ALU_CONST_BUFFER_SIZE_PS_0 = 0x28140
	regSQ_ALU_CONST_BUFFER_SIZE_VS_0 = 0x28180
	regSQ_ALU_CONST_BUFFER_SIZE_GS_0 = 0x281c0
	regPA_SC_WINDOW_OFFSET           = 0x28200
	regPA_SC_WINDOW_SCISSOR_TL       = 0x28204
	regPA_SC_WINDOW_SCISSOR_BR       = 0x28208
	regCB_TARGET_MASK                = 0x28238
	regCB_SHADER_MASK                = 0x2823c
	regPA_SC_GENERIC_SCISSOR_TL      = 0x28240
	regPA_SC_GENERIC_SCISSOR_BR      = 0x28244
	regPA_SC_VPORT_SCISSOR_0_TL      = 0x28250
	regPA_SC_VPORT_SCISSOR_0_BR      = 0x28254
	regPA_SC_VPORT_ZMIN_0            = 0x282d0
	regPA_SC_VPORT_ZMAX_0            = 0x282d4
	regSQ_VTX_SEMANTIC_0             = 0x28380
	regVGT_INDX_OFFSET               = 0x28408
	regVGT_MULTI_PRIM_IB_RESET_INDX  = 0x2840c
	regSX_ALPHA_TEST_CONTROL         = 0x28410
	regCB_BLEND_RED                  = 0x28414
	regDB_STENCILREFMASK             = 0x28430
	regDB_STENCILREFMASK_BF          = 0x28434
	regSX_ALPHA_REF                  = 0x28438
	regPA_CL_VPORT_XSCALE_0          = 0x2843c
	regSPI_VS_OUT_ID_0               = 0x28614
	regSPI_PS_INPUT_CNTL_0           = 0x28644
	regSPI_VS_OUT_CONFIG             = 0x286c4
	regSPI_PS_IN_CONTROL_0           = 0x286cc
	regSPI_PS_IN_CONTROL_1           = 0x286d0
	regSPI_INTERP_CONTROL_0          = 0x286d4
	regSPI_INPUT_Z                   = 0x286d8
	regCB_BLEND0_CONTROL             = 0x28780
	regDB_DEPTH_CONTROL              = 0x28800
	regCB_BLEND_CONTROL              = 0x28804
	regCB_COLOR_CONTROL              = 0x28808
	regDB_SHADER_CONTROL             = 0x2880c
	regPA_CL_CLIP_CNTL               = 0x28810
	regPA_SU_SC_MODE_CNTL            = 0x28814
	regPA_CL_VTE_CNTL                = 0x28818
	regPA_CL_VS_OUT_CNTL             = 0x2881c
	regSQ_PGM_START_PS               = 0x28840
	regSQ_PGM_RESOURCES_PS           = 0x28850
	regSQ_PGM_EXPORTS_PS             = 0x28854
	regSQ_PGM_START_VS               = 0x28858
	regSQ_PGM_RESOURCES_VS           = 0x28868
	regSQ_PGM_START_GS               = 0x2886c
	regSQ_PGM_START_ES               = 0x28880
	regSQ_PGM_START_FS               = 0x28894
	regSQ_PGM_RESOURCES_FS           = 0x288a4
	regSQ_ALU_CONST_CACHE_PS_0       = 0x28940
	regSQ_ALU_CONST_CACHE_VS_0       = 0x28980
	regSQ_ALU_CONST_CACHE_GS_0       = 0x289c0
	regPA_SU_POINT_SIZE              = 0x28a00
	regPA_SU_POINT_MINMAX            = 0x28a04
	regPA_SU_LINE_CNTL               = 0x28a08
	regVGT_DMA_NUM_INSTANCES         = 0x28a88
	regVGT_MULTI_PRIM_IB_RESET_EN    = 0x28a94
	regVGT_INSTANCE_STEP_RATE_0      = 0x28aa0
	regVGT_INSTANCE_STEP_RATE_1      = 0x28aa4
	regPA_SC_AA_MASK                 = 0x28c48
	regPA_SU_POLY_OFFSET_DB_FMT_CNTL = 0x28df8
	regPA_SU_POLY_OFFSET_CLAMP       = 0x28dfc
	regPA_SU_POLY_OFFSET_FRONT_SCALE = 0x28e00
	regPA_SU_POLY_OFFSET_FRONT_OFFS  = 0x28e04
	regPA_SU_POLY_OFFSET_BACK_SCALE  = 0x28e08
	regPA_SU_POLY_OFFSET_BACK_OFFS   = 0x28e0c

	// constants, resources, samplers
	regSQ_ALU_CONSTANT0_0     = 0x30000
	regSQ_FETCH_RESOURCE      = 0x38000 // 7 words each
	regSQ_TEX_SAMPLER_WORD0_0 = 0x3c000 // 3 words each
	regSQ_VTX_BASE_VTX_LOC    = 0x3cff0
	regSQ_VTX_START_INST_LOC  = 0x3cff4
	regSQ_LOOP_CONST_0        = 0x3e200
	regSQ_BOOL_CONST_0        = 0x3e380
)

// the register spaces the SET_* packets write, from their base address
const (
	baseConfig   = 0x8000
	baseContext  = 0x28000
	baseALUConst = 0x30000
	baseResource = 0x38000
	baseSampler  = 0x3c000
	baseCtlConst = 0x3cff0
	baseLoop     = 0x3e200
	baseBool     = 0x3e380
	regTop       = 0x48000
)

// shader stages, and where each one's resources, samplers, constants start
const (
	stagePS = 0
	stageVS = 1
	stageGS = 2
)

var stageResBase = [3]uint32{0, 160, 336}
var stageSamplerBase = [3]uint32{0, 18, 36}
var stageConstBase = [3]uint32{0, 256, 512}

// RetroPixels' own PM4 packets, for the parts of GX2 that are easier to do
// directly than as draws (the real GX2 clears and copies with the GPU's
// draw engine). The GPU leaves these opcodes free.
const (
	itHLE_CLEAR_COLOR  = 0xe0 // CB base, size, info, view, r, g, b, a (float bits)
	itHLE_CLEAR_DEPTH  = 0xe1 // DB base, size, info, view, depth, stencil, flags
	itHLE_COPY_TO_SCAN = 0xe2 // CB base, size, info, view, width, height, target
	itHLE_COPY_SURFACE = 0xe3 // two packed surfaces (see packSurf)
	itHLE_DEPTH_TO_TEX = 0xe5 // DB surface then texture surface (packSurf)
	itHLE_TIMESTAMP    = 0xe6 // address: write the time there (64 bits)
)

func pm4Type3(op uint32, n int) uint32 {
	return 3<<30 | uint32(n-1)&0x3fff<<16 | op<<8
}

// Latte is the GPU.
type Latte struct {
	m    *Machine
	mem  *Memory
	regs [(regTop - baseConfig) / 4]uint32

	indexType    uint32 // VGT_DMA_INDEX_TYPE (from INDEX_TYPE)
	numInstances uint32
	ibDepth      int
	predicate    bool // SET_PREDICATION: draws depend on a query

	// CONTEXT_CONTROL, and for each register the memory a LOAD_* packet
	// read it from (where SET_* writes go too while shadowing is on)
	ctxControl [2]uint32
	shadowAddr [(regTop - baseConfig) / 4]uint32

	tex texCache
	hw  hwRenderer // a host GPU backend, or nil

	// counters
	Draws, Prims, Pixels uint64
	warned               map[string]bool
	// for tests: the last error a shader stopped with
	ShaderErr string
}

func newLatte(m *Machine) *Latte {
	g := &Latte{m: m, mem: m.Mem, numInstances: 1, warned: map[string]bool{}}
	g.tex.init()
	return g
}

func (g *Latte) reg(addr uint32) uint32 { return g.regs[(addr-baseConfig)/4] }

func (g *Latte) setReg(addr, v uint32) {
	if addr < baseConfig || addr >= regTop {
		return
	}
	g.regs[(addr-baseConfig)/4] = v
}

func (g *Latte) regF(addr uint32) float32 { return u2f(g.reg(addr)) }

func (g *Latte) warn(f string, a ...any) {
	s := fmt.Sprintf(f, a...)
	if g.warned[s] || len(g.warned) > 200 {
		return
	}
	g.warned[s] = true
	if g.m != nil {
		g.m.logf("gpu: %s", s)
	}
}

// hlePacket runs one of RetroPixels' own packets; false if op isn't one.
func (g *Latte) hlePacket(op uint32, d []uint32) bool {
	switch op {
	case itHLE_CLEAR_COLOR:
		if len(d) >= 8 {
			g.clearColor(cbFromRegs(d[0], d[1], d[2], d[3]), [4]float32{u2f(d[4]), u2f(d[5]), u2f(d[6]), u2f(d[7])})
		}
	case itHLE_CLEAR_DEPTH:
		if len(d) >= 7 {
			g.clearDepth(dbFromRegs(d[0], d[1], d[2], d[3]), u2f(d[4]), d[5], d[6])
		}
	case itHLE_COPY_TO_SCAN:
		if len(d) >= 7 {
			g.copyToScan(cbFromRegs(d[0], d[1], d[2], d[3]), d[4], d[5], d[6])
		}
	case itHLE_COPY_SURFACE, itHLE_DEPTH_TO_TEX:
		if len(d) >= 2*packedSurfWords {
			g.copySurface(unpackSurf(d), unpackSurf(d[packedSurfWords:]))
		}
	case itHLE_TIMESTAMP:
		if len(d) >= 1 && d[0] != 0 && g.m != nil {
			g.mem.Write64(d[0], g.m.bootTime+g.m.Time())
		}
	default:
		return false
	}
	return true
}

// ---- reading GPU memory ----

// the GPU reads memory as little-endian words
func (g *Latte) read32(addr uint32) uint32 { return bswap32(g.mem.Read32(addr)) }

func bswap32(v uint32) uint32 { return v>>24 | v>>8&0xff00 | v<<8&0xff0000 | v<<24 }
func bswap16(v uint16) uint16 { return v>>8 | v<<8 }

// indexFetcher reads indices for a draw from addr with VGT_DMA_INDEX_TYPE.
func (g *Latte) indexFetcher(addr uint32) func(uint32) uint32 {
	t := g.indexType
	is32 := t&3 == 1
	swap := t >> 2 & 3
	mem := g.mem
	if is32 {
		return func(i uint32) uint32 {
			v := mem.Read32(addr + i*4) // big-endian read
			if swap != 2 {              // stored little-endian
				v = bswap32(v)
			}
			return v
		}
	}
	return func(i uint32) uint32 {
		v := mem.Read16(addr + i*2)
		if swap != 1 {
			v = bswap16(v)
		}
		return uint32(v)
	}
}

// pm4 builds PM4 packets.
type pm4 struct{ w []uint32 }

func (b *pm4) packet(op uint32, d ...uint32) {
	b.w = append(b.w, pm4Type3(op, max(len(d), 1)))
	if len(d) == 0 {
		b.w = append(b.w, 0)
	}
	b.w = append(b.w, d...)
}

// set writes registers from addr on, with the SET_* packet for its range.
func (b *pm4) set(addr uint32, vals ...uint32) {
	var op, base uint32
	switch {
	case addr >= baseBool:
		op, base = itSET_BOOL_CONST, baseBool
	case addr >= baseLoop:
		op, base = itSET_LOOP_CONST, baseLoop
	case addr >= baseCtlConst:
		op, base = itSET_CTL_CONST, baseCtlConst
	case addr >= baseSampler:
		op, base = itSET_SAMPLER, baseSampler
	case addr >= baseResource:
		op, base = itSET_RESOURCE, baseResource
	case addr >= baseALUConst:
		op, base = itSET_ALU_CONST, baseALUConst
	case addr >= baseContext:
		op, base = itSET_CONTEXT_REG, baseContext
	default:
		op, base = itSET_CONFIG_REG, baseConfig
	}
	b.packet(op, append([]uint32{(addr - base) / 4}, vals...)...)
}
