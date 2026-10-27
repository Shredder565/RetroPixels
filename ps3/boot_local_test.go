package ps3

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/png"
	"os"
	"runtime/pprof"
	"sort"
	"strconv"
	"strings"
	"testing"
	"time"
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
	lastMachine = m
	frames := 600
	if n, err := strconv.Atoi(os.Getenv("PS3_FRAMES")); err == nil {
		frames = n
	}
	input := parseInput(os.Getenv("PS3_INPUT"))
	// PS3_WAV=<file>: the sound, as a 48 kHz stereo WAV file
	var wav []byte
	if p := os.Getenv("PS3_WAV"); p != "" {
		wav = make([]byte, 44, 1<<24)
		defer func() { writeWAV(p, wav) }()
	}
	for i := 0; i < frames; i++ {
		var b uint32
		for _, e := range input {
			if i >= e.from && i < e.to {
				b |= e.btn
			}
		}
		m.SetPad(b, 0, 0, 0, 0)
		if from, err := strconv.Atoi(os.Getenv("PS3_FAKEGPU")); err == nil && i == from {
			// PS3_FAKEGPU=<frame>: draw on the stand-in GPU from that frame
			if err := m.UseHostGPU(newFakeGPU()); err != nil {
				t.Fatal(err)
			}
		}
		if from, err := strconv.Atoi(os.Getenv("PS3_PROFFROM")); err == nil && i == from {
			// PS3_PROFFROM=<frame>: CPU profile from that frame to the end
			if j := m.cpu.jit; j != nil {
				clear(j.profCount)
			}
			pf, _ := os.Create(os.Getenv("PS3_PROFOUT"))
			pprof.StartCPUProfile(pf)
			defer pprof.StopCPUProfile()
			profStart, profRan, profFrame := time.Now(), m.cpu.Ran, m.Frame()
			defer func() {
				d := time.Since(profStart)
				n := m.Frame() - profFrame
				t.Logf("profiled %d frames in %v: %.1f fps, %.1f M PPU instructions a second",
					n, d, float64(n)/d.Seconds(), float64(m.cpu.Ran-profRan)/d.Seconds()/1e6)
			}()
		}
		if err := m.RunFrame(); err != nil {
			t.Log(err)
			if os.Getenv("PS3_ERRDUMP") != "" {
				for _, th := range m.threads {
					if th.PC>>8 == 0x1a95 || th.Name == "PPU PhysX thread" {
						t.Logf("%s: GPR %x", th.Name, th.GPR)
						if th.PC>>8 == 0x1a95 {
							x := m.Mem.Read32(m.Mem.Read32(uint32(th.GPR[22])+1048) + 236)
							tab := m.Mem.Read32(uint32(th.GPR[2]) - 21760)
							y := m.Mem.Read32(tab + (x<<2)&0x3ffffc)
							vt := m.Mem.Read32(y)
							t.Logf("handle %x obj %x vtable %x fn opd %x code %x", x, y, vt, m.Mem.Read32(vt+144), m.Mem.Read32(m.Mem.Read32(vt+144)))
						}
						a := uint32(th.GPR[16])
						for k := uint32(0); k < 64; k += 8 {
							t.Logf("%08x: %08x %08x", a+k, m.Mem.Read32(a+k), m.Mem.Read32(a+k+4))
						}
					}
				}
			}
			break
		}
		if m.Exited {
			break
		}
		if wav != nil {
			for _, v := range m.TakeAudio() {
				x := int16(max(-1, min(1, v)) * 32767)
				wav = append(wav, byte(x), byte(x>>8))
			}
		}
		if every, _ := strconv.Atoi(os.Getenv("PS3_PNGEVERY")); every > 0 && (i+1)%every == 0 {
			dir := os.Getenv("PS3_SHOTDIR")
			if dir == "" {
				dir = "/tmp/claude-0/ps3"
			}
			savePNG(m, fmt.Sprintf("%s/shot_%04d.png", dir, i+1))
		}
	}
	t.Logf("frames %d, instructions %d", m.Frame(), m.cpu.Ran)
	if m.gpu != nil {
		t.Logf("GPU: %d draws, %d fell back to software", m.gpu.Draws, m.gpu.Fallbacks)
	}
	if jitInterpHist != nil {
		type kv struct {
			k string
			v int
		}
		var l []kv
		for k, v := range jitInterpHist {
			l = append(l, kv{k, v})
		}
		sort.Slice(l, func(i, j int) bool { return l[i].v > l[j].v })
		for i := 0; i < len(l) && i < 40; i++ {
			t.Logf("interpreted %s: %d", l[i].k, l[i].v)
		}
	}
	if j := m.cpu.jit; j != nil {
		t.Logf("PPU recompiler: %d blocks, %d flushes, %d interpreted", j.Blocks, j.Flushes, j.Interp)
	}
	if m.spus != nil {
		for _, s := range m.spus.threads {
			t.Logf("SPU %s: %d instructions", s.name, s.c.Ran)
			if j := s.c.jit; j != nil {
				t.Logf("SPU recompiler: %d blocks, %d flushes, %d invalidated, %d interpreted", j.Blocks, j.Flushes, j.Invalidated, j.Interp)
			}
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
	for _, a := range strings.Split(os.Getenv("PS3_STUBS"), ",") {
		if v, err := strconv.ParseUint(a, 16, 32); err == nil {
			t.Logf("stub %x: %s", v, stubName(m, uint32(v)))
		}
	}
	for reg, n := range rsxStat {
		t.Logf("RSX method %04X: %d", reg, n)
	}
	for pc, n := range spuProf {
		if n > 0 {
			t.Logf("SPU pc %05x: %d", pc, n)
		}
	}
	if spuHist != nil {
		var sb strings.Builder
		for pc, n := range spuHist {
			if n > 0 {
				fmt.Fprintf(&sb, "%05x %d\n", pc, n)
			}
		}
		os.WriteFile(os.Getenv("PS3_SPUHIST"), []byte(sb.String()), 0o644)
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

type inputEvent struct {
	from, to int
	btn      uint32
}

// parseInput reads "from-to:button,..." (frames), e.g. "1200-1210:start".
func parseInput(s string) []inputEvent {
	names := map[string]uint32{"select": PadSelect, "start": PadStart, "up": PadUp, "down": PadDown, "left": PadLeft, "right": PadRight,
		"cross": PadCross, "circle": PadCircle, "square": PadSquare, "triangle": PadTriangle, "l1": PadL1, "r1": PadR1, "l2": PadL2, "r2": PadR2}
	var out []inputEvent
	for _, part := range strings.Split(s, ",") {
		var a, b int
		var n string
		if _, err := fmt.Sscanf(strings.Replace(part, ":", " ", 1), "%d-%d %s", &a, &b, &n); err == nil {
			out = append(out, inputEvent{a, b, names[n]})
		}
	}
	return out
}

// stubName follows an import stub (lis r12,hi; lwz r12,lo(r12) ...) to the
// system function it calls.
func stubName(m *Machine, a uint32) string {
	var hi uint32
	for i := uint32(0); i < 8; i++ {
		w := m.Mem.Read32(a + 4*i)
		switch w >> 16 {
		case 0x3d80, 0x658c: // lis / oris r12
			hi = w << 16
		case 0x818c:
			ptr := m.Mem.Read32(hi + uint32(int32(int16(w))))
			code := m.Mem.Read32(ptr)
			idx := m.Mem.Read32(code) & 0x3ffffff
			if int(idx) < len(m.hle) {
				return m.hle[idx].name
			}
			return fmt.Sprintf("? opd %x", ptr)
		}
	}
	return "?"
}

func writeWAV(path string, b []byte) {
	n := uint32(len(b) - 44)
	le := binary.LittleEndian
	copy(b, "RIFF")
	le.PutUint32(b[4:], 36+n)
	copy(b[8:], "WAVEfmt ")
	le.PutUint32(b[16:], 16)
	le.PutUint16(b[20:], 1)
	le.PutUint16(b[22:], 2)
	le.PutUint32(b[24:], 48000)
	le.PutUint32(b[28:], 48000*4)
	le.PutUint16(b[32:], 4)
	le.PutUint16(b[34:], 16)
	copy(b[36:], "data")
	le.PutUint32(b[40:], n)
	os.WriteFile(path, b, 0o644)
}
