package ps3

import (
	"image"
	"image/png"
	"os"
	"strconv"
	"testing"
)

// PS3_GAME=<installed folder> go test -run BootLocal -v
func TestBootLocal(t *testing.T) {
	dir := os.Getenv("PS3_GAME")
	if dir == "" {
		t.Skip("PS3_GAME not set")
	}
	lf, _ := os.Create(os.Getenv("PS3_LOG"))
	defer lf.Close()
	m, err := New(Options{GameDir: dir, SaveDir: t.TempDir(), Log: lf})
	if err != nil {
		t.Fatal(err)
	}
	frames := 600
	if n, err := strconv.Atoi(os.Getenv("PS3_FRAMES")); err == nil {
		frames = n
	}
	for i := 0; i < frames; i++ {
		if err := m.RunFrame(); err != nil {
			t.Log(err)
			break
		}
		if m.Exited {
			break
		}
	}
	t.Logf("frames %d, instructions %d", m.Frame(), m.cpu.Ran)
	t.Log("\n" + m.Threads())
	if sc := m.Picture(); sc != nil && os.Getenv("PS3_PNG") != "" {
		img := image.NewRGBA(image.Rect(0, 0, sc.W, sc.H))
		for i, p := range sc.Pix {
			img.Pix[4*i], img.Pix[4*i+1], img.Pix[4*i+2], img.Pix[4*i+3] = byte(p>>16), byte(p>>8), byte(p), 255
		}
		f, _ := os.Create(os.Getenv("PS3_PNG"))
		png.Encode(f, img)
		f.Close()
	}
	for k, v := range m.Missing {
		t.Logf("missing %s x%d", k, v)
	}
}
