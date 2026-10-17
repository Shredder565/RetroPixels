package wiiu

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestHello(t *testing.T) {
	for _, z := range []bool{false, true} {
		m, log := runRPX(t, buildRPX(t, z, "hello.c"), 10)
		want := "hello from test.rpx: -42 42 beef wii u 3.250 1234567890123"
		if !strings.Contains(log, want) || !strings.Contains(log, "snprintf: 00007|ab  |Z (12)") {
			t.Fatalf("log:\n%s", log)
		}
		if !m.Exited || m.ExitCode != 3 {
			t.Fatalf("exited %v code %d", m.Exited, m.ExitCode)
		}
	}
}

// TestCPU runs the same C program on the emulated Espresso and natively; the
// results must match.
func TestCPU(t *testing.T) {
	rpx := buildRPX(t, false, "cputest.c")
	native := filepath.Join(t.TempDir(), "cputest")
	out, err := exec.Command("clang", "-O2", "-DNATIVE", "-fwrapv", "-ffp-contract=off", "-o", native,
		"testdata/progs/cputest.c", "-lm").CombinedOutput()
	if err != nil {
		t.Skipf("can't build the native version: %v\n%s", err, out)
	}
	want, err := exec.Command(native).Output()
	if err != nil {
		t.Fatal(err)
	}
	_, log := runRPX(t, rpx, 600)
	log = strings.Replace(log, "the program ended (exit code 0)\n", "", 1)
	if strings.TrimSpace(log) != strings.TrimSpace(string(want)) {
		t.Fatalf("emulated:\n%s\nnative:\n%s", log, want)
	}
}

func TestThreads(t *testing.T) {
	_, log := runRPX(t, buildRPX(t, false, "threads.c"), 600)
	for _, want := range []string{"counter 3000 results 10 21 32", "queue sum 210", "slept 100 ms, alarms ok", "heap 1 1 1", "reuse 1"} {
		if !strings.Contains(log, want) {
			t.Fatalf("missing %q in log:\n%s", want, log)
		}
	}
}

func TestOSScreen(t *testing.T) {
	m, err := New(buildRPX(t, false, "screen.c"), "screen.rpx", nil)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 30; i++ {
		m.RunFrame()
	}
	if m.Err != nil {
		t.Fatal(m.Err)
	}
	tv := m.screen.TV
	if tv[0] != 0x102040 {
		t.Fatalf("background %06x", tv[0])
	}
	if tv[300*TVWidth+350] != 0xff0000 {
		t.Fatalf("line %06x", tv[300*TVWidth+350])
	}
	text := 0
	for y := 0; y < 24*3; y++ {
		for x := 0; x < 400; x++ {
			if tv[y*TVWidth+x] == 0xffffff {
				text++
			}
		}
	}
	if text < 500 {
		t.Fatalf("only %d text pixels", text)
	}
	// press A twice: the program counts them and ends
	for i := 0; i < 4 && !m.Exited; i++ {
		b := uint32(0)
		if i%2 == 0 {
			b = BtnA
		}
		m.SetGamePad(PadState{Buttons: b})
		for j := 0; j < 3; j++ {
			m.RunFrame()
		}
	}
	if !m.Exited {
		t.Fatalf("still running")
	}
	if os.Getenv("WIIU_DUMP") != "" {
		savePNG(t, os.Getenv("WIIU_DUMP")+"/screen.png", m.screen.TV, TVWidth, TVHeight)
	}
}
