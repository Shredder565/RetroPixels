package wiiu

import (
	"testing"
	"time"
)

// BenchmarkCPU: frames of a busy program (one core flat out).
func BenchmarkCPU(b *testing.B) {
	m, err := New(buildRPX(b, false, "spin.c"), "spin.rpx", nil)
	if err != nil {
		b.Fatal(err)
	}
	b.ResetTimer()
	start := time.Now()
	for i := 0; i < b.N; i++ {
		m.RunFrame()
	}
	b.ReportMetric(float64(b.N*m.Budget)/time.Since(start).Seconds()/1e6, "Minst/s")
}
