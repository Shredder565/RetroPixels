package texpack

import (
	"fmt"
	"image"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"retropixels/cores/pixelds/nds"
)

// Options for Make.
type Options struct {
	Out        string   // the pack folder
	Scale      int      // how much bigger the pictures are (1-8)
	Style      string   // "modern" restyles the characters' colours; "" or "hd" only enlarges
	Characters []string // the models the style applies to (parts of file paths), e.g. "mario"
	OnlyChars  bool     // write only the characters' textures
	DumpDir    string   // textures PixelDS dumped while playing (palette changes the game makes)
}

// Report says what Make wrote.
type Report struct {
	Textures   int // pictures written
	Characters int // of those, the characters' (restyled)
	FromDump   int // dumped textures found to be the characters' with other colours
}

type pic struct {
	w, h int
	px   []float64 // r, g, b, a in 0-1, row by row
}

func fromTexels(w, h int, t []uint32, clear []bool) pic {
	p := pic{w, h, make([]float64, w*h*4)}
	for i, v := range t {
		a := float64(v>>16&31) / 31
		if clear != nil && clear[i] {
			a = 0
		}
		c := v & 0x7fff
		p.px[i*4] = float64(c&31) / 31
		p.px[i*4+1] = float64(c>>5&31) / 31
		p.px[i*4+2] = float64(c>>10&31) / 31
		p.px[i*4+3] = a
	}
	return p
}

// Make writes a texture pack for a DS game into o.Out.
func Make(rom []byte, o Options) (Report, error) {
	var r Report
	if o.Scale < 1 {
		o.Scale = 4
	}
	o.Scale = min(o.Scale, 8)
	texs, err := Scan(rom, o.Characters)
	if err != nil {
		return r, err
	}
	if err := os.MkdirAll(o.Out, 0o755); err != nil {
		return r, err
	}
	// the characters' first: their textures also turn up in other models
	// (the title screen's, the map's), and should be restyled there too
	sort.SliceStable(texs, func(a, b int) bool {
		return IsCharacter(texs[a].File, o.Characters) && !IsCharacter(texs[b].File, o.Characters)
	})
	done := map[uint64]bool{}
	var chars []pic // the characters' own textures, to find their recoloured versions in a dump
	for _, t := range texs {
		isChar := IsCharacter(t.File, o.Characters)
		if o.OnlyChars && !isChar {
			continue
		}
		texels := nds.DecodeTexture(&t.Data)
		h := nds.HashTexels(t.Data.Format, t.Data.W, t.Data.H, texels)
		if done[h] {
			continue
		}
		done[h] = true
		var clear []bool
		if t.Clear0 {
			clear = color0Of(&t.Data)
		}
		p := fromTexels(t.Data.W, t.Data.H, texels, clear)
		restyle := isChar && o.Style == "modern"
		if restyle {
			chars = append(chars, p)
		}
		name := nds.HashName(h) + "_" + safeName(t.File, t.Name, t.PalName) + ".png"
		if err := writePic(filepath.Join(o.Out, name), finish(p, o.Scale, restyle)); err != nil {
			return r, err
		}
		r.Textures++
		if isChar {
			r.Characters++
		}
	}
	// textures PixelDS dumped: the characters' textures in colours the game
	// set while playing (a power-up's), found by having the same picture
	// with a different palette
	if o.DumpDir != "" && o.Style == "modern" {
		es, _ := os.ReadDir(o.DumpDir)
		for _, e := range es {
			if len(e.Name()) < 16 || !strings.EqualFold(filepath.Ext(e.Name()), ".png") {
				continue
			}
			var h uint64
			if _, err := fmt.Sscanf(e.Name()[:16], "%016x", &h); err != nil || done[h] {
				continue
			}
			d, err := readPic(filepath.Join(o.DumpDir, e.Name()))
			if err != nil {
				continue
			}
			match := false
			for _, c := range chars {
				if samePicture(c, d) {
					match = true
					break
				}
			}
			if !match {
				continue
			}
			done[h] = true
			if err := writePic(filepath.Join(o.Out, nds.HashName(h)+"_dumped.png"), finish(d, o.Scale, true)); err != nil {
				return r, err
			}
			r.Textures++
			r.Characters++
			r.FromDump++
		}
	}
	return r, nil
}

func color0Of(d *nds.TexData) []bool {
	z := make([]bool, d.W*d.H)
	for i := range z {
		switch d.Format {
		case nds.Tex4Color:
			z[i] = int(d.Texels[i/4])>>(i&3*2)&3 == 0
		case nds.Tex16Color:
			z[i] = int(d.Texels[i/2])>>(i&1*4)&15 == 0
		case nds.Tex256Color:
			z[i] = d.Texels[i] == 0
		}
	}
	return z
}

func safeName(parts ...string) string {
	var b strings.Builder
	for _, p := range parts {
		p = strings.TrimSuffix(filepath.Base(p), ".bin")
		if p == "" || p == "." {
			continue
		}
		if b.Len() > 0 {
			b.WriteByte('_')
		}
		for _, c := range p {
			if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-' {
				b.WriteRune(c)
			} else {
				b.WriteByte('-')
			}
		}
	}
	s := b.String()
	if len(s) > 80 {
		s = s[:80]
	}
	return s
}

// samePicture: d is c with each colour swapped for another (a palette
// change): wherever c has the same colour, d does too.
func samePicture(c, d pic) bool {
	if c.w != d.w || c.h != d.h {
		return false
	}
	m := map[[3]int][3]int{}
	same, n := 0, 0
	for i := 0; i < c.w*c.h; i++ {
		if c.px[i*4+3] == 0 || d.px[i*4+3] == 0 {
			continue
		}
		k := [3]int{int(c.px[i*4]*31 + .5), int(c.px[i*4+1]*31 + .5), int(c.px[i*4+2]*31 + .5)}
		v := [3]int{int(d.px[i*4]*31 + .5), int(d.px[i*4+1]*31 + .5), int(d.px[i*4+2]*31 + .5)}
		n++
		if w, ok := m[k]; !ok {
			m[k] = v
			same++
		} else if w == v {
			same++
		}
	}
	// at least a few colours, nearly all texels consistent, and not the
	// very same colours (that would have had the same hash)
	return n > 0 && len(m) >= 4 && float64(same) >= 0.98*float64(n)
}

// finish restyles (when asked) and enlarges a texture.
func finish(p pic, scale int, restyle bool) pic {
	if restyle {
		p = modern(p)
	}
	q := enlarge(p, scale)
	if restyle {
		q = grain(q)
	}
	return q
}

// ---------------- the modern style ----------------

func rgb2hsv(r, g, b float64) (h, s, v float64) {
	mx, mn := math.Max(r, math.Max(g, b)), math.Min(r, math.Min(g, b))
	v = mx
	if mx == 0 {
		return 0, 0, 0
	}
	s = (mx - mn) / mx
	if mx == mn {
		return 0, s, v
	}
	d := mx - mn
	switch mx {
	case r:
		h = (g - b) / d
	case g:
		h = 2 + (b-r)/d
	default:
		h = 4 + (r-g)/d
	}
	h *= 60
	if h < 0 {
		h += 360
	}
	return
}

func hsv2rgb(h, s, v float64) (r, g, b float64) {
	h = math.Mod(h, 360)
	if h < 0 {
		h += 360
	}
	c := v * s
	x := c * (1 - math.Abs(math.Mod(h/60, 2)-1))
	m := v - c
	switch {
	case h < 60:
		r, g, b = c, x, 0
	case h < 120:
		r, g, b = x, c, 0
	case h < 180:
		r, g, b = 0, c, x
	case h < 240:
		r, g, b = 0, x, c
	case h < 300:
		r, g, b = x, 0, c
	default:
		r, g, b = c, 0, x
	}
	return r + m, g + m, b + m
}

// modern recolours a character's texture towards the colours of today's
// Mario games: a deeper, warmer red with softer shadows, a brighter royal
// blue, gold buttons, a softer brown and more natural skin. Shading is
// kept (each texel keeps its brightness, lifted a little in the shadows),
// so the texture's painted detail stays where it was.
func modern(p pic) pic {
	q := pic{p.w, p.h, append([]float64(nil), p.px...)}
	for i := 0; i < p.w*p.h; i++ {
		r, g, b := p.px[i*4], p.px[i*4+1], p.px[i*4+2]
		h, s, v := rgb2hsv(r, g, b)
		switch {
		case s < 0.15: // white, grey, black: as they are
			continue
		case (h < 12 || h > 335) && s > 0.45: // red: cap and shirt
			h, s, v = 354, 0.86, 0.12+0.9*v
		case h > 190 && h < 255 && s > 0.35: // blue: overalls (and eyes)
			h, s, v = 221, 0.80, 0.16+0.92*v
		case h >= 38 && h <= 70 && s > 0.45 && v > 0.5: // yellow: buttons
			h, s, v = 44, 0.82, 0.1+0.92*v
		case h >= 8 && h < 45 && v < 0.62 && s > 0.45: // brown: hair, shoes
			h, s, v = 21, 0.62, 0.05+1.05*v
		case h >= 8 && h < 45 && v >= 0.62: // skin
			h, s = 23, math.Min(s, 0.42)*0.9
		default:
			continue
		}
		r, g, b = hsv2rgb(h, s, math.Min(v, 1))
		q.px[i*4], q.px[i*4+1], q.px[i*4+2] = r, g, b
	}
	return q
}

// grain gives coloured cloth a faint weave, so an enlarged texture doesn't
// look flat.
func grain(p pic) pic {
	for y := 0; y < p.h; y++ {
		for x := 0; x < p.w; x++ {
			i := (y*p.w + x) * 4
			_, s, _ := rgb2hsv(p.px[i], p.px[i+1], p.px[i+2])
			if s < 0.5 || p.px[i+3] == 0 {
				continue
			}
			n := float64(hash2(x, y)%1000)/1000 - 0.5
			weave := 0.5 + 0.5*math.Sin(float64(x+y)*1.3)*math.Sin(float64(x-y)*1.3)
			f := 1 + 0.035*n + 0.025*(weave-0.5)
			for k := 0; k < 3; k++ {
				p.px[i+k] = math.Min(1, p.px[i+k]*f)
			}
		}
	}
	return p
}

func hash2(x, y int) uint32 {
	h := uint32(x)*374761393 + uint32(y)*668265263
	h = (h ^ h>>13) * 1274126177
	return h ^ h>>16
}

// ---------------- enlarging ----------------

// enlarge makes a picture n times bigger with Catmull-Rom (bicubic)
// filtering on premultiplied colour. A picture whose texels are either
// see-through or not stays that way (sharp edges, now smooth in shape).
func enlarge(p pic, n int) pic {
	if n <= 1 {
		return p
	}
	binary := true
	for i := 0; i < p.w*p.h; i++ {
		if a := p.px[i*4+3]; a != 0 && a != 1 {
			binary = false
			break
		}
	}
	q := pic{p.w * n, p.h * n, make([]float64, p.w*n*p.h*n*4)}
	at := func(x, y, k int) float64 {
		x = min(max(x, 0), p.w-1)
		y = min(max(y, 0), p.h-1)
		i := (y*p.w + x) * 4
		if k == 3 {
			return p.px[i+3]
		}
		return p.px[i+k] * p.px[i+3]
	}
	cr := func(t float64) [4]float64 {
		t2, t3 := t*t, t*t*t
		return [4]float64{
			(-t3 + 2*t2 - t) / 2,
			(3*t3 - 5*t2 + 2) / 2,
			(-3*t3 + 4*t2 + t) / 2,
			(t3 - t2) / 2,
		}
	}
	for y := 0; y < q.h; y++ {
		fy := (float64(y)+0.5)/float64(n) - 0.5
		y0 := int(math.Floor(fy))
		wy := cr(fy - float64(y0))
		for x := 0; x < q.w; x++ {
			fx := (float64(x)+0.5)/float64(n) - 0.5
			x0 := int(math.Floor(fx))
			wx := cr(fx - float64(x0))
			var c [4]float64
			for k := 0; k < 4; k++ {
				var s float64
				for j := 0; j < 4; j++ {
					var row float64
					for i := 0; i < 4; i++ {
						row += wx[i] * at(x0-1+i, y0-1+j, k)
					}
					s += wy[j] * row
				}
				c[k] = s
			}
			a := math.Min(1, math.Max(0, c[3]))
			o := (y*q.w + x) * 4
			if a > 0 {
				for k := 0; k < 3; k++ {
					q.px[o+k] = math.Min(1, math.Max(0, c[k]/a))
				}
			}
			if binary {
				if a >= 0.5 {
					a = 1
				} else {
					a = 0
				}
			}
			q.px[o+3] = a
		}
	}
	return q
}

// ---------------- files ----------------

func writePic(path string, p pic) error {
	img := image.NewNRGBA(image.Rect(0, 0, p.w, p.h))
	for i := 0; i < p.w*p.h; i++ {
		for k := 0; k < 4; k++ {
			img.Pix[i*4+k] = uint8(math.Round(math.Min(1, math.Max(0, p.px[i*4+k])) * 255))
		}
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	if err := png.Encode(f, img); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

func readPic(path string) (pic, error) {
	f, err := os.Open(path)
	if err != nil {
		return pic{}, err
	}
	defer f.Close()
	img, _, err := image.Decode(f)
	if err != nil {
		return pic{}, err
	}
	b := img.Bounds()
	p := pic{b.Dx(), b.Dy(), make([]float64, b.Dx()*b.Dy()*4)}
	for y := 0; y < p.h; y++ {
		for x := 0; x < p.w; x++ {
			r, g, bl, a := img.At(b.Min.X+x, b.Min.Y+y).RGBA()
			i := (y*p.w + x) * 4
			if a > 0 {
				p.px[i], p.px[i+1], p.px[i+2] = float64(r)/float64(a), float64(g)/float64(a), float64(bl)/float64(a)
			}
			p.px[i+3] = float64(a) / 0xffff
		}
	}
	return p, nil
}
