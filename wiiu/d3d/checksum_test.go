package d3d

import (
	"encoding/binary"
	"os"
	"testing"
)

// The blobs in testdata were compiled by Wine's shader compiler, which signs
// them the same way Windows does.
func TestChecksum(t *testing.T) {
	for _, n := range []string{"basic", "tex", "disc"} {
		b, err := os.ReadFile("testdata/" + n + ".dxbc")
		if err != nil {
			t.Fatal(err)
		}
		got := Checksum(b)
		for i := range got {
			if want := binary.LittleEndian.Uint32(b[4+4*i:]); got[i] != want {
				t.Fatalf("%s: word %d is %08x, want %08x", n, i, got[i], want)
			}
		}
	}
}
