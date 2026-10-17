package cps1

// What differs between CPS-1 games, kept as data so more games can be added
// by writing a table entry: the CPS-B chip's register layout (Capcom made
// many CPS-B variants, each with its registers in different places and its
// own ID value, which the games check), and how the board's PAL maps each
// kind of tile number onto the graphics ROMs.

// Tile kinds, for GfxRange.Kinds.
const (
	KindSprites = 1 << iota
	KindScroll1 // 8x8 tiles
	KindScroll2 // 16x16 tiles
	KindScroll3 // 32x32 tiles
)

// CPSB is one CPS-B variant: the byte offsets (0x00-0x3e, from 0x800140)
// of its registers, -1 where the chip doesn't have one.
type CPSB struct {
	IDOffset, IDValue int    // the game reads IDValue back from IDOffset
	LayerCtrl         int    // layer order and enables
	Prio              [4]int // per tile group: the pens drawn over sprites
	PalCtrl           int    // which palette pages the palette copy fills
	// LayerEnable: the layer control bits that turn on scroll 1, 2 and 3.
	LayerEnable [3]uint16
	// Multiply unit (protection on some games): factors and the 32-bit
	// product's low and high words.
	MulA, MulB, MulLo, MulHi int
}

// GfxRange maps tile numbers of some kinds onto the graphics ROM. Numbers
// are in 64-byte units (a 16x16 tile is 2 units, an 8x8 tile 1, a 32x32
// tile 8); tiles in [Start, End] are found at (number + Offset) units.
// Tiles outside every range of their kind aren't drawn.
type GfxRange struct {
	Kinds      int
	Start, End int
	Offset     int
}

// Config is one game's board setup.
type Config struct {
	CPSB   CPSB
	Mapper []GfxRange
	// Rows (bytes of program ROM): where the program ROM pairs and words go
	// is in the game's file list; nothing else is needed here yet.
}

// cpsB05 is the CPS-B-05 (1941 and others).
var cpsB05 = CPSB{
	IDOffset: 0x20, IDValue: 0x0005,
	LayerCtrl: 0x28, Prio: [4]int{0x2a, 0x2c, 0x2e, 0x30}, PalCtrl: 0x32,
	LayerEnable: [3]uint16{0x02, 0x08, 0x20},
	MulA:        -1, MulB: -1, MulLo: -1, MulHi: -1,
}

// mapperYI24B is the YI24B PAL (1941): one 2 MB bank, split between the kinds.
var mapperYI24B = []GfxRange{
	{KindSprites, 0x0000, 0x1fff, 0},
	{KindScroll3, 0x2000, 0x3fff, 0},
	{KindScroll1, 0x4000, 0x47ff, 0},
	{KindScroll2, 0x4800, 0x7fff, 0},
}

var configs = map[string]*Config{
	"1941": {CPSB: cpsB05, Mapper: mapperYI24B},
}
