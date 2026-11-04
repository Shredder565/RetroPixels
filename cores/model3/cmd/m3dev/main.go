// m3dev runs a Model 3 game without a window, for testing: it can compare
// the processor's path with a trace file, and save pictures.
package main

import (
	"archive/zip"
	"encoding/binary"
	"flag"
	"fmt"
	"image"
	"image/png"
	"io"
	"math"
	"os"
	"path/filepath"
	"runtime/pprof"
	"sort"
	"strings"
	"time"

	"retropixels/cores/model3"
	"retropixels/cores/model3/ppc"
)

var curFrame int

func main() {
	romPath := flag.String("rom", "", "zip")
	frames := flag.Int("frames", 60, "frames to run")
	trace := flag.String("trace", "", "reference trace to compare with")
	out := flag.String("out", "", "folder for pictures")
	every := flag.Int("every", 60, "save a picture every n frames")
	from := flag.Int("from", 0, "save pictures from this frame on")
	disFrom := flag.Uint64("dis", 0, "disassemble from this address after running")
	disN := flag.Int("disn", 64, "instructions to disassemble")
	bp := flag.Uint64("bp", 0, "print registers when the processor reaches this address")
	bpN := flag.Int("bpn", 3, "times to print at the breakpoint")
	bpFrom := flag.Int("bpfrom", 0, "only stop at the breakpoint from this frame on")
	dumpRAM := flag.String("dumpram", "", "write RAM to this file at the end")
	wavPath := flag.String("wav", "", "write the sound to this WAV file")
	coinAt := flag.Int("coin", 0, "insert a coin at this frame, and press start 120 frames later")
	cpuProf := flag.String("cpuprofile", "", "write a CPU profile here")
	flag.Parse()
	if *cpuProf != "" {
		f, _ := os.Create(*cpuProf)
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}

	zr, err := zip.OpenReader(*romPath)
	if err != nil {
		panic(err)
	}
	files := map[string][]byte{}
	for _, f := range zr.File {
		rc, _ := f.Open()
		b, _ := io.ReadAll(rc)
		rc.Close()
		files[filepath.Base(f.Name)] = b
	}
	src := model3.NewFileSource(files)
	var g *model3.GameDef
	for _, cand := range model3.Games {
		if len(cand.Missing(src)) == 0 {
			g = cand
		}
	}
	if g == nil {
		panic("no game matches")
	}
	fmt.Println("game:", g.ID)
	roms, err := g.LoadROMs(src)
	if err != nil {
		panic(err)
	}
	m, err := model3.New(g, roms)
	if err != nil {
		panic(err)
	}
	c := m.CPU()
	// the clock chip follows the emulated time, so runs repeat exactly
	m.Now = func() time.Time {
		return time.Date(2024, 1, 1, 12, 0, 0, 0, time.UTC).Add(time.Duration(float64(curFrame) / 57.52416 * float64(time.Second)))
	}
	c.NoIdleSkip = os.Getenv("M3_NOIDLE") != ""
	if *trace != "" {
		tf, err := os.ReadFile(*trace)
		if err != nil {
			panic(err)
		}
		n := len(tf) / 8
		var i int
		var hist []string
		c.Trace = func(c *ppc.CPU, pc, op uint32) {
			if i >= n {
				return
			}
			h := uint32(0)
			for k := 0; k < 32; k++ {
				h = h*31 + c.GPR[k]
			}
			h = h*31 + c.LR
			h = h*31 + c.CTR
			rpc := binary.LittleEndian.Uint32(tf[i*8:])
			rh := binary.LittleEndian.Uint32(tf[i*8+4:])
			line := fmt.Sprintf("%d pc=%08X op=%08X r3=%08X r4=%08X r5=%08X lr=%08X ctr=%08X cr=%08X", i, pc, op, c.GPR[3], c.GPR[4], c.GPR[5], c.LR, c.CTR, c.CR)
			hist = append(hist, line)
			if len(hist) > 40 {
				hist = hist[1:]
			}
			if rpc != pc || rh != h {
				for _, l := range hist {
					fmt.Println(l)
				}
				fmt.Printf("DIVERGE at %d: ref pc=%08X h=%08X, mine pc=%08X h=%08X\n", i, rpc, rh, pc, h)
				for k := 0; k < 32; k++ {
					fmt.Printf("r%d=%08X ", k, c.GPR[k])
				}
				fmt.Println()
				os.Exit(1)
			}
			i++
			if i == n {
				fmt.Println("trace matched for", n, "instructions")
			}
		}
	}
	if *bp != 0 && *trace == "" {
		left := *bpN
		c.Trace = func(c *ppc.CPU, pc, op uint32) {
			if pc == uint32(*bp) && left > 0 && curFrame >= *bpFrom {
				left--
				fmt.Printf("bp %08X lr=%08X cr=%08X ctr=%08X\n", pc, c.LR, c.CR, c.CTR)
				for k := 0; k < 32; k++ {
					fmt.Printf("r%d=%08X ", k, c.GPR[k])
				}
				fmt.Println()
				sp := c.GPR[1]
				fmt.Print("stack:")
				for k := 0; k < 12 && sp != 0 && sp < 0x800000; k++ {
					sp = c.Read32(sp)
					if sp == 0 || sp >= 0x800000 {
						break
					}
					fmt.Printf(" %08X", c.Read32(sp+4))
				}
				fmt.Println()
			}
		}
	}
	var wav []int16
	pcHist := map[uint32]int{}
	histFrom, histTo := 0, 0
	fmt.Sscanf(os.Getenv("M3_PCHIST"), "%d,%d", &histFrom, &histTo)
	if histTo > 0 {
		c.Trace = func(c *ppc.CPU, pc, op uint32) {
			if curFrame >= histFrom && curFrame < histTo {
				pcHist[pc]++
			}
		}
	}
	// M3_STICKX=from,to,adc,...: the stick's X (as the ADC reads it) over frames
	var stickX []int
	for _, p := range strings.Split(os.Getenv("M3_STICKX"), ",") {
		var v int
		if _, err := fmt.Sscan(p, &v); err == nil {
			stickX = append(stickX, v)
		}
	}
	t0 := time.Now()
	tLast := t0
	for f := 1; f <= *frames; f++ {
		curFrame = f
		if *coinAt > 0 {
			m.Inputs.Coin1 = f >= *coinAt && f < *coinAt+10
			m.Inputs.Start1 = f >= *coinAt+120 && f < *coinAt+130
			trigAt := *coinAt + 400
			if v := os.Getenv("M3_TRIGAT"); v != "" {
				fmt.Sscan(v, &trigAt)
			}
			m.Inputs.Trigger = f >= trigAt && (f/20)%2 == 0
			if os.Getenv("M3_EVENT") != "" {
				m.Inputs.Event = f >= trigAt && (f/30)%2 == 1
			}
		}
		m.Inputs.StickX, m.Inputs.StickY = 0, 0
		if sw := os.Getenv("M3_SWEEPFROM"); sw != "" {
			// M3_SWEEPFROM=frame: sweep the stick around the screen from then on
			var from int
			fmt.Sscan(sw, &from)
			if f >= from {
				m.Inputs.StickX = float32(0.85 * math.Sin(float64(f)/37))
				m.Inputs.StickY = float32(0.6 * math.Sin(float64(f)/53))
			}
		}
		for i := 0; i+2 < len(stickX); i += 3 {
			if f >= stickX[i] && f < stickX[i+1] {
				m.Inputs.StickX = float32(stickX[i+2]-0x80) / 127
			}
		}
		if pk := os.Getenv("M3_POKE"); pk != "" {
			// M3_POKE=addr:value,... writes RAM bytes before each frame
			ram, _ := m.DebugMemory()
			for _, e := range strings.Split(pk, ",") {
				var a, v int
				if _, err := fmt.Sscanf(e, "%x:%x", &a, &v); err == nil && a < len(ram) {
					ram[a] = byte(v)
				}
			}
		}
		m.RunFrame()
		for _, d := range strings.Split(os.Getenv("M3_DUMPS"), ",") {
			if d == fmt.Sprint(f) {
				ram, _ := m.DebugMemory()
				os.WriteFile(fmt.Sprintf("/tmp/claude-0/ram_%d.bin", f), ram, 0o644)
			}
		}
		if d := os.Getenv("M3_DUMP_FRAME"); d == fmt.Sprint(f) {
			ram, vram := m.DebugMemory()
			os.WriteFile("/tmp/claude-0/my_ram.bin", ram, 0o644)
			os.WriteFile("/tmp/claude-0/my_vram.bin", vram, 0o644)
		}
		if *wavPath != "" {
			for _, v := range m.Audio() {
				wav = append(wav, int16(v*32767))
			}
		}
		if err := m.Halted(); err != nil {
			fmt.Println("frame", f, err)
			break
		}
		if f%300 == 0 {
			fmt.Printf("frame %d %.1f fps\n", f, 300/time.Since(tLast).Seconds())
			tLast = time.Now()
		}
		if *out != "" && f >= *from && f%*every == 0 {
			os.MkdirAll(*out, 0o755)
			save(filepath.Join(*out, fmt.Sprintf("f%06d.png", f)), m.Picture[:])
			if os.Getenv("M3_TGREGS") != "" {
				r := m.DebugTileRegs()
				fmt.Printf("f%d 20=%08X 60=%08X 64=%08X 68=%08X 6C=%08X 40=%08X 44=%08X\n", f, r[8], r[24], r[25], r[26], r[27], r[16], r[17])
			}
		}
	}
	if *dumpRAM != "" {
		ram := make([]byte, 0x800000)
		for a := 0; a < len(ram); a += 4 {
			binary.BigEndian.PutUint32(ram[a:], c.Read32(uint32(a)))
		}
		os.WriteFile(*dumpRAM, ram, 0o644)
	}
	if histTo > 0 {
		type kv struct {
			pc uint32
			n  int
		}
		var l []kv
		total := 0
		for k, v := range pcHist {
			l = append(l, kv{k, v})
			total += v
		}
		sort.Slice(l, func(i, j int) bool { return l[i].n > l[j].n })
		fmt.Println("instructions", total)
		for i := 0; i < 40 && i < len(l); i++ {
			fmt.Printf("%08X %6.2f%% %s\n", l[i].pc, 100*float64(l[i].n)/float64(total), ppc.Disasm(l[i].pc, c.Read32(l[i].pc)))
		}
	}
	if *wavPath != "" {
		writeWAV(*wavPath, wav)
	}
	if *disFrom != 0 {
		dis(m, uint32(*disFrom), uint32(*disFrom)+uint32(*disN)*4)
	}
	fmt.Println("time", time.Since(t0), "pc", fmt.Sprintf("%08X", c.PC))
}

var dis func(m *model3.Machine, from, to uint32)

func save(path string, pic []uint32) {
	img := image.NewRGBA(image.Rect(0, 0, 496, 384))
	for i, p := range pic {
		img.Pix[i*4] = uint8(p >> 16)
		img.Pix[i*4+1] = uint8(p >> 8)
		img.Pix[i*4+2] = uint8(p)
		img.Pix[i*4+3] = 255
	}
	f, _ := os.Create(path)
	png.Encode(f, img)
	f.Close()
}

func init() {
	dis = func(m *model3.Machine, from, to uint32) {
		c := m.CPU()
		for a := from; a < to; a += 4 {
			op := c.Read32(a)
			fmt.Printf("%08X: %08X  %s\n", a, op, ppc.Disasm(a, op))
		}
	}
}

func writeWAV(path string, s []int16) {
	b := make([]byte, 44+2*len(s))
	copy(b, "RIFF")
	binary.LittleEndian.PutUint32(b[4:], uint32(36+2*len(s)))
	copy(b[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(b[16:], 16)
	binary.LittleEndian.PutUint16(b[20:], 1)
	binary.LittleEndian.PutUint16(b[22:], 2)
	binary.LittleEndian.PutUint32(b[24:], 44100)
	binary.LittleEndian.PutUint32(b[28:], 44100*4)
	binary.LittleEndian.PutUint16(b[32:], 4)
	binary.LittleEndian.PutUint16(b[34:], 16)
	copy(b[36:], "data")
	binary.LittleEndian.PutUint32(b[40:], uint32(2*len(s)))
	for i, v := range s {
		binary.LittleEndian.PutUint16(b[44+2*i:], uint16(v))
	}
	os.WriteFile(path, b, 0o644)
}
