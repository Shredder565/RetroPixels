package boxart

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"
)

// bg is the library tiles' colour, behind see-through covers.
var bg = color.RGBA{0x1f, 0x22, 0x2b, 0xff}

func encodePNG(img image.Image) []byte {
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		return nil
	}
	return b.Bytes()
}

// resize draws src onto dst (which is smaller or the same size), averaging
// the source pixels under each destination pixel, over dst's background.
func resize(dst *image.RGBA, src image.Image) {
	sr := src.Bounds()
	dw, dh := dst.Bounds().Dx(), dst.Bounds().Dy()
	sw, sh := sr.Dx(), sr.Dy()
	for y := 0; y < dh; y++ {
		y0 := sr.Min.Y + y*sh/dh
		y1 := sr.Min.Y + (y+1)*sh/dh
		if y1 <= y0 {
			y1 = y0 + 1
		}
		for x := 0; x < dw; x++ {
			x0 := sr.Min.X + x*sw/dw
			x1 := sr.Min.X + (x+1)*sw/dw
			if x1 <= x0 {
				x1 = x0 + 1
			}
			var r, g, b, a, n uint64
			for sy := y0; sy < y1; sy++ {
				for sx := x0; sx < x1; sx++ {
					cr, cg, cb, ca := src.At(sx, sy).RGBA() // premultiplied, 16-bit
					r += uint64(cr)
					g += uint64(cg)
					b += uint64(cb)
					a += uint64(ca)
					n++
				}
			}
			r, g, b, a = r/n, g/n, b/n, a/n
			// Over the background: out = src + bg*(1-a).
			inv := 0xffff - a
			o := dst.PixOffset(x, y)
			dst.Pix[o+0] = uint8((r + uint64(bg.R)*0x101*inv/0xffff) >> 8)
			dst.Pix[o+1] = uint8((g + uint64(bg.G)*0x101*inv/0xffff) >> 8)
			dst.Pix[o+2] = uint8((b + uint64(bg.B)*0x101*inv/0xffff) >> 8)
			dst.Pix[o+3] = 0xff
		}
	}
}

// Decode reads a cover file as a picture (for RetroPixels Native, which
// draws covers itself). Shrinks it to fit w x h, keeping its shape.
func Decode(path string, w, h int) (*image.RGBA, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	src, _, err := image.Decode(bytes.NewReader(b))
	if err != nil {
		return nil, err
	}
	sw, sh := src.Bounds().Dx(), src.Bounds().Dy()
	scale := min(float64(w)/float64(sw), float64(h)/float64(sh))
	if scale > 1 {
		scale = 1
	}
	nw, nh := max(1, int(float64(sw)*scale+0.5)), max(1, int(float64(sh)*scale+0.5))
	dst := image.NewRGBA(image.Rect(0, 0, nw, nh))
	resize(dst, src)
	return dst, nil
}
