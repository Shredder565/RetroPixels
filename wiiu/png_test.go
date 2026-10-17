package wiiu

import (
	"image"
	"image/color"
	"image/png"
	"os"
	"testing"
)

func savePNG(t testing.TB, path string, pix []uint32, w, h int) {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			v := pix[y*w+x]
			img.Set(x, y, color.RGBA{uint8(v >> 16), uint8(v >> 8), uint8(v), 255})
		}
	}
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	png.Encode(f, img)
}
