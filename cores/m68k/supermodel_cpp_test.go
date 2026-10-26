package m68k

import (
	"bufio"
	"encoding/binary"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
)

// Supermodel's 68000 (supermodel/Src/CPU/68K/M68000.cpp, RetroPixels' own
// C++ core for the Model 3 sound board) must behave exactly like this one.
// These tests build supermodel/tests/m68k/cases.cpp with the C++ compiler
// and compare every result and bus access with the Go processor's.
// Skipped without a C++ compiler.

// buildCppHarness compiles the C++ harness and returns its path.
func buildCppHarness(t *testing.T) string {
	t.Helper()
	cxx := os.Getenv("CXX")
	if cxx == "" {
		for _, c := range []string{"g++", "clang++", "c++"} {
			if p, err := exec.LookPath(c); err == nil {
				cxx = p
				break
			}
		}
	}
	if cxx == "" {
		t.Skip("no C++ compiler")
	}
	src, _ := filepath.Abs("../../supermodel/Src")
	exe := filepath.Join(t.TempDir(), "cases")
	cmd := exec.Command(cxx, "-std=c++17", "-O2", "-Wall", "-Werror",
		"-I"+src, "-I"+filepath.Join(src, "OSD", "SDL"),
		"-o", exe, "../../supermodel/tests/m68k/cases.cpp", filepath.Join(src, "CPU", "68K", "M68000.cpp"))
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("building the C++ harness: %v\n%s", err, out)
	}
	return exe
}

// compareWithCpp runs cases in the Go processor and in the C++ one.
func compareWithCpp(t *testing.T, exe string, cases []testCase) {
	t.Helper()
	var buf []byte
	for i := range cases {
		buf = cases[i].encode(buf)
	}
	file := filepath.Join(t.TempDir(), "cases.bin")
	if err := os.WriteFile(file, buf, 0o644); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(exe, file)
	cmd.Stderr = os.Stderr
	stdout, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer cmd.Wait()
	defer stdout.Close()
	r := bufio.NewReaderSize(stdout, 1<<20)
	u32 := func() (uint32, error) {
		var b [4]byte
		_, err := io.ReadFull(r, b[:])
		return binary.LittleEndian.Uint32(b[:]), err
	}
	fails, accesses, acks := 0, 0, 0
	for i := range cases {
		cpp := make([]uint32, 24)
		var err error
		for k := range cpp {
			if cpp[k], err = u32(); err != nil {
				t.Fatalf("case %d: reading C++ output: %v", i, err)
			}
		}
		for k := 0; k < int(cpp[23])*3; k++ {
			v, err := u32()
			if err != nil {
				t.Fatalf("case %d: reading C++ output: %v", i, err)
			}
			cpp = append(cpp, v)
		}
		gor := cases[i].run()
		for k := 24; k+2 < len(gor); k += 3 {
			accesses++
			if gor[k] == 5 {
				acks++
			}
		}
		if len(gor) != len(cpp) && compareResult(gor, cpp) == "" {
			t.Errorf("case %d: go made %d accesses, C++ %d", i, (len(gor)-24)/3, (len(cpp)-24)/3)
			fails++
		} else if diff := compareResult(gor, cpp); diff != "" {
			fails++
			tc := &cases[i]
			t.Errorf("opcode %04x (case %d, pc %06x sr %04x irq %d steps %d): %s", tc.words[0], i, tc.pc, tc.sr, tc.irq, tc.steps, diff)
		}
		if fails > 20 {
			t.FailNow()
		}
	}
	t.Logf("%d cases identical (%d bus accesses, %d interrupts taken)", len(cases), accesses, acks)
}

// Every opcode, with random registers, memory, extension words, status
// register, pending interrupt and stop state (1-3 instructions each).
func TestMatchesSupermodelCpp(t *testing.T) {
	exe := buildCppHarness(t)
	perOp := 4
	if testing.Short() {
		perOp = 1
	}
	if s := os.Getenv("RP_M68K_PEROP"); s != "" {
		perOp, _ = strconv.Atoi(s)
	}
	cases := makeCases(0x5EA5, perOp)
	for i := range cases {
		cases[i].flags |= 4 // TAS writes back on the Model 3 sound board
	}
	compareWithCpp(t, exe, cases)
}

// Whole programs: a 16 MB address space starting with a vector table that
// points every exception into the program, followed by random legal
// instructions (and random extension words), run for thousands of
// instructions with interrupts requested at random levels and vectors.
// This covers what single instructions can't: exception and interrupt
// entry and return (TRAP, CHK, divide by zero, privilege violations, A-line
// and F-line, RTE/RTR/RTS), stack switching, STOP and long chains of flags.
func TestProgramsMatchSupermodelCpp(t *testing.T) {
	exe := buildCppHarness(t)
	r := &rng{s: 0x68000 ^ 0x3d}
	n := 400
	if testing.Short() {
		n = 50
	}
	var cases []testCase
	for p := 0; p < n; p++ {
		tc := testCase{seed: r.next(), flags: 4}
		const progStart, progWords = 0x400, 2048
		var words []uint32
		// Vector table: SSP, PC, then every vector into the program (even
		// addresses within it).
		for v := 0; v < 256; v++ {
			target := uint32(progStart + 2*r.n(progWords-8))
			if v == 0 {
				target = 0x600003fe // (never read as a vector) BRA.W 0x400, where the case starts
			}
			words = append(words, target>>16, target&0xffff)
		}
		for len(words) < progStart/2 {
			words = append(words, 0)
		}
		for len(words) < progStart/2+progWords {
			op := r.next() & 0xffff
			switch r.n(10) {
			case 0: // anything, illegal ones too
			case 1: // short data for the operands
				words = append(words, op&0x00ff)
				continue
			default:
				for decode(int(op)) == nil || op == 0x4e72 && r.n(4) != 0 || op == 0x4e70 {
					op = r.next() & 0xffff
				}
			}
			words = append(words, op)
		}
		tc.words = words
		tc.pc = 0 // the words are placed at the PC
		for i := range tc.d {
			tc.d[i] = r.val()
			tc.a[i] = r.next() & 0xfffffe
		}
		tc.ssp = 0xf000
		tc.a[7] = 0xf000
		tc.usp = 0xe000
		tc.sr = 0x2700 | r.next()&0x1f
		if r.n(2) == 0 {
			tc.sr = 0x2000 | uint32(r.n(7))<<8 | r.next()&0x1f
		}
		if r.n(2) == 0 {
			tc.irq = uint32(1 + r.n(7))
		}
		if r.n(2) == 0 {
			tc.ack = uint32(64 + r.n(192))
		}
		tc.steps = uint32(1001 + r.n(3000))
		cases = append(cases, tc)
	}
	compareWithCpp(t, exe, cases)
}

// The layer Supermodel's boards use (68K.cpp: contexts, interrupts, save
// states), with the C++ core under it: supermodel/tests/m68k/wrapper.cpp.
func TestSupermodelCppWrapper(t *testing.T) {
	cxx := ""
	for _, c := range []string{"g++", "clang++", "c++"} {
		if p, err := exec.LookPath(c); err == nil {
			cxx = p
			break
		}
	}
	if cxx == "" {
		t.Skip("no C++ compiler")
	}
	src, _ := filepath.Abs("../../supermodel/Src")
	dir := t.TempDir()
	exe := filepath.Join(dir, "wrapper")
	cmd := exec.Command(cxx, "-std=c++17", "-O2", "-Wall", "-Werror",
		"-I"+src, "-I"+filepath.Join(src, "OSD", "SDL"), "-I"+filepath.Join(src, "CPU", "68K"),
		"-o", exe, "../../supermodel/tests/m68k/wrapper.cpp",
		filepath.Join(src, "CPU", "68K", "68K.cpp"), filepath.Join(src, "CPU", "68K", "M68000.cpp"), filepath.Join(src, "BlockFile.cpp"))
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("building the wrapper test: %v\n%s", err, out)
	}
	run := exec.Command(exe)
	run.Dir = dir
	out, err := run.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	t.Logf("%s", out)
}
