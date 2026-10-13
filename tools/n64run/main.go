// n64run: headless test runner for the N64 core (development tool).
package main

import (
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"time"

	"retropixels/n64"
)

func main() {
	frames := flag.Int("frames", 60, "frames to run")
	every := flag.Int("every", 0, "save a picture every N frames")
	out := flag.String("out", "frame", "output prefix")
	flag.Parse()
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
	for f := 1; f <= *frames; f++ {
		m.RunFrame()
		m.Audio = m.Audio[:0]
		if (*every > 0 && f%*every == 0) || f == *frames {
			save(m, fmt.Sprintf("%s_%05d.png", *out, f))
		}
	}
	dt := time.Since(t0).Seconds()
	fmt.Printf("%d frames in %.1fs = %.0f fps, pc=%x %dx%d\n", *frames, dt, float64(*frames)/dt, m.CPU.PC(), m.FrameW, m.FrameH)
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
