package ps3

import (
	"debug/elf"
	"fmt"
	"os"
	"strings"
	"testing"
)

// PS3_SPUELF=file.elf disassembles an SPU ELF's executable segments to file.dis.
func TestSPUElfDisLocal(t *testing.T) {
	p := os.Getenv("PS3_SPUELF")
	if p == "" {
		t.Skip("PS3_SPUELF not set")
	}
	f, err := elf.Open(p)
	if err != nil {
		t.Fatal(err)
	}
	var sb strings.Builder
	for _, ph := range f.Progs {
		if ph.Type != elf.PT_LOAD || ph.Flags&elf.PF_X == 0 {
			continue
		}
		b := make([]byte, ph.Filesz)
		ph.ReadAt(b, 0)
		for i := 0; i+4 <= len(b); i += 4 {
			w := uint32(b[i])<<24 | uint32(b[i+1])<<16 | uint32(b[i+2])<<8 | uint32(b[i+3])
			pc := uint32(ph.Vaddr) + uint32(i)
			fmt.Fprintf(&sb, "%05x: %08x  %s\n", pc, w, SPUDisasm(pc, w))
		}
	}
	os.WriteFile(strings.TrimSuffix(p, ".elf")+".dis", []byte(sb.String()), 0o644)
}
