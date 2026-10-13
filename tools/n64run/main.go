// n64run: headless test runner for the N64 core (development tool).
package main

import (
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"runtime/pprof"
	"time"

	"retropixels/n64"
)

func main() {
	frames := flag.Int("frames", 60, "frames to run")
	every := flag.Int("every", 0, "save a picture every N frames")
	out := flag.String("out", "frame", "output prefix")
	prof := flag.String("cpuprofile", "", "write a CPU profile here")
	wav := flag.String("wav", "", "write the sound to this WAV file")
	flag.Parse()
	if *prof != "" {
		f, err := os.Create(*prof)
		if err != nil {
			panic(err)
		}
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}
	rom, err := os.ReadFile(flag.Arg(0))
	if err != nil {
		panic(err)
	}
	m, err := n64.New(rom)
	if err != nil {
		panic(err)
	}
	fmt.Printf("%s %q\n", m.GameID(), m.Title())
	t0 := time.Now()
	var sound []int16
	rate := 0
	for f := 1; f <= *frames; f++ {
		m.RunFrame()
		sound = append(sound, m.Audio...)
		if m.AudioRate != 0 {
			rate = m.AudioRate
		}
		m.Audio = m.Audio[:0]
		if (*every > 0 && f%*every == 0) || f == *frames {
			save(m, fmt.Sprintf("%s_%05d.png", *out, f))
		}
	}
	dt := time.Since(t0).Seconds()
	if *wav != "" {
		writeWAV(*wav, sound, rate)
	}
	if len(sound) > 0 {
		peak := 0
		for _, v := range sound {
			if int(v) > peak {
				peak = int(v)
			} else if -int(v) > peak {
				peak = -int(v)
			}
		}
		fmt.Printf("audio: %d Hz, %d stereo samples (%.2f s), peak %d\n", rate, len(sound)/2, float64(len(sound)/2)/float64(max(rate, 1)), peak)
	}
	fmt.Printf("%d frames in %.1fs = %.0f fps, pc=%x %dx%d\n", *frames, dt, float64(*frames)/dt, m.CPU.PC(), m.FrameW, m.FrameH)
	fmt.Printf("vi %x\n", m.VIRegs())
}

func save(m *n64.Machine, name string) {
	img := image.NewRGBA(image.Rect(0, 0, m.FrameW, m.FrameH))
	for y := 0; y < m.FrameH; y++ {
		for x := 0; x < m.FrameW; x++ {
			c := m.Frame[y*m.FrameW+x]
			img.Set(x, y, color.RGBA{uint8(c), uint8(c >> 8), uint8(c >> 16), 255})
		}
	}
	f, _ := os.Create(name)
	png.Encode(f, img)
	f.Close()
}

func writeWAV(name string, s []int16, rate int) {
	b := make([]byte, 44+len(s)*2)
	put32 := func(o int, v uint32) { b[o], b[o+1], b[o+2], b[o+3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24) }
	put16 := func(o int, v uint16) { b[o], b[o+1] = byte(v), byte(v>>8) }
	copy(b[0:], "RIFF")
	put32(4, uint32(36+len(s)*2))
	copy(b[8:], "WAVEfmt ")
	put32(16, 16)
	put16(20, 1)
	put16(22, 2)
	put32(24, uint32(rate))
	put32(28, uint32(rate*4))
	put16(32, 4)
	put16(34, 16)
	copy(b[36:], "data")
	put32(40, uint32(len(s)*2))
	for i, v := range s {
		put16(44+i*2, uint16(v))
	}
	os.WriteFile(name, b, 0o644)
}
