package ctr

import "testing"

// The DSP's shared structures fit in a region (0x4000 words) without
// overlapping.
func TestDSPLayout(t *testing.T) {
	if end := int(dspStructs[14]) + 0x13a3; end > 0xc000 {
		t.Errorf("the structures end at %#x, past the region", end)
	}
	for i := 1; i < len(dspStructs); i++ {
		if dspStructs[i] <= dspStructs[i-1] {
			t.Errorf("structure %d at %#x", i, dspStructs[i])
		}
	}
}
