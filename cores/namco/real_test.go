package namco

// Tests with the user's own ROM sets and MAME reference dumps; they skip
// when those aren't there (they are private and never in the repository).

import (
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

const realDir = "/mnt/project-files/roms-private/arcade-batch2/"

// work is where development runs write pictures (and find MAME's dumps).
var work = func() string {
	if d := os.Getenv("NAMCO_WORK"); d != "" {
		return d
	}
	return "/tmp/claude-0/namco"
}()

func openReal(t testing.TB, set string) *arcade.Core {
	t.Helper()
	data, err := os.ReadFile(realDir + set + ".zip")
	if err != nil {
		t.Skip("no ROM set " + set)
	}
	dips := map[string]int{}
	for _, kv := range strings.Split(os.Getenv("RP_DIPS"), ",") {
		if k, v, ok := strings.Cut(kv, "="); ok {
			dips[k], _ = strconv.Atoi(v)
		}
	}
	c, err := arcade.New(data, core.Options{Name: set + ".zip", SampleRate: 48000, Dips: dips})
	if err != nil {
		t.Fatal(err)
	}
	return c.(*arcade.Core)
}

func writePix(t testing.TB, pix []uint32, w, h int, path string) {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i, p := range pix {
		img.Set(i%w, i/w, color.RGBA{uint8(p >> 16), uint8(p >> 8), uint8(p), 255})
	}
	os.MkdirAll(filepath.Dir(path), 0o755)
	fh, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer fh.Close()
	png.Encode(fh, img)
}

func readPix(path string) ([]uint32, int, int, error) {
	fh, err := os.Open(path)
	if err != nil {
		return nil, 0, 0, err
	}
	defer fh.Close()
	img, err := png.Decode(fh)
	if err != nil {
		return nil, 0, 0, err
	}
	r := img.Bounds()
	out := make([]uint32, r.Dx()*r.Dy())
	for y := 0; y < r.Dy(); y++ {
		for x := 0; x < r.Dx(); x++ {
			cr, cg, cb, _ := img.At(r.Min.X+x, r.Min.Y+y).RGBA()
			out[y*r.Dx()+x] = arcade.RGB(int(cr>>8), int(cg>>8), int(cb>>8))
		}
	}
	return out, r.Dx(), r.Dy(), nil
}

// upright: the board's picture turned as the framework turns it.
func upright(b *board) []uint32 {
	dst := make([]uint32, scrW*scrH)
	arcade.RotateInto(b.screen, scrW, scrH, 90, dst)
	return dst
}

// script presses coin and start at fixed frames, then plays a little.
func script(c *arcade.Core, frame int) {
	b := c.Board().(*board)
	p := arcade.Pad{}
	switch {
	case frame%900 >= 120 && frame%900 < 126:
		p["Coin"] = true
	case frame%900 >= 200 && frame%900 < 206:
		p["Start (1P)"] = true
	}
	if frame > 1200 {
		k := frame / 20 % 7
		p[[]string{"Left", "Right", "Up", "Down", "Fire", "Left", "Right"}[k]] = true
		if frame%10 < 3 {
			p["Fire"] = true
		}
	}
	b.SetInput([]arcade.Pad{p, {}})
}

// RP_FRAMES=n go test -run TestRealRun: plays each set and writes a
// picture every 300 frames to $NAMCO_WORK/native/<set>/.
func TestRealRun(t *testing.T) {
	n, _ := strconv.Atoi(os.Getenv("RP_FRAMES"))
	if n == 0 {
		t.Skip("set RP_FRAMES")
	}
	for _, set := range []string{"galaga", "digdug"} {
		t.Run(set, func(t *testing.T) {
			c := openReal(t, set)
			b := c.Board().(*board)
			for i := 1; i <= n; i++ {
				script(c, i)
				c.RunFrame()
				if i%300 == 0 {
					writePix(t, upright(b), scrH, scrW, fmt.Sprintf("%s/native/%s/%05d.png", work, set, i))
				}
			}
		})
	}
}

// TestRefRender loads MAME's memory dumps into the board, draws them and
// compares with MAME's picture of the same frame ($NAMCO_WORK/ref/<set>/).
func TestRefRender(t *testing.T) {
	for _, s := range refSets() {
		dir := filepath.Join(work, "ref", s[1])
		bins, _ := filepath.Glob(dir + "/*.bin")
		if len(bins) == 0 {
			continue
		}
		sort.Strings(bins)
		c := openReal(t, s[0])
		b := c.Board().(*board)
		for _, bin := range bins {
			d, err := os.ReadFile(bin)
			if err != nil || len(d) < 0x2008 {
				continue
			}
			copy(b.vram[:], d[0:0x800])
			for i := 0; i < 3; i++ {
				copy(b.ram[i][:], d[0x800+i*0x800:0xc00+i*0x800])
			}
			copy(b.vlat[:], d[0x2000:0x2008])
			b.vid.render(b)
			got := upright(b)
			want, w, h, err := readPix(strings.TrimSuffix(bin, ".bin") + ".png")
			if err != nil || w != scrH || h != scrW {
				t.Logf("%s: no picture", bin)
				continue
			}
			diff := 0
			side := make([]uint32, w*3*h)
			for y := 0; y < h; y++ {
				for x := 0; x < w; x++ {
					g, m := got[y*w+x], want[y*w+x]
					side[y*w*3+x] = g
					side[y*w*3+w+x] = m
					if g != m {
						diff++
						side[y*w*3+2*w+x] = 0xff0000
					} else {
						side[y*w*3+2*w+x] = g >> 2 & 0x3f3f3f
					}
				}
			}
			name := filepath.Base(strings.TrimSuffix(bin, ".bin"))
			writePix(t, side, w*3, h, fmt.Sprintf("%s/compare/ref-%s-%s.png", work, s[0], name))
			t.Logf("%s frame %s: %d of %d pixels differ", s[0], name, diff, w*h)
		}
	}
}

// TestRefSprites finds, for each sprite in MAME's dumps, how far MAME draws
// it from where this board does (development aid).
func TestRefSprites(t *testing.T) {
	if os.Getenv("RP_SPRITES") == "" {
		t.Skip("set RP_SPRITES")
	}
	for _, s := range refSets() {
		dir := filepath.Join(work, "ref", s[1])
		bins, _ := filepath.Glob(dir + "/*.bin")
		if len(bins) == 0 {
			continue
		}
		c := openReal(t, s[0])
		b := c.Board().(*board)
		for _, bin := range bins {
			d, _ := os.ReadFile(bin)
			want, _, _, err := readPix(strings.TrimSuffix(bin, ".bin") + ".png")
			if err != nil {
				continue
			}
			// MAME's picture back in the board's orientation
			raw := make([]uint32, scrW*scrH)
			arcade.RotateInto(want, scrH, scrW, 270, raw)
			copy(b.vram[:], d[0:0x800])
			for i := 0; i < 3; i++ {
				copy(b.ram[i][:], d[0x800+i*0x800:0xc00+i*0x800])
			}
			for n := 0; n < 64; n++ {
				// draw only sprite n
				for i := range b.screen {
					b.screen[i] = 0x123456
				}
				saved := [3][128]uint8{}
				for i := 0; i < 3; i++ {
					copy(saved[i][:], b.ram[i][0x380:])
				}
				for m := 0; m < 64; m++ {
					if m != n {
						b.ram[2][0x380+m*2+1] |= 2
						b.ram[1][0x380+m*2] = 0
					}
				}
				b.vid.drawSprites(b, 0)
				for i := 0; i < 3; i++ {
					copy(b.ram[i][0x380:], saved[i][:])
				}
				var px []int
				for i, v := range b.screen {
					if v != 0x123456 && v != 0 {
						px = append(px, i)
					}
				}
				if len(px) < 8 {
					if n == 13 {
						t.Logf("#%d: %d px", n, len(px))
					}
					continue
				}
				best, bx, by := -1, 0, 0
				for dy := -24; dy <= 24; dy++ {
					for dx := -24; dx <= 24; dx++ {
						m := 0
						for _, i := range px {
							x, y := i%scrW+dx, i/scrW+dy
							if x >= 0 && x < scrW && y >= 0 && y < scrH && raw[y*scrW+x] == b.screen[i] {
								m++
							}
						}
						if m > best {
							best, bx, by = m, dx, dy
						}
					}
				}
				o := 0x380 + n*2
				t.Logf("%s %s #%d code %02x attr %02x %02x pos %02x %02x: %d px, best dx %d dy %d (%d match)", s[0], filepath.Base(bin), n,
					b.ram[0][o], b.ram[2][o], b.ram[2][o+1], b.ram[1][o], b.ram[1][o+1], len(px), bx, by, best)
			}
		}
	}
}

// refSets: which dump folders to use (RP_REF=galaga:galagao2,...).
func refSets() [][2]string {
	if e := os.Getenv("RP_REF"); e != "" {
		var out [][2]string
		for _, x := range strings.Split(e, ",") {
			a, b, _ := strings.Cut(x, ":")
			out = append(out, [2]string{a, b})
		}
		return out
	}
	return [][2]string{{"galaga", "galagao"}, {"digdug", "digdug1"}}
}
