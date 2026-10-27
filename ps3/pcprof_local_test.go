package ps3

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"testing"
)

type pcKey struct {
	name string
	pc   uint32
}

var pcProf = map[pcKey]uint64{}
var pcThreads = map[string]uint64{}

func init() {
	if os.Getenv("PS3_PCPROF") == "" {
		return
	}
	sliceHook = func(m *Machine, t *Thread, ran uint64) {
		if m.frame < pcProfFrom {
			return
		}
		pcProf[pcKey{t.Name, uint32(t.PC)}] += ran
		pcThreads[t.Name] += ran
	}
}

func TestPCProfDump(t *testing.T) {
	if os.Getenv("PS3_PCPROF") == "" {
		return
	}
	type kv struct {
		k pcKey
		v uint64
	}
	var all []kv
	for k, v := range pcProf {
		all = append(all, kv{k, v})
	}
	sort.Slice(all, func(i, j int) bool { return all[i].v > all[j].v })
	f, _ := os.Create(os.Getenv("PS3_PCPROF"))
	defer f.Close()
	for n, v := range pcThreads {
		fmt.Fprintf(f, "thread %-30s %d\n", n, v)
	}
	for i, e := range all {
		if i > 60 {
			break
		}
		fmt.Fprintf(f, "%-30s %08x %d\n", e.k.name, e.k.pc, e.v)
	}
}

// PS3_PCPROF=<file>: where the PPU threads were at the end of each turn,
// weighted by instructions, from frame PS3_PCPROFFROM on (run with
// -run 'TestBootLocal|TestPCProfDump').
var pcProfFrom, _ = strconv.ParseUint(os.Getenv("PS3_PCPROFFROM"), 10, 64)
