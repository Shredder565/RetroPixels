//go:build windows

package wiiu

import (
	"os"
	"path/filepath"
	"testing"
)

// TestD3DFrames runs test programs with the software renderer and with
// the Direct3D backend and compares the pictures (the programs come from
// TestEmitRPX: set WIIU_RPX to its directory).
func TestD3DFrames(t *testing.T) {
	dir := os.Getenv("WIIU_RPX")
	if dir == "" {
		t.Skip("WIIU_RPX not set")
	}
	dxDevice(t)
	for _, name := range []string{"gx2test", "demo"} {
		t.Run(name, func(t *testing.T) {
			rpx, err := os.ReadFile(filepath.Join(dir, name+".rpx"))
			if err != nil {
				t.Skip(err)
			}
			frames := 30
			soft, _ := runRPX(t, rpx, frames)
			m, err := New(rpx, "test.rpx", nil)
			if err != nil {
				t.Fatal(err)
			}
			hw, err := newD3DBackend(m.gpu, false)
			if err != nil {
				t.Fatal(err)
			}
			defer hw.close()
			m.gpu.hw = hw
			dxDumpShaders = os.Getenv("WIIU_D3D_DUMP") != ""
			for i := 0; i < frames && !m.Exited && m.Err == nil; i++ {
				m.RunFrame()
			}
			if m.Err != nil {
				t.Fatal(m.Err)
			}
			t.Logf("%d draws; last fallback: %q", m.gpu.Draws, hw.err)
			a, b := soft.screen.TV, m.screen.TV
			bad, worst := 0, 0
			for i := range a {
				d := 0
				for s := 0; s < 32; s += 8 {
					x, y := int(a[i]>>s&255), int(b[i]>>s&255)
					d = max(d, x-y, y-x)
				}
				if d > 8 {
					bad++
				}
				worst = max(worst, d)
			}
			t.Logf("%d of %d pixels differ by more than 8 (worst %d)", bad, len(a), worst)
			if bad > len(a)/1000 {
				t.Errorf("pictures differ")
				if out := os.Getenv("WIIU_DUMP"); out != "" {
					writePPM(filepath.Join(out, name+"_soft.ppm"), a, TVWidth, TVHeight)
					writePPM(filepath.Join(out, name+"_d3d.ppm"), b, TVWidth, TVHeight)
				}
			}
		})
	}
}

// TestUseHostGPU: a machine picks up the Direct3D backend, and the
// software switch keeps it off.
func TestUseHostGPU(t *testing.T) {
	m := newMachine(nil)
	if !m.UseHostGPU() {
		t.Skip("no Direct3D device here")
	}
	m.Close()
	if m.gpu.hw != nil {
		t.Fatal("Close kept the backend")
	}
	t.Setenv("RETROPIXELS_WIIU_GPU", "software")
	m2 := newMachine(nil)
	if m2.UseHostGPU() {
		t.Fatal("RETROPIXELS_WIIU_GPU=software still used the host GPU")
	}
}
