package main

// Box art in the library lists (GameCube / Wii and arcade): a small cover
// beside each game, found by internal/boxart (a picture next to the game,
// else GameTDB / libretro-thumbnails, kept in a cache) in the background.
// Games without one keep a plain tile.

import (
	"image"
	"sync"
	"unsafe"

	"retropixels/internal/boxart"
)

var (
	pImageListCreate   = comctl32.NewProc("ImageList_Create")
	pImageListAdd      = comctl32.NewProc("ImageList_Add")
	pCreateDIBSection  = gdi32.NewProc("CreateDIBSection")
	coverAgentOnce     sync.Once
	coverPlaceholderBG = [3]byte{0x2b, 0x2f, 0x3a} // R, G, B
)

const (
	lvmSetImageList = 0x1003
	lvmSetItemW     = 0x104C
	lvsilSmall      = 1
	ilcColor32      = 0x20
)

// coverList keeps one list view's covers.
type coverList struct {
	lv, il uintptr
	w, h   int
	index  map[string]int // game key -> picture in the image list

	mu    sync.Mutex
	asked map[string]bool
	ready map[string]*image.RGBA // found, not yet in the image list
}

func coverKey(g boxart.Game) string { return g.System + "|" + g.Path }

// newCoverList gives the list view a cover column of w x h pictures.
func newCoverList(lv uintptr, w, h int) *coverList {
	coverAgentOnce.Do(func() { boxart.UserAgent = "RetroPixels-Native/" + version })
	c := &coverList{lv: lv, w: w, h: h, index: map[string]int{}, asked: map[string]bool{}, ready: map[string]*image.RGBA{}}
	c.il = call(pImageListCreate, uintptr(w), uintptr(h), ilcColor32, 64, 64)
	if c.il == 0 {
		return c
	}
	blank := image.NewRGBA(image.Rect(0, 0, w, h))
	for i := 0; i < len(blank.Pix); i += 4 {
		blank.Pix[i], blank.Pix[i+1], blank.Pix[i+2], blank.Pix[i+3] = coverPlaceholderBG[0], coverPlaceholderBG[1], coverPlaceholderBG[2], 255
	}
	c.add(blank) // picture 0: no cover (yet)
	// The list view owns the image list from here (and frees it with itself).
	call(pSendMessageW, lv, lvmSetImageList, lvsilSmall, c.il)
	return c
}

// image: the game's picture in the image list (0 until its cover is found).
func (c *coverList) image(g boxart.Game) int32 {
	if c == nil || c.il == 0 {
		return 0
	}
	return int32(c.index[coverKey(g)])
}

// askVisible looks for the covers of the rows in view (and a few more), in
// the background; poll puts them in. n is the number of rows.
func (c *coverList) askVisible(n int, game func(row int) boxart.Game) {
	if c == nil || c.il == 0 || n == 0 {
		return
	}
	top := int(int32(call(pSendMessageW, c.lv, 0x1027 /* LVM_GETTOPINDEX */, 0, 0)))
	per := int(int32(call(pSendMessageW, c.lv, 0x1028 /* LVM_GETCOUNTPERPAGE */, 0, 0)))
	for i := max(0, top); i < min(n, top+per+8); i++ {
		c.ask(game(i))
	}
}

func (c *coverList) ask(g boxart.Game) {
	k := coverKey(g)
	if _, ok := c.index[k]; ok {
		return
	}
	c.mu.Lock()
	asked := c.asked[k]
	c.asked[k] = true
	c.mu.Unlock()
	if !asked {
		go func() {
			p, err := boxart.Find(g)
			if err != nil || p == "" {
				return
			}
			img, err := boxart.Decode(p, c.w, c.h)
			if err != nil {
				return
			}
			c.mu.Lock()
			c.ready[k] = img
			c.mu.Unlock()
		}()
	}
}

// poll puts the covers found since the last call into the image list, and
// gives each one's key and picture to set (on the window's thread: the
// library's timer calls it).
func (c *coverList) poll(set func(key string, img int32)) {
	if c == nil || c.il == 0 {
		return
	}
	c.mu.Lock()
	ready := c.ready
	if len(ready) > 0 {
		c.ready = map[string]*image.RGBA{}
	}
	c.mu.Unlock()
	for k, img := range ready {
		if i := c.add(img); i >= 0 {
			c.index[k] = i
			set(k, int32(i))
		}
	}
}

// forget: games without a cover are looked for again ("Find games again").
func (c *coverList) forget() {
	if c == nil {
		return
	}
	boxart.Forget()
	c.mu.Lock()
	for k := range c.asked {
		if _, ok := c.index[k]; !ok {
			delete(c.asked, k)
		}
	}
	c.mu.Unlock()
}

// setRow shows picture img on a row.
func (c *coverList) setRow(row int, img int32) {
	it := lvItem{Mask: 2 /* LVIF_IMAGE */, Item: int32(row), Image: img}
	call(pSendMessageW, c.lv, lvmSetItemW, 0, uintptr(unsafe.Pointer(&it)))
}

// add puts a picture (centred on the tile colour) into the image list.
func (c *coverList) add(src *image.RGBA) int {
	w, h := c.w, c.h
	pix := make([]byte, w*h*4) // BGRA, top-down
	for i := 0; i < len(pix); i += 4 {
		pix[i], pix[i+1], pix[i+2], pix[i+3] = coverPlaceholderBG[2], coverPlaceholderBG[1], coverPlaceholderBG[0], 255
	}
	sw, sh := src.Bounds().Dx(), src.Bounds().Dy()
	ox, oy := (w-sw)/2, (h-sh)/2
	for y := 0; y < sh; y++ {
		for x := 0; x < sw; x++ {
			dx, dy := x+ox, y+oy
			if dx < 0 || dy < 0 || dx >= w || dy >= h {
				continue
			}
			s := src.PixOffset(src.Bounds().Min.X+x, src.Bounds().Min.Y+y)
			d := (dy*w + dx) * 4
			pix[d], pix[d+1], pix[d+2], pix[d+3] = src.Pix[s+2], src.Pix[s+1], src.Pix[s], 255
		}
	}
	bmi := bitmapInfoHeader{Width: int32(w), Height: -int32(h), Planes: 1, BitCount: 32}
	bmi.Size = uint32(unsafe.Sizeof(bmi))
	var bits *byte
	hbm := call(pCreateDIBSection, 0, uintptr(unsafe.Pointer(&bmi)), 0 /* DIB_RGB_COLORS */, uintptr(unsafe.Pointer(&bits)), 0, 0)
	if hbm == 0 || bits == nil {
		return -1
	}
	copy(unsafe.Slice(bits, len(pix)), pix)
	i := int(int32(call(pImageListAdd, c.il, hbm, 0)))
	call(pDeleteObject, hbm)
	return i
}
