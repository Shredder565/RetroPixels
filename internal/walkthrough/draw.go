package walkthrough

import "math"

// Draw paints the hint over a picture (w*h pixels, 0x00RRGGBB): a dark
// panel along the bottom with the step and how to get there, and a yellow
// arrow at its left pointing the way.
func Draw(pix []uint32, w, h int, hint Hint) {
	if !hint.Show || w < 128 || h < 64 || len(pix) < w*h {
		return
	}
	const panelH = 22
	top := h - panelH - 4
	// panel: darken what's under it, with a thin border
	for y := top; y < h-4; y++ {
		for x := 4; x < w-4; x++ {
			i := y*w + x
			if y == top || y == h-5 || x == 4 || x == w-5 {
				pix[i] = 0xF8D838
				continue
			}
			p := pix[i]
			pix[i] = (p >> 2) & 0x3F3F3F
		}
	}
	// arrow (or a dot when the goal is right here)
	cx, cy := 4+12, top+panelH/2
	if math.IsNaN(hint.Angle) {
		fillCircle(pix, w, h, cx, cy, 5, 0xF8D838)
	} else {
		drawArrow(pix, w, h, float64(cx), float64(cy), hint.Angle, 9, 0xF8D838)
	}
	tx := 4 + 26
	drawText(pix, w, h, tx, top+4, hint.Title, 0xFFFFFF, w-tx-6)
	drawText(pix, w, h, tx, top+12, hint.Detail, 0xA8E4FC, w-tx-6)
}

func put(pix []uint32, w, h, x, y int, c uint32) {
	if x >= 0 && y >= 0 && x < w && y < h {
		pix[y*w+x] = c
	}
}

func fillCircle(pix []uint32, w, h, cx, cy, r int, c uint32) {
	for y := -r; y <= r; y++ {
		for x := -r; x <= r; x++ {
			if x*x+y*y <= r*r {
				put(pix, w, h, cx+x, cy+y, c)
			}
		}
	}
}

// drawArrow fills an arrow of half-length r centred on (cx, cy), pointing
// at angle a (0 = right, pi/2 = down).
func drawArrow(pix []uint32, w, h int, cx, cy, a, r float64, c uint32) {
	ca, sa := math.Cos(a), math.Sin(a)
	ir := int(r) + 1
	for py := -ir; py <= ir; py++ {
		for px := -ir; px <= ir; px++ {
			// rotate the pixel into the arrow's frame (u along, v across)
			fx, fy := float64(px)+0.5, float64(py)+0.5
			u := fx*ca + fy*sa
			v := -fx*sa + fy*ca
			in := false
			if u >= -r && u < 0 && math.Abs(v) <= r*0.28 { // shaft
				in = true
			}
			if u >= -r*0.1 && u <= r && math.Abs(v) <= (r-u)*0.75 { // head
				in = true
			}
			if in {
				put(pix, w, h, int(cx)+px, int(cy)+py, c)
			}
		}
	}
}

// drawText writes s in the 3x5 font (one pixel between letters), with a
// dark shadow, cut at maxW pixels.
func drawText(pix []uint32, w, h, x, y int, s string, c uint32, maxW int) {
	x0 := x
	for _, ch := range s {
		if x+3 > x0+maxW {
			return
		}
		g, ok := font[ch]
		if !ok {
			g = font['?']
		}
		for row := 0; row < 5; row++ {
			for col := 0; col < 3; col++ {
				if g[row*3+col] == '#' {
					put(pix, w, h, x+col+1, y+row+1, 0)
					put(pix, w, h, x+col, y+row, c)
				}
			}
		}
		x += 4
	}
}

// font: 3x5 capitals, digits and a little punctuation, row by row.
var font = map[rune]string{
	' ':  "...............",
	'A':  ".#.#.#####.##.#",
	'B':  "##.#.###.#.###.",
	'C':  ".###..#..#...##",
	'D':  "##.#.##.##.###.",
	'E':  "####..##.#..###",
	'F':  "####..##.#..#..",
	'G':  ".###..#.##.#.##",
	'H':  "#.##.#####.##.#",
	'I':  "###.#..#..#.###",
	'J':  "..#..#..##.#.#.",
	'K':  "#.##.###.#.##.#",
	'L':  "#..#..#..#..###",
	'M':  "#.########.##.#",
	'N':  "##.#.##.##.##.#",
	'O':  ".#.#.##.##.#.#.",
	'P':  "##.#.###.#..#..",
	'Q':  ".#.#.##.###..##",
	'R':  "##.#.###.#.##.#",
	'S':  ".###...#...###.",
	'T':  "###.#..#..#..#.",
	'U':  "#.##.##.##.####",
	'V':  "#.##.##.##.#.#.",
	'W':  "#.##.########.#",
	'X':  "#.##.#.#.#.##.#",
	'Y':  "#.##.#.#..#..#.",
	'Z':  "###..#.#.#..###",
	'0':  ".#.#.##.##.#.#.",
	'1':  ".#.##..#..#.###",
	'2':  "##...#.#.#..###",
	'3':  "##...#.#...###.",
	'4':  "#.##.####..#..#",
	'5':  "####..##...###.",
	'6':  ".###..##.#.#.#.",
	'7':  "###..#.#..#..#.",
	'8':  ".#.#.#.#.#.#.#.",
	'9':  ".#.#.#.##..###.",
	'.':  ".............#.",
	',':  "..........#.#..",
	'!':  ".#..#..#.....#.",
	'?':  "##...#.#.....#.",
	'\'': ".#..#..........",
	'-':  "......###......",
	':':  "....#.....#....",
	'/':  "..#..#.#.#..#..",
}
