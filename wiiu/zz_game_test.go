package wiiu

import (
	"fmt"
	"image"
	"image/png"
	"os"
	"strings"
	"testing"
)

// TestLocalGame runs a game you own (a .wua) for checking compatibility by
// hand; it's skipped unless WIIU_GAME names the file. Options:
//
//	WIIU_FRAMES=n       how many frames to run (600)
//	WIIU_SHOTS=dir      both screens as PNGs every 60 frames
//	WIIU_TRACE=s        log library calls whose line contains s (1 = all)
//	WIIU_TRACEFILE=f    write the whole trace to f
//	WIIU_TEX=addr       describe the texture at addr (and save tex.png)
//	WIIU_DUMP=a:n,...   print n bytes of memory at a
//	WIIU_WAV=f          the sound as raw 48 kHz 16-bit stereo
//
// After frame 2000 it presses + and A now and then, to get past menus.
func TestLocalGame(t *testing.T) {
	p := os.Getenv("WIIU_GAME")
	if p == "" {
		t.Skip()
	}
	rpx, name, files, err := TitleFromWUA(p)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("%s: %d bytes, title %016x", name, len(rpx), files.TitleID)
	ents, _ := files.ReadDir("/vol/code")
	for _, e := range ents {
		t.Logf("  code/%s %d", e.Name, e.Size)
	}
	m, err := New(rpx, name, files)
	if err != nil {
		t.Fatal(err)
	}
	var log []string
	m.Log = func(s string) { log = append(log, s) }
	var trace []string
	if os.Getenv("WIIU_TRACE") != "" {
		filter := os.Getenv("WIIU_TRACE")
		m.Trace = func(s string) {
			if filter != "1" && !strings.Contains(s, filter) {
				return
			}
			trace = append(trace, s)
			if len(trace) > 400000 {
				trace = trace[2000:]
			}
		}
	}
	defer func() {
		if f := os.Getenv("WIIU_TRACEFILE"); f != "" {
			os.WriteFile(f, []byte(strings.Join(trace, "\n")), 0o644)
		}
		if len(trace) > 150 {
			trace = trace[len(trace)-150:]
		}
		t.Log("trace:\n" + strings.Join(trace, "\n"))
	}()
	frames := 600
	if v := os.Getenv("WIIU_FRAMES"); v != "" {
		fmt.Sscan(v, &frames)
	}
	var wav []float32
	defer func() {
		if f := os.Getenv("WIIU_WAV"); f != "" {
			b := make([]byte, 0, len(wav)*2)
			for _, x := range wav {
				v := int16(x * 32767)
				b = append(b, byte(v), byte(v>>8))
			}
			os.WriteFile(f, b, 0o644)
		}
	}()
	var peak float32
	audioN := 0
	for i := 0; i < frames && !m.Exited && m.Err == nil; i++ {
		// press + and A now and then once the logos are past
		var btn uint32
		if i > 2000 && i%120 < 6 {
			btn = BtnPlus
		} else if i > 2000 && i%120 >= 60 && i%120 < 66 {
			btn = BtnA
		}
		m.SetGamePad(PadState{Buttons: btn})
		m.RunFrame()
		au := m.TakeAudio()
		wav = append(wav, au...)
		for _, a := range au {
			audioN++
			peak = max(peak, a, -a)
		}
		if d := os.Getenv("WIIU_SHOTS"); d != "" && (i+1)%60 == 0 {
			for which := 0; which < 2; which++ {
				pix, w, h := m.Picture(which)
				img := image.NewRGBA(image.Rect(0, 0, w, h))
				for k, p := range pix[:w*h] {
					img.Pix[k*4], img.Pix[k*4+1], img.Pix[k*4+2], img.Pix[k*4+3] = byte(p>>16), byte(p>>8), byte(p), 255
				}
				f, _ := os.Create(fmt.Sprintf("%s/f%04d_%d.png", d, i+1, which))
				png.Encode(f, img)
				f.Close()
			}
		}
	}
	if a := os.Getenv("WIIU_DUMP"); a != "" {
		for _, part := range strings.Split(a, ",") {
			var da, dn uint32
			fmt.Sscanf(part, "%x:%x", &da, &dn)
			var sb strings.Builder
			for i := uint32(0); i < dn; i += 4 {
				if i%32 == 0 {
					fmt.Fprintf(&sb, "\n%08x:", da+i)
				}
				fmt.Fprintf(&sb, " %08x", m.Mem.Read32(da+i))
			}
			t.Log(sb.String())
		}
	}
	if a := os.Getenv("WIIU_TEX"); a != "" {
		var ta uint32
		fmt.Sscanf(a, "%x", &ta)
		mem := m.Mem
		img := mem.Read32(ta + surfImage)
		size := mem.Read32(ta + surfSize)
		nz := 0
		for i := uint32(0); i < size; i += 4 {
			if mem.Read32(img+i) != 0 {
				nz++
			}
		}
		gs := m.readSurface(ta)
		lay, w, h := gs.level(0)
		timg := image.NewRGBA(image.Rect(0, 0, int(w), int(h)))
		hist := map[uint32]int{}
		for y := uint32(0); y < h; y++ {
			for x := uint32(0); x < w; x++ {
				v := mem.Read32(lay.addr(x, y, 0))
				hist[v]++
				o := (int(y)*int(w) + int(x)) * 4
				timg.Pix[o], timg.Pix[o+1], timg.Pix[o+2], timg.Pix[o+3] = byte(v>>24), byte(v>>16), byte(v>>8), 255
			}
		}
		if d := os.Getenv("WIIU_SHOTS"); d != "" {
			f, _ := os.Create(d + "/tex.png")
			png.Encode(f, timg)
			f.Close()
		}
		t.Logf("distinct values %d", len(hist))
		n := 0
		for v, c := range hist {
			if n++; n > 8 {
				break
			}
			t.Logf("  %08x x%d", v, c)
		}
		t.Logf("texture %08x: %dx%d fmt %x tile %d image %08x size %d: %d non-zero words", ta, mem.Read32(ta+surfWidth), mem.Read32(ta+surfHeight),
			mem.Read32(ta+surfFormat), mem.Read32(ta+surfTile), img, size, nz)
	}
	t.Logf("audio: %d samples, peak %.3f", audioN, peak)
	t.Logf("exited %v code %d err %v draws %d shader err %q\n%s", m.Exited, int32(m.ExitCode), m.Err, m.gpu.Draws, m.gpu.ShaderErr, m.ThreadsSummary())
	if len(log) > 400 {
		log = log[len(log)-400:]
	}
	t.Log("\n" + strings.Join(log, "\n"))
}
