package ps3

import (
	"fmt"
	"os"
	"sort"
	"testing"
)

// TestBlockProfDump (run after TestBootLocal with PS3_BLOCKPROF=<file>)
// writes the hottest blocks and the mix of instructions run.
func TestBlockProfDump(t *testing.T) {
	out := os.Getenv("PS3_BLOCKPROF")
	if out == "" || lastMachine == nil || lastMachine.cpu.jit == nil {
		return
	}
	m := lastMachine
	j := m.cpu.jit
	type blk struct {
		pc    uint32
		n     uint32
		count uint64
	}
	byPC := map[uint32]*blk{}
	mix := map[string]uint64{}
	var total uint64
	for k, c := range j.profCount {
		if c == 0 {
			continue
		}
		pc, n := j.profPC[k], j.profN[k]
		b := byPC[pc]
		if b == nil {
			b = &blk{pc: pc, n: n}
			byPC[pc] = b
		}
		b.count += c
		for i := uint32(0); i < n; i++ {
			w := m.Mem.Read32(pc + 4*i)
			op := w >> 26
			key := fmt.Sprintf("%d", op)
			switch op {
			case 31, 63, 59:
				key = fmt.Sprintf("%d/%d", op, w>>1&0x3ff)
			case 19:
				key = fmt.Sprintf("19/%d", w>>1&0x3ff)
			case 4:
				key = fmt.Sprintf("4/%d", w&0x7ff)
			case 30:
				key = fmt.Sprintf("30/%d", w>>2&7)
			}
			mix[key] += c
			total += c
		}
	}
	var bl []*blk
	for _, b := range byPC {
		bl = append(bl, b)
	}
	sort.Slice(bl, func(a, b int) bool { return bl[a].count*uint64(bl[a].n) > bl[b].count*uint64(bl[b].n) })
	f, _ := os.Create(out)
	defer f.Close()
	fmt.Fprintf(f, "total %d instructions in compiled blocks\n", total)
	type kv struct {
		k string
		v uint64
	}
	var ml []kv
	for k, v := range mix {
		ml = append(ml, kv{k, v})
	}
	sort.Slice(ml, func(a, b int) bool { return ml[a].v > ml[b].v })
	for i, e := range ml {
		if i >= 50 {
			break
		}
		fmt.Fprintf(f, "op %-8s %5.2f%%\n", e.k, 100*float64(e.v)/float64(total))
	}
	for i, b := range bl {
		if i >= 60 {
			break
		}
		fmt.Fprintf(f, "block %08x n %3d runs %10d  %5.2f%%\n", b.pc, b.n, b.count, 100*float64(b.count*uint64(b.n))/float64(total))
	}
}

// lastMachine is the machine TestBootLocal ran (for the dumps after it).
var lastMachine *Machine
