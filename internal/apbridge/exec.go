package apbridge

// Carrying out a batch on an emulator's memory (RetroPixels Native's cores,
// RetroPixels.exe's Nintendo 64), the way web/emu/archipelago.js does it in
// the page.

import "fmt"

// Memory is an emulator's memory, as named memory domains (BizHawk's names).
// core.Memory is the same interface.
type Memory interface {
	MemoryDomains() []string
	MemorySize(domain string) int
	// ReadMemory and WriteMemory work on addr..addr+len-1, all inside the domain.
	ReadMemory(domain string, addr uint32, buf []byte) error
	WriteMemory(domain string, addr uint32, data []byte) error
}

func hasDomain(m Memory, d string) bool {
	for _, x := range m.MemoryDomains() {
		if x == d {
			return true
		}
	}
	return false
}

// Exec carries out a batch of operations on m, stopping after a failed guard.
// "msg", "lock" and "unlock" go to other (which may be nil). It must run
// between two frames, on the emulator's own thread.
func Exec(m Memory, ops []Op, other func(Op)) []Res {
	out := make([]Res, 0, len(ops))
	for _, o := range ops {
		r := execOne(m, o, other)
		out = append(out, r)
		if o.T == "guard" && !r.OK {
			break
		}
	}
	return out
}

func execOne(m Memory, o Op, other func(Op)) (r Res) {
	defer func() {
		if x := recover(); x != nil {
			r = Res{Err: fmt.Sprintf("the emulator couldn't do that (%v)", x)}
		}
	}()
	switch o.T {
	case "msg", "lock", "unlock":
		if other != nil {
			other(o)
		}
		return
	case "size", "read", "write", "guard":
	default:
		return Res{Err: "unknown operation " + o.T}
	}
	if !hasDomain(m, o.D) {
		return Res{Err: fmt.Sprintf("no memory domain named %q", o.D)}
	}
	size := int64(m.MemorySize(o.D))
	if o.T == "size" {
		return Res{N: size}
	}
	n := o.N
	if o.T != "read" {
		n = int64(len(o.V))
	}
	if o.A < 0 || n < 0 || (!o.Pad && o.A+n > size) {
		return Res{Err: "address out of range for " + o.D}
	}
	// the part inside the domain (with Pad, the rest reads as 0 and isn't written)
	in := int64(0)
	if o.A < size {
		in = min(n, size-o.A)
	}
	switch o.T {
	case "read":
		r.V = make([]byte, n)
		if in > 0 {
			if err := m.ReadMemory(o.D, uint32(o.A), r.V[:in]); err != nil {
				return Res{Err: err.Error()}
			}
		}
	case "write":
		if in > 0 {
			if err := m.WriteMemory(o.D, uint32(o.A), o.V[:in]); err != nil {
				return Res{Err: err.Error()}
			}
		}
	case "guard":
		have := make([]byte, n)
		if in > 0 {
			if err := m.ReadMemory(o.D, uint32(o.A), have[:in]); err != nil {
				return Res{Err: err.Error()}
			}
		}
		r.OK = true
		for i, b := range o.V {
			if have[i] != b {
				r.OK = false
				break
			}
		}
	}
	return r
}
