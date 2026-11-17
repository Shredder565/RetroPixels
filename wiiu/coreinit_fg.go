package wiiu

// The foreground bucket: 64 MB at FGBase shared by the foreground program
// and the system. The program may use the first 40 MB as it likes; the rest
// is split into areas the system keeps for itself (copy data such as the
// clipboard, title switch parameters, transition sound and pictures), ending
// with a 4 KB save area.

const fgFreeSize = 40 << 20

// the system's save area, the last 4 KB of the bucket
const fgSaveArea = FGBase + FGSize - 0x1000

func (m *Machine) fgFreeArea() (addr, size uint32) { return FGBase, fgFreeSize }

// initForegroundBucket marks the save area as set up: the bucket starts out
// zeroed, as fresh memory is, apart from the area's "Save" tag and its
// ready flags.
func (m *Machine) initForegroundBucket() {
	m.Mem.Write32(fgSaveArea, 0x53617665) // "Save"
	m.Mem.Write32(fgSaveArea+8, m.Mem.Read32(fgSaveArea+8)|0x300)
}

func init() {
	const lib = "coreinit"
	// both calls write an address and a size where their arguments point
	// (when they point anywhere) and return true
	give := func(area func(m *Machine) (uint32, uint32)) hleFunc {
		return func(m *Machine, cpu *CPU, t *Thread) {
			a, n := area(m)
			for i, v := range [2]uint32{a, n} {
				if p := arg(cpu, i); p != 0 {
					m.Mem.Write32(p, v)
				}
			}
			ret(cpu, 1)
		}
	}
	reg(lib, "OSGetForegroundBucket", give(func(*Machine) (uint32, uint32) { return FGBase, FGSize }))
	reg(lib, "OSGetForegroundBucketFreeArea", give((*Machine).fgFreeArea))

	// OSCopyFromClipboard(buffer, size in/out): nothing is ever on the
	// clipboard. Asking for the size alone (no buffer, size 0) answers
	// false; otherwise the size comes back 0.
	reg(lib, "OSCopyFromClipboard", func(m *Machine, cpu *CPU, t *Thread) {
		buf, size := arg(cpu, 0), arg(cpu, 1)
		if size == 0 || buf == 0 && m.Mem.Read32(size) == 0 {
			ret(cpu, 0)
			return
		}
		m.Mem.Write32(size, 0)
		ret(cpu, 1)
	})
}
