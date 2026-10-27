package ps3

import (
	"fmt"
	"image"
	"image/png"
	"os"
	"strconv"
	"strings"
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
	if os.Getenv("PS3_RSXSTAT") != "" {
		rsxStat = map[uint32]int{}
	}
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
		if every, _ := strconv.Atoi(os.Getenv("PS3_PNGEVERY")); every > 0 && (i+1)%every == 0 {
			savePNG(m, fmt.Sprintf("/tmp/claude-0/ps3/shot_%04d.png", i+1))
		}
	}
	t.Logf("frames %d, instructions %d", m.Frame(), m.cpu.Ran)
	if m.spus != nil {
		for _, s := range m.spus.threads {
			t.Logf("SPU %s: %d instructions", s.name, s.c.Ran)
		}
	}
	t.Log("\n" + m.Threads())
	if os.Getenv("PS3_PNG") != "" {
		savePNG(m, os.Getenv("PS3_PNG"))
	}
	if r := os.Getenv("PS3_SPUDIS"); r != "" && m.spus != nil && len(m.spus.threads) > 0 {
		var a, b uint32
		fmt.Sscanf(r, "%x-%x", &a, &b)
		c := m.spus.threads[0].c
		var sb strings.Builder
		for pc := a; pc < b; pc += 4 {
			fmt.Fprintf(&sb, "%05x: %08x  %s\n", pc, c.ls32(pc), SPUDisasm(pc, c.ls32(pc)))
		}
		os.WriteFile("/tmp/claude-0/ps3/spudis.txt", []byte(sb.String()), 0o644)
	}
	for reg, n := range rsxStat {
		t.Logf("RSX method %04X: %d", reg, n)
	}
	for pc, n := range spuProf {
		if n > 20 {
			t.Logf("SPU pc %05x: %d", pc, n)
		}
	}
	for k, v := range m.Missing {
		t.Logf("missing %s x%d", k, v)
	}
}

func TestProfileLocal(t *testing.T) {
	if os.Getenv("PS3_PROF") == "" {
		t.Skip()
	}
	TestBootLocal(t)
}

func savePNG(m *Machine, path string) {
	sc := m.Picture()
	if sc == nil {
		return
	}
	img := image.NewRGBA(image.Rect(0, 0, sc.W, sc.H))
	for i, p := range sc.Pix {
		img.Pix[4*i], img.Pix[4*i+1], img.Pix[4*i+2], img.Pix[4*i+3] = byte(p>>16), byte(p>>8), byte(p), 255
	}
	f, _ := os.Create(path)
	png.Encode(f, img)
	f.Close()
}
