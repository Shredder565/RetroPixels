package wiiu

import (
	"math/rand"
	"testing"
)

// TestHostFormatRoundTrip: guest colours survive a trip through the host
// format they're drawn in.
func TestHostFormatRoundTrip(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	kinds := []numKind{kUnorm, kSnorm, kUint, kSint, kSrgb, kFloat}
	for f := uint32(1); f < 64; f++ {
		d := &fmtDescs[f]
		n := fmtBytes(f)
		if d.nc == 0 || n == 0 || n > 16 || formatCompressed(f) {
			continue
		}
		for _, k := range kinds {
			if (d.float || d.small) != (k == kFloat) {
				continue
			}
			h := hostColorFormat(f, k)
			hb := make([]byte, hostPixelBytes(h))
			for i := 0; i < 200; i++ {
				src := make([]byte, n)
				rng.Read(src)
				v := decodeElem(f, k, src)
				// what the guest itself would store for these values
				want := make([]byte, n)
				encodeElem(f, k, v, want)
				hostEncode(h, v, hb)
				back := hostDecode(h, hb)
				got := make([]byte, n)
				encodeElem(f, k, back, got)
				if string(got) != string(want) {
					t.Errorf("format %#x kind %d host %d: %x -> %08x -> %x, want %x", f, k, h, src, v, got, want)
					break
				}
			}
		}
	}
}

func TestHostIndices(t *testing.T) {
	r := ^uint32(0)
	cases := []struct {
		prim uint32
		ids  []uint32
		want []uint32
	}{
		{4, []uint32{0, 1, 2, 3, 4, 5}, []uint32{0, 1, 2, 3, 4, 5}},
		{5, []uint32{0, 1, 2, 3}, []uint32{0, 1, 2, 0, 2, 3}},
		{6, []uint32{0, 1, 2, 3, r, 4, 5, 6}, []uint32{0, 1, 2, 2, 1, 3, 4, 5, 6}},
		{0x13, []uint32{0, 1, 2, 3}, []uint32{0, 1, 2, 0, 2, 3}},
		{0x12, []uint32{0, 1, 2}, []uint32{0, 1, 1, 2, 2, 0}},
	}
	for _, c := range cases {
		_, got, ok := hostIndices(c.prim, c.ids, false)
		if !ok || fmtU32(got) != fmtU32(c.want) {
			t.Errorf("prim %#x: got %v, want %v", c.prim, got, c.want)
		}
	}
	if _, got, _ := hostIndices(0x11, []uint32{0, 1, 2, 3, ^uint32(0), 5}, true); fmtU32(got) != fmtU32([]uint32{0, 1, 2}) {
		t.Errorf("rectangles: got %v", got)
	}
}

func fmtU32(v []uint32) string {
	s := ""
	for _, x := range v {
		s += string(rune('a'+x%26)) + ","
	}
	return s
}
