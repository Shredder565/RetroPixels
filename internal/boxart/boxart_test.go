package boxart

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
)

func pngBytes(w, h int) []byte {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i := range img.Pix {
		img.Pix[i] = 200
	}
	var b bytes.Buffer
	_ = png.Encode(&b, img)
	return b.Bytes()
}

// fakeArt serves GameTDB, libretro-thumbnails and the Arcade Database.
func fakeArt(t *testing.T, files map[string][]byte) *int32 {
	var hits int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&hits, 1)
		if b, ok := files[r.URL.Path]; ok {
			_, _ = w.Write(b)
			return
		}
		http.NotFound(w, r)
	}))
	t.Cleanup(srv.Close)
	old := []string{gametdbBase, gametdbTitles, githubAPI, adbBase}
	oldLR := libretroBases
	gametdbBase, gametdbTitles, githubAPI, adbBase = srv.URL+"/tdb", srv.URL+"/tdbtxt", srv.URL+"/gh", srv.URL+"/adb"
	libretroBases = []string{srv.URL + "/lr", srv.URL + "/raw"}
	Dir = t.TempDir()
	titleLists = map[string][]title{}
	listCache = map[string][]string{}
	t.Cleanup(func() {
		gametdbBase, gametdbTitles, githubAPI, adbBase = old[0], old[1], old[2], old[3]
		libretroBases = oldLR
		Dir = ""
	})
	return &hits
}

func TestBaseTitle(t *testing.T) {
	for in, want := range map[string]string{
		"Legend of Zelda, The - Spirit Tracks (USA) (En,Fr,Es)": "legend of zelda spirit tracks",
		"The Legend of Zelda: Spirit Tracks":                    "legend of zelda spirit tracks",
		"025 - New Super Mario Bros. (USA)":                     "new super mario bros",
		"Mario & Luigi - Bowser's Inside Story (USA)":           "mario and luigi bowsers inside story",
	} {
		if got := baseTitle(in); got != want {
			t.Errorf("baseTitle(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestBestMatchRegion(t *testing.T) {
	list := []string{"Mario Kart DS (Japan)", "Mario Kart DS (Europe) (En,Fr,De,Es,It)", "Mario Kart DS (USA, Australia)", "Mario Kart DS (USA) (Kiosk)", "Mario Kart 7 (USA)"}
	if got := bestMatch(list, Game{System: DS, Path: "/g/Mario Kart DS.nds", Name: "Mario Kart DS"}); got != "Mario Kart DS (USA, Australia)" {
		t.Errorf("no region: %q", got)
	}
	if got := bestMatch(list, Game{System: DS, Path: "/g/Mario Kart DS (Europe).nds", Name: "Mario Kart DS"}); got != "Mario Kart DS (Europe) (En,Fr,De,Es,It)" {
		t.Errorf("Europe: %q", got)
	}
	if got := bestMatch(list, Game{System: DS, Path: "/g/Mario Kart.nds", Name: "Mario Kart"}); got != "" {
		t.Errorf("a different game matched: %q", got)
	}
}

func TestLocalPicture(t *testing.T) {
	dir := t.TempDir()
	game := filepath.Join(dir, "Metroid Prime (USA).iso")
	_ = os.WriteFile(game, []byte("x"), 0o644)
	if p := Local(Game{System: GameCube, Path: game, Name: "Metroid Prime"}); p != "" {
		t.Fatalf("found %s with no picture there", p)
	}
	_ = os.MkdirAll(filepath.Join(dir, "covers"), 0o755)
	pic := filepath.Join(dir, "covers", "Metroid Prime (USA).jpg")
	_ = os.WriteFile(pic, []byte("\xff\xd8\xff"), 0o644)
	if p := Local(Game{System: GameCube, Path: game, Name: "Metroid Prime"}); p != pic {
		t.Fatalf("Local = %q, want %q", p, pic)
	}
	next := filepath.Join(dir, "Metroid Prime (USA).png")
	_ = os.WriteFile(next, pngBytes(4, 4), 0o644)
	if p := Local(Game{System: GameCube, Path: game, Name: "Metroid Prime"}); p != next {
		t.Fatalf("a picture right next to the game should win: %q", p)
	}
}

func TestGameTDBByID(t *testing.T) {
	cover := pngBytes(500, 700)
	fakeArt(t, map[string][]byte{"/tdb/wii/cover/US/GALE01.png": cover})
	p, err := Find(Game{System: GameCube, Path: "/nowhere/melee.iso", Name: "Super Smash Bros. Melee", ID: "GALE01"})
	if err != nil {
		t.Fatal(err)
	}
	f, _ := os.Open(p)
	defer f.Close()
	cfg, _, err := image.DecodeConfig(f)
	if err != nil || cfg.Height != MaxSide || !strings.HasSuffix(p, ".jpg") {
		t.Fatalf("cover %s: %+v %v (want a JPEG %d tall)", p, cfg, err, MaxSide)
	}
	if Cached(Game{System: GameCube, Path: "/nowhere/melee.iso", Name: "Super Smash Bros. Melee", ID: "GALE01"}) != p {
		t.Fatal("not cached")
	}
}

func TestWiiUByProductCode(t *testing.T) {
	dir := t.TempDir()
	game := filepath.Join(dir, "mk8.wud")
	_ = os.WriteFile(game, []byte("WUP-P-AMKE0000"), 0o644)
	fakeArt(t, map[string][]byte{
		"/tdbtxt/wiiutdb.txt":           []byte("TITLES = https://www.gametdb.com\nAMKE01 = Mario Kart 8\nAMKP01 = Mario Kart 8\n"),
		"/tdb/wiiu/cover/US/AMKE01.jpg": pngBytes(10, 14),
	})
	if _, err := Find(Game{System: WiiU, Path: game, Name: "mk8"}); err != nil {
		t.Fatal(err)
	}
}

func TestLibretroListMatch(t *testing.T) {
	index := `<a href="../">../</a><a href="Super%20Mario%203D%20Land%20%28USA%29%20%28En%2CFr%2CEs%29.png">x</a>`
	hits := fakeArt(t, map[string][]byte{
		"/lr/Nintendo - Nintendo 3DS/Named_Boxarts/":                                         []byte(index),
		"/lr/Nintendo - Nintendo 3DS/Named_Boxarts/Super Mario 3D Land (USA) (En,Fr,Es).png": pngBytes(20, 20),
	})
	g := Game{System: N3DS, Path: "/g/Super Mario 3D Land.3ds", Name: "Super Mario 3D Land"}
	if _, err := Find(g); err != nil {
		t.Fatal(err)
	}
	// A game with no art is remembered: no second round of downloads.
	none := Game{System: N3DS, Path: "/g/Nothing Like It.3ds", Name: "Nothing Like It"}
	if _, err := Find(none); err != ErrNone {
		t.Fatalf("want ErrNone, got %v", err)
	}
	n := atomic.LoadInt32(hits)
	if _, err := Find(none); err != ErrNone || atomic.LoadInt32(hits) != n {
		t.Fatalf("looked again: %v, %d -> %d requests", err, n, atomic.LoadInt32(hits))
	}
	Forget()
	_, _ = Find(none)
	if atomic.LoadInt32(hits) == n {
		t.Fatal("Forget didn't make it look again")
	}
}

func TestArcadeDatabase(t *testing.T) {
	fakeArt(t, map[string][]byte{"/adb/flyers/dkong.png": pngBytes(30, 40)})
	if _, err := Find(Game{System: Arcade, Path: "/roms/dkong.zip", Name: "Donkey Kong (US set 1)", Short: "dkong"}); err != nil {
		t.Fatal(err)
	}
}

func TestReadIDs(t *testing.T) {
	dir := t.TempDir()
	nds := make([]byte, 0x200)
	copy(nds, "NEW MARIO\x00\x00\x00A2DE01")
	_ = os.WriteFile(filepath.Join(dir, "a.nds"), nds, 0o644)
	if id := ReadID(DS, filepath.Join(dir, "a.nds")); id != "A2DE" {
		t.Errorf("DS id %q", id)
	}
	cci := make([]byte, 0x400)
	copy(cci[0x100:], "NCSD")
	cci[0x120] = 1 // partition 0 at 0x200
	copy(cci[0x300:], "NCCH")
	copy(cci[0x350:], "CTR-P-AREE")
	_ = os.WriteFile(filepath.Join(dir, "a.3ds"), cci, 0o644)
	if id := ReadID(N3DS, filepath.Join(dir, "a.3ds")); id != "AREE" {
		t.Errorf("3DS id %q", id)
	}
	title := filepath.Join(dir, "Game [0005000010145D00]")
	_ = os.MkdirAll(filepath.Join(title, "code"), 0o755)
	_ = os.MkdirAll(filepath.Join(title, "meta"), 0o755)
	_ = os.WriteFile(filepath.Join(title, "meta", "meta.xml"), []byte(`<menu><product_code type="string" length="32">WUP-P-AMKE</product_code></menu>`), 0o644)
	if id := ReadID(WiiU, filepath.Join(title, "code", "game.rpx")); id != "AMKE" {
		t.Errorf("Wii U folder id %q", id)
	}
}

func TestDecodeTGA(t *testing.T) {
	// 2x1, 32-bit, top-down: a red and a green pixel (BGRA).
	b := []byte{0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 32, 0x28, 0, 0, 255, 255, 0, 255, 0, 255}
	img := decodeTGA(b)
	if img == nil {
		t.Fatal("not decoded")
	}
	if c := color.NRGBAModel.Convert(img.At(0, 0)).(color.NRGBA); c.R != 255 || c.G != 0 {
		t.Errorf("pixel 0 = %+v", c)
	}
	if c := color.NRGBAModel.Convert(img.At(1, 0)).(color.NRGBA); c.G != 255 || c.R != 0 {
		t.Errorf("pixel 1 = %+v", c)
	}
}
