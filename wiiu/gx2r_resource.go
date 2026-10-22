// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at https://mozilla.org/MPL/2.0/.
//
// Based on Cemu (https://github.com/cemu-project/Cemu), src/Cafe/OS/libs/gx2/GX2_Resource.cpp
// and GX2_Resource.h, rewritten in Go for RetroPixels.

package wiiu

// GX2R resource flags.
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
	gx2rFreeFlags      = 0x1f << 19 // the destroy call's flags passed on to the free callback
)

// gx2rBufferAlignment is GX2RGetBufferAlignment.
func gx2rBufferAlignment(flags uint32) uint32 {
	switch {
	case flags&(gx2rBindStreamOut|gx2rBindUniform|gx2rBindShader|gx2rBindRingBuffer) != 0:
		return 0x100
	case flags&(gx2rBindVertex|gx2rBindIndex|gx2rBindDisplayLst) != 0:
		return 0x40
	}
	return 0x100
}

// gx2rAllocSize is GX2RGetBufferAllocationSize: the size padded to 64 bytes.
func gx2rAllocSize(m *Machine, b uint32) uint32 {
	return (m.Mem.Read32(b+gx2rBufElem)*m.Mem.Read32(b+gx2rBufCount) + 0x3f) &^ 0x3f
}

func init() {
	x := "gx2"
	// gx2Call runs a gx2 function with the given arguments.
	gx2Call := func(m *Machine, cpu *CPU, t *Thread, name string, args ...uint32) {
		for i, a := range args {
			cpu.GPR[3+i] = a
		}
		hleFuncs["gx2"][name](m, cpu, t)
	}
	reg(x, "GX2RSetStreamOutBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		gx2Call(m, cpu, t, "GX2SetStreamOutBuffer", arg(cpu, 0), arg(cpu, 1))
	})
	bufSize := func(m *Machine, b uint32) uint32 {
		return m.Mem.Read32(b+gx2rBufElem) * m.Mem.Read32(b+gx2rBufCount)
	}
	// (buffer, ?, flags)
	reg(x, "GX2RBeginDisplayListEx", func(m *Machine, cpu *CPU, t *Thread) {
		b := arg(cpu, 0)
		gx2Call(m, cpu, t, "GX2BeginDisplayList", m.Mem.Read32(b+gx2rBufData), bufSize(m, b))
	})
	reg(x, "GX2REndDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		gx2Call(m, cpu, t, "GX2EndDisplayList", m.Mem.Read32(arg(cpu, 0)+gx2rBufData))
	})
	// (buffer, size)
	reg(x, "GX2RCallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		gx2Call(m, cpu, t, "GX2CallDisplayList", m.Mem.Read32(arg(cpu, 0)+gx2rBufData), arg(cpu, 1))
	})
	reg(x, "GX2RDirectCallDisplayList", func(m *Machine, cpu *CPU, t *Thread) {
		gx2Call(m, cpu, t, "GX2DirectCallDisplayList", m.Mem.Read32(arg(cpu, 0)+gx2rBufData), arg(cpu, 1))
	})
}
