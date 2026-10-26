// Original code for RetroPixels. Not derived from Cemu.

package wiiu

// GX2R resource flags beyond the vertex/index/uniform bindings, buffer
// alignment and size rules, and the GX2R wrappers that pass a GX2RBuffer's
// memory on to the plain GX2 display list and stream-out calls.

const (
	gx2rBindShader     = 1 << 7
	gx2rBindStreamOut  = 1 << 8
	gx2rBindDisplayLst = 1 << 9
	gx2rBindRingBuffer = 1 << 10

	gx2rUsageCPURead  = 1 << 11
	gx2rUsageCPUWrite = 1 << 12
	gx2rUsageGPURead  = 1 << 13
	gx2rUsageGPUWrite = 1 << 14
	gx2rUsageMask     = gx2rUsageCPURead | gx2rUsageCPUWrite | gx2rUsageGPURead | gx2rUsageGPUWrite

	// bits 19..23 of a destroy call's options are handed to the free callback
	gx2rFreeFlags = 0x1f << 19
)

// gx2rBufferAlignment is GX2RGetBufferAlignment. Buffers the shaders read
// as uniform blocks or rings, and stream-out targets, need 256 bytes;
// vertex, index and display list buffers 64; anything else 256.
func gx2rBufferAlignment(flags uint32) uint32 {
	const (
		big   = gx2rBindUniform | gx2rBindShader | gx2rBindStreamOut | gx2rBindRingBuffer
		small = gx2rBindVertex | gx2rBindIndex | gx2rBindDisplayLst
	)
	if flags&big == 0 && flags&small != 0 {
		return 0x40
	}
	return 0x100
}

// gx2rBufferBytes is a GX2RBuffer's element size times its element count.
func gx2rBufferBytes(m *Machine, buf uint32) uint32 {
	return m.Mem.Read32(buf+gx2rBufElem) * m.Mem.Read32(buf+gx2rBufCount)
}

// gx2rAllocSize is GX2RGetBufferAllocationSize: the buffer's bytes rounded
// up to a multiple of 64.
func gx2rAllocSize(m *Machine, buf uint32) uint32 {
	return (gx2rBufferBytes(m, buf) + 63) &^ 63
}

func init() {
	const x = "gx2"
	// forward runs the gx2 function name with args in r3 onward.
	forward := func(m *Machine, cpu *CPU, t *Thread, name string, args ...uint32) {
		copy(cpu.GPR[3:], args)
		hleFuncs[x][name](m, cpu, t)
	}
	data := func(m *Machine, buf uint32) uint32 { return m.Mem.Read32(buf + gx2rBufData) }

	// (buffer, index)
	reg(x, "GX2RSetStreamOutBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		forward(m, cpu, t, "GX2SetStreamOutBuffer", arg(cpu, 0), arg(cpu, 1))
	})
	// (buffer, unused, options): the whole buffer becomes the display list
	reg(x, "GX2RBeginDisplayListEx", func(m *Machine, cpu *CPU, t *Thread) {
		buf := arg(cpu, 0)
		forward(m, cpu, t, "GX2BeginDisplayList", data(m, buf), gx2rBufferBytes(m, buf))
	})
	reg(x, "GX2REndDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		forward(m, cpu, t, "GX2EndDisplayList", data(m, arg(cpu, 0)))
	})
	// (buffer, bytes)
	reg(x, "GX2RCallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		forward(m, cpu, t, "GX2CallDisplayList", data(m, arg(cpu, 0)), arg(cpu, 1))
	})
	reg(x, "GX2RDirectCallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		forward(m, cpu, t, "GX2DirectCallDisplayList", data(m, arg(cpu, 0)), arg(cpu, 1))
	})
}
