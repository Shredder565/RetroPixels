package main

// Archipelago requests for a Nintendo 64 game, whose memory is in this program.

import (
	"errors"
	"time"
)

type n64APReq struct {
	ops   []apOp
	reply chan []apRes
}

func runN64Ops(session string, p *apPage, ops []apOp) ([]apRes, error) {
	n64Mu.Lock()
	s := n64Sessions[session]
	n64Mu.Unlock()
	if s == nil {
		return nil, errNoGame
	}
	req := n64APReq{ops: ops, reply: make(chan []apRes, 1)}
	select {
	case s.cmd <- req:
	case <-s.done:
		return nil, errNoGame
	case <-time.After(apCallTimeout):
		return nil, errors.New("the Nintendo 64 emulator is busy")
	}
	var res []apRes
	select {
	case res = <-req.reply:
	case <-s.done:
		return nil, errNoGame
	case <-time.After(apCallTimeout):
		return nil, errors.New("the Nintendo 64 emulator didn't answer")
	}
	// On-screen messages are shown by the page.
	for i, o := range ops {
		if i < len(res) && o.T == "msg" && p != nil {
			_ = p.send(map[string]any{"ev": "msg", "text": o.Text})
		}
	}
	return res, nil
}

// n64Domain finds a memory domain: its bytes, and for "System Bus" the offset
// of the address within them.
func (s *n64Session) n64Domain(name string, addr int64) (mem []byte, off int64, ok bool) {
	switch name {
	case "RDRAM":
		return s.m.RDRAM(), addr, true
	case "ROM":
		return s.m.ROM(), addr, true
	case "System Bus":
		// KSEG0 / KSEG1 views of RDRAM, and the cartridge at 0x10000000
		a := addr & 0x1fffffff
		if a < int64(len(s.m.RDRAM())) {
			return s.m.RDRAM(), a, true
		}
		if a >= 0x10000000 && a < 0x1fc00000 {
			return s.m.ROM(), a - 0x10000000, true
		}
		return nil, 0, true
	}
	return nil, 0, false
}

// apOps runs on the emulator goroutine, between frames.
func (s *n64Session) apOps(ops []apOp) []apRes {
	out := make([]apRes, 0, len(ops))
	if s.m == nil {
		return append(out, apRes{Err: "the Nintendo 64 emulator has stopped"})
	}
	for _, o := range ops {
		var r apRes
		switch o.T {
		case "msg", "lock", "unlock":
		case "size":
			mem, _, ok := s.n64Domain(o.D, 0)
			if !ok {
				r.Err = "no memory domain named " + o.D
			} else if o.D == "System Bus" {
				r.N = 1 << 32
			} else {
				r.N = int64(len(mem))
			}
		case "read", "write", "guard":
			mem, off, ok := s.n64Domain(o.D, o.A)
			n := o.N
			if o.T != "read" {
				n = int64(len(o.V))
			}
			if !ok {
				r.Err = "no memory domain named " + o.D
				break
			}
			if o.A < 0 || n < 0 || (!o.Pad && o.D != "System Bus" && off+n > int64(len(mem))) {
				r.Err = "address out of range for " + o.D
				break
			}
			switch o.T {
			case "read":
				r.V = make([]byte, n)
				if off < int64(len(mem)) {
					copy(r.V, mem[off:])
				}
			case "write":
				if off < int64(len(mem)) {
					copy(mem[off:], o.V)
				}
			case "guard":
				r.OK = true
				for i, b := range o.V {
					p := off + int64(i)
					var v byte
					if p < int64(len(mem)) {
						v = mem[p]
					}
					if v != b {
						r.OK = false
						break
					}
				}
			}
		default:
			r.Err = "unknown operation " + o.T
		}
		out = append(out, r)
		if o.T == "guard" && !r.OK {
			break
		}
	}
	return out
}
