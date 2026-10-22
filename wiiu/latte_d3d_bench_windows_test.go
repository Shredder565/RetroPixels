package wiiu

import (
	"os"
	"testing"
	"time"
)

// TestD3DDrawCost times many small draws (D3DBENCH=1).
func TestD3DDrawCost(t *testing.T) {
	if os.Getenv("D3DBENCH") == "" {
		t.Skip("set D3DBENCH=1")
	}
	for _, hw := range []bool{false, true} {
		g := newTestGPU()
		if hw {
			b, err := newD3DBackend(g, false)
			if err != nil {
				t.Skip(err)
			}
			defer b.close()
			g.hw = b
		}
		b := &pm4{}
		setupTarget(b, tile2DThin1)
		passShaders(g, b)
		putFloatsBE(g, tVB,
			-0.1, 0.1, 0, 1, 1, 0, 0, 1,
			0.1, 0.1, 0, 1, 0, 1, 0, 1,
			-0.1, -0.1, 0, 1, 0, 0, 1, 1)
		b.set(regVGT_PRIMITIVE_TYPE, 4)
		b.packet(itNUM_INSTANCES, 1)
		for i := 0; i < 2000; i++ {
			b.packet(itDRAW_INDEX_AUTO, 3, 2)
		}
		start := time.Now()
		g.exec(b.w)
		if g.hw != nil {
			g.hw.flushAll()
		}
		t.Logf("host GPU %v: %v a draw", hw, time.Since(start)/2000)
	}
}
