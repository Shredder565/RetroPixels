package ps3

import (
	"os"
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
	for k, v := range m.Missing {
		t.Logf("missing %s x%d", k, v)
	}
}
