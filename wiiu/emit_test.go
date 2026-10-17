package wiiu

import (
	"os"
	"path/filepath"
	"testing"
)

// TestEmitRPX writes the test programs as RPX files into $WIIU_EMIT (for
// tests on systems without the compiler, like the Direct3D ones under
// Wine).
func TestEmitRPX(t *testing.T) {
	dir := os.Getenv("WIIU_EMIT")
	if dir == "" {
		t.Skip("WIIU_EMIT not set")
	}
	for _, p := range []string{"gx2test.c", "demo.c"} {
		rpx := buildRPX(t, false, p)
		name := filepath.Join(dir, p[:len(p)-2]+".rpx")
		if err := os.WriteFile(name, rpx, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}
