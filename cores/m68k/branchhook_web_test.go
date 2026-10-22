package m68k

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The web 68000 (RetroPixels.exe's Genesis core) has the same branch hooks
// as this one: with branchHooks on, an A-line opcode holding a backward short
// branch loops back; without, it traps. Skipped without node.
func TestBranchHooksJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	emu, _ := filepath.Abs("../../web/emu/m68k.js")
	out, err := exec.Command(node, "testdata/branchhook.js", emu).CombinedOutput()
	if err != nil {
		t.Fatalf("node: %v\n%s", err, out)
	}
	if got := strings.TrimSpace(string(out)); got != "hooks: 102 no hooks: 202" {
		t.Fatalf("web 68000: %q", got)
	}
}
