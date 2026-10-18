package wiiu

// GX2R: GX2's resource helpers. A GX2RBuffer is { flags, element size,
// element count, memory }; GX2R allocates the memory with the program's
// allocator (GX2RSetAllocator) or the default heap, and the lock and
// unlock calls are where the CPU's writes become visible to the GPU.

const (
	gx2rBindVertex    = 1 << 4
	gx2rBindIndex     = 1 << 5
	gx2rBindUniform   = 1 << 6
	gx2rNoCPUInval    = 1 << 20
	gx2rDestroyNoFree = 1 << 23
	gx2rUserMemory    = 1 << 29 // RetroPixels' mark: the program gave the memory
	gx2rLocked        = 1 << 30

	gx2rBufFlags = 0x0
	gx2rBufElem  = 0x4
	gx2rBufCount = 0x8
	gx2rBufData  = 0xc
)

func init() {
	x := "gx2"
	reg(x, "GX2RSetAllocator", func(m *Machine, cpu *CPU, t *Thread) {
		m.gx2.rAlloc, m.gx2.rFree = arg(cpu, 0), arg(cpu, 1)
	})
	reg(x, "GX2RIsUserMemory", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, b2u(arg(cpu, 0)&gx2rUserMemory != 0))
	})
	// alloc gets memory for a resource and hands it to then (0 if none)
	alloc := func(m *Machine, cpu *CPU, t *Thread, flags, size, align uint32, then func(m *Machine, cpu *CPU, t *Thread, p uint32)) {
		if m.gx2.rAlloc != 0 {
			m.callGuest(cpu, t, m.gx2.rAlloc, []uint32{flags, size, align}, func(m *Machine, cpu *CPU, t *Thread, p uint32) {
				then(m, cpu, t, p)
			})
			return
		}
		m.callGuest(cpu, t, m.defaultHeapFunc("MEMAllocFromDefaultHeapEx"), []uint32{size, align}, then)
	}
	free := func(m *Machine, cpu *CPU, t *Thread, flags, p uint32, then func(m *Machine, cpu *CPU, t *Thread)) {
		if p == 0 {
			then(m, cpu, t)
			return
		}
		if m.gx2.rFree != 0 {
			m.callGuest(cpu, t, m.gx2.rFree, []uint32{flags, p}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) { then(m, cpu, t) })
			return
		}
		m.callGuest(cpu, t, m.defaultHeapFunc("MEMFreeToDefaultHeap"), []uint32{p}, func(m *Machine, cpu *CPU, t *Thread, _ uint32) { then(m, cpu, t) })
	}
	bufSize := func(m *Machine, b uint32) uint32 {
		return m.Mem.Read32(b+gx2rBufElem) * m.Mem.Read32(b+gx2rBufCount)
	}
	const bufAlign = 0x100
	reg(x, "GX2RGetBufferAlignment", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, bufAlign) })
	reg(x, "GX2RGetBufferAllocationSize", func(m *Machine, cpu *CPU, t *Thread) {
		ret(cpu, (bufSize(m, arg(cpu, 0))+bufAlign-1)&^(bufAlign-1))
	})
	reg(x, "GX2RCreateBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		b := arg(cpu, 0)
		flags := m.Mem.Read32(b + gx2rBufFlags)
		size := (bufSize(m, b) + bufAlign - 1) &^ (bufAlign - 1)
		alloc(m, cpu, t, flags, size, bufAlign, func(m *Machine, cpu *CPU, t *Thread, p uint32) {
			m.Mem.Write32(b+gx2rBufData, p)
			if p != 0 {
				m.Mem.Write32(b+gx2rBufFlags, flags&^(gx2rUserMemory|gx2rLocked))
			}
			ret(cpu, b2u(p != 0))
		})
	})
	reg(x, "GX2RCreateBufferUserMemory", func(m *Machine, cpu *CPU, t *Thread) {
		b, p := arg(cpu, 0), arg(cpu, 1)
		m.Mem.Write32(b+gx2rBufData, p)
		m.Mem.Write32(b+gx2rBufFlags, m.Mem.Read32(b+gx2rBufFlags)|gx2rUserMemory)
		ret(cpu, b2u(p != 0))
	})
	reg(x, "GX2RDestroyBufferEx", func(m *Machine, cpu *CPU, t *Thread) {
		b, extra := arg(cpu, 0), arg(cpu, 1)
		flags := m.Mem.Read32(b+gx2rBufFlags) | extra
		p := m.Mem.Read32(b + gx2rBufData)
		m.Mem.Write32(b+gx2rBufData, 0)
		if flags&(gx2rUserMemory|gx2rDestroyNoFree) != 0 {
			return
		}
		free(m, cpu, t, flags, p, func(m *Machine, cpu *CPU, t *Thread) {})
	})
	reg(x, "GX2RBufferExists", func(m *Machine, cpu *CPU, t *Thread) {
		b := arg(cpu, 0)
		ret(cpu, b2u(b != 0 && m.Mem.Read32(b+gx2rBufData) != 0))
	})
	reg(x, "GX2RLockBufferEx", func(m *Machine, cpu *CPU, t *Thread) {
		b := arg(cpu, 0)
		m.Mem.Write32(b+gx2rBufFlags, m.Mem.Read32(b+gx2rBufFlags)|gx2rLocked)
		ret(cpu, m.Mem.Read32(b+gx2rBufData))
	})
	reg(x, "GX2RUnlockBufferEx", func(m *Machine, cpu *CPU, t *Thread) {
		b, extra := arg(cpu, 0), arg(cpu, 1)
		flags := m.Mem.Read32(b+gx2rBufFlags) &^ gx2rLocked
		m.Mem.Write32(b+gx2rBufFlags, flags)
		if (flags|extra)&gx2rNoCPUInval == 0 {
			p := m.Mem.Read32(b + gx2rBufData)
			m.gpuWritten(p, bufSize(m, b))
		}
	})
	reg(x, "GX2RInvalidateBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		b, off, size := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
		p := m.Mem.Read32(b + gx2rBufData)
		if size == 0xffffffff {
			size = bufSize(m, b) - off
		}
		m.gpuWritten(p+off, size)
	})
	reg(x, "GX2RSetAttributeBuffer", func(m *Machine, cpu *CPU, t *Thread) {
		b, idx, stride, off := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		if idx > 15 {
			return
		}
		p := m.Mem.Read32(b + gx2rBufData)
		size := bufSize(m, b) - off
		m.setRegs(regSQ_FETCH_RESOURCE+(stageResBase[stageVS]+160+idx)*28, p+off, max(size, 1)-1, (stride&0x7ff)<<8, 0, 0, 0, 3<<30)
	})
	uniform := func(name string) {
		reg(x, "GX2RSet"+name+"UniformBlock", func(m *Machine, cpu *CPU, t *Thread) {
			b, loc, off := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2)
			cpu.GPR[3], cpu.GPR[4], cpu.GPR[5] = loc, bufSize(m, b)-off, m.Mem.Read32(b+gx2rBufData)+off
			hleFuncs["gx2"]["GX2Set"+name+"UniformBlock"](m, cpu, t)
		})
	}
	uniform("Vertex")
	uniform("Pixel")
	uniform("Geometry")
	reg(x, "GX2RDrawIndexed", func(m *Machine, cpu *CPU, t *Thread) {
		// (mode, buffer, index type, count, first index, base vertex, instances)
		mode, b, typ, count, first, base, inst := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5), arg(cpu, 6)
		size := uint32(2)
		if typ&3 == 1 || typ == 9 {
			size = 4
		}
		p := m.Mem.Read32(b+gx2rBufData) + first*size
		cpu.GPR[3], cpu.GPR[4], cpu.GPR[5], cpu.GPR[6], cpu.GPR[7], cpu.GPR[8] = mode, count, typ, p, base, max(inst, 1)
		hleFuncs["gx2"]["GX2DrawIndexedEx"](m, cpu, t)
	})
	reg(x, "GX2RSetStreamOutBuffer", func(m *Machine, cpu *CPU, t *Thread) {})

	// surfaces: GX2RCreateSurface works out the size and allocates the image
	// and mip levels together
	reg(x, "GX2RCreateSurface", func(m *Machine, cpu *CPU, t *Thread) {
		s, flags := arg(cpu, 0), arg(cpu, 1)
		m.Mem.Write32(s+surfUse, flags)
		m.calcSurface(s)
		size, mips := m.Mem.Read32(s+surfSize), m.Mem.Read32(s+surfMipSize)
		align := max(m.Mem.Read32(s+surfAlign), 0x100)
		alloc(m, cpu, t, flags, size+mips, align, func(m *Machine, cpu *CPU, t *Thread, p uint32) {
			m.Mem.Write32(s+surfImage, p)
			if p != 0 && mips != 0 {
				m.Mem.Write32(s+surfMipmaps, p+size)
			} else {
				m.Mem.Write32(s+surfMipmaps, 0)
			}
			ret(cpu, b2u(p != 0))
		})
	})
	reg(x, "GX2RCreateSurfaceUserMemory", func(m *Machine, cpu *CPU, t *Thread) {
		s, img, mips, flags := arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3)
		m.Mem.Write32(s+surfUse, flags|gx2rUserMemory)
		m.calcSurface(s)
		m.Mem.Write32(s+surfImage, img)
		m.Mem.Write32(s+surfMipmaps, mips)
		ret(cpu, b2u(img != 0))
	})
	reg(x, "GX2RDestroySurfaceEx", func(m *Machine, cpu *CPU, t *Thread) {
		s, extra := arg(cpu, 0), arg(cpu, 1)
		flags := m.Mem.Read32(s+surfUse) | extra
		p := m.Mem.Read32(s + surfImage)
		m.Mem.Write32(s+surfImage, 0)
		m.Mem.Write32(s+surfMipmaps, 0)
		if flags&(gx2rUserMemory|gx2rDestroyNoFree) != 0 {
			return
		}
		free(m, cpu, t, flags, p, func(m *Machine, cpu *CPU, t *Thread) {})
	})
	reg(x, "GX2RSurfaceExists", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0)
		ret(cpu, b2u(s != 0 && m.Mem.Read32(s+surfImage) != 0))
	})
	reg(x, "GX2RLockSurfaceEx", func(m *Machine, cpu *CPU, t *Thread) {
		s, level := arg(cpu, 0), int32(arg(cpu, 1))
		if level > 0 {
			ret(cpu, m.Mem.Read32(s+surfMipmaps)+m.Mem.Read32(s+surfMipOffs+uint32(level-1)*4))
			return
		}
		ret(cpu, m.Mem.Read32(s+surfImage))
	})
	reg(x, "GX2RUnlockSurfaceEx", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0)
		m.gpuWritten(m.Mem.Read32(s+surfImage), m.Mem.Read32(s+surfSize))
		if p := m.Mem.Read32(s + surfMipmaps); p != 0 {
			m.gpuWritten(p, m.Mem.Read32(s+surfMipSize))
		}
	})
	reg(x, "GX2RInvalidateSurface", func(m *Machine, cpu *CPU, t *Thread) {
		s := arg(cpu, 0)
		m.gpuWritten(m.Mem.Read32(s+surfImage), m.Mem.Read32(s+surfSize))
	})
}

// gpuWritten: the CPU wrote [p, p+size); copies the GPU side holds are old.
func (m *Machine) gpuWritten(p, size uint32) {
	if p == 0 || size == 0 {
		return
	}
	m.gpu.tex.invalidate(p, p+size)
	m.gpu.hwInvalidate(p, p+size)
}
