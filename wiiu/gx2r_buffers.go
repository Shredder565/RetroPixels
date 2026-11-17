package wiiu

// More of GX2R: the resource flags for buffers, their alignment and size,
// and the display list and stream-out calls that take a GX2RBuffer.

// GX2RResourceFlags bits beyond the vertex, index and uniform bindings
const (
	gx2rBindShader     = 1 << 7
	gx2rBindStreamOut  = 1 << 8
	gx2rBindDisplayLst = 1 << 9
	gx2rBindRingBuffer = 1 << 10
	gx2rUsageCPURead   = 1 << 11
	gx2rUsageCPUWrite  = 1 << 12
	gx2rUsageGPURead   = 1 << 13
	gx2rUsageGPUWrite  = 1 << 14
	gx2rUsageMask      = gx2rUsageCPURead | gx2rUsageCPUWrite | gx2rUsageGPURead | gx2rUsageGPUWrite

	// the flags of a destroy call that reach the free callback (bits 19-23)
	gx2rFreeFlags = 0x1f << 19
)

// gx2rBufferAlignment is GX2RGetBufferAlignment: geometry and display lists
// need 64 bytes, everything else (shaders, uniform blocks, stream-out and
// ring buffers) 256.
func gx2rBufferAlignment(flags uint32) uint32 {
	const wide = gx2rBindShader | gx2rBindStreamOut | gx2rBindUniform | gx2rBindRingBuffer
	const narrow = gx2rBindVertex | gx2rBindIndex | gx2rBindDisplayLst
	if flags&wide == 0 && flags&narrow != 0 {
		return 64
	}
	return 256
}

// gx2rBytes is a buffer's element size times its element count.
func gx2rBytes(m *Machine, b uint32) uint32 {
	return m.Mem.Read32(b+gx2rBufElem) * m.Mem.Read32(b+gx2rBufCount)
}

// gx2rAllocSize is GX2RGetBufferAllocationSize: the bytes rounded up to a
// multiple of 64.
func gx2rAllocSize(m *Machine, b uint32) uint32 {
	return (gx2rBytes(m, b) + 63) &^ 63
}

func init() {
	const lib = "gx2"
	// forward runs another gx2 function with these arguments
	forward := func(name string, args func(m *Machine, cpu *CPU) []uint32) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a := args(m, cpu)
			for i, v := range a {
				cpu.GPR[3+i] = v
			}
			hleFuncs[lib][name](m, cpu, t)
		}
	}
	data := func(m *Machine, b uint32) uint32 { return m.Mem.Read32(b + gx2rBufData) }

	reg(lib, "GX2RSetStreamOutBuffer", forward("GX2SetStreamOutBuffer", func(m *Machine, cpu *CPU) []uint32 {
		return []uint32{arg(cpu, 0), arg(cpu, 1)}
	}))
	// GX2RBeginDisplayListEx(buffer, ...): record into the buffer's memory
	reg(lib, "GX2RBeginDisplayListEx", forward("GX2BeginDisplayList", func(m *Machine, cpu *CPU) []uint32 {
		b := arg(cpu, 0)
		return []uint32{data(m, b), gx2rBytes(m, b)}
	}))
	reg(lib, "GX2REndDisplayList", forward("GX2EndDisplayList", func(m *Machine, cpu *CPU) []uint32 {
		return []uint32{data(m, arg(cpu, 0))}
	}))
	// GX2R(Direct)CallDisplayList(buffer, bytes)
	reg(lib, "GX2RCallDisplayList", forward("GX2CallDisplayList", func(m *Machine, cpu *CPU) []uint32 {
		return []uint32{data(m, arg(cpu, 0)), arg(cpu, 1)}
	}))
	reg(lib, "GX2RDirectCallDisplayList", forward("GX2DirectCallDisplayList", func(m *Machine, cpu *CPU) []uint32 {
		return []uint32{data(m, arg(cpu, 0)), arg(cpu, 1)}
	}))
}
