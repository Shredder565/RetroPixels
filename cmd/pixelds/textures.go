package main

// Texture packs and the 3D resolution for DS games.
//
// A game's pack is the folder textures\<game code> in PixelDS's settings
// (%APPDATA%\RetroPixels\PixelDS), loaded when the game starts; pictures
// PixelDS makes itself go in its auto subfolder, and any picture of your
// own for the same texture wins over them. In the window:
//
//	F2   3D resolution: 1×, 2×, 3×, 4× (remembered)
//	F3   dump the game's textures into textures\<game code>\dump, on / off
//	F9   make the automatic pack (Mario restyled, everything enlarged 4×)
//	F10  texture pack on / off
//
// Without a window: -make-texpack writes the pack and stops; -scale,
// -textures and -dump-textures work with -frames too.

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"retropixels/cores/pixelds"
	"retropixels/cores/pixelds/texpack"
)

var (
	scaleFlag    = flag.Int("scale", 0, "DS 3D resolution, 1-4 times the DS's (default: the last one chosen with F2, else 1)")
	texturesFlag = flag.String("textures", "", "texture pack folder (default: textures\\<game code> in PixelDS's settings); \"off\" for none")
	dumpFlag     = flag.String("dump-textures", "", "write the game's textures into this folder as it draws them")
	makePack     = flag.Bool("make-texpack", false, "make the automatic texture pack for the DS game and stop")
	packStyle    = flag.String("style", "modern", "with -make-texpack: modern (restyle the characters) or hd (only enlarge)")
	packChars    = flag.String("chars", "mario", "with -make-texpack: the characters to restyle (parts of their model files' names, comma separated)")
	packScale    = flag.Int("pack-scale", 4, "with -make-texpack: how much bigger the pictures are")
	packOnly     = flag.Bool("only-chars", false, "with -make-texpack: only the characters' textures (else every 3D texture, enlarged)")
	packOut      = flag.String("out", "", "with -make-texpack: the folder to write (default: the game's pack folder, auto subfolder)")
)

func pixeldsDir() string {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = os.TempDir()
	}
	return filepath.Join(dir, "RetroPixels", "PixelDS")
}

// packDir is a game's texture pack folder.
func packDir(code string) string {
	code = strings.Map(func(r rune) rune {
		if r >= 'A' && r <= 'Z' || r >= 'a' && r <= 'z' || r >= '0' && r <= '9' {
			return r
		}
		return '_'
	}, code)
	if code == "" {
		code = "unknown"
	}
	return filepath.Join(pixeldsDir(), "textures", code)
}

// savedScale is the 3D resolution chosen last (settings: scale.txt).
func savedScale() int {
	b, err := os.ReadFile(filepath.Join(pixeldsDir(), "scale.txt"))
	if err != nil {
		return 1
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(b)))
	if err != nil || n < 1 || n > 4 {
		return 1
	}
	return n
}

func saveScale(n int) {
	os.MkdirAll(pixeldsDir(), 0o755)
	os.WriteFile(filepath.Join(pixeldsDir(), "scale.txt"), []byte(strconv.Itoa(n)+"\n"), 0o644)
}

// setupTextures applies -scale, -textures and -dump-textures to a DS game
// (3DS games: nothing). It returns what to tell the player, if anything.
func setupTextures(e *pixelds.Emulator, window bool) string {
	if e.System != pixelds.DS {
		return ""
	}
	sc := *scaleFlag
	if sc == 0 {
		sc = 1
		if window {
			sc = savedScale()
		}
	}
	e.SetRenderScale(sc)
	var said []string
	dir := *texturesFlag
	if dir == "" {
		dir = packDir(e.GameCode())
	}
	if dir != "off" {
		if st, err := os.Stat(dir); err == nil && st.IsDir() {
			if n, err := e.LoadTexturePack(dir); err == nil && n > 0 {
				said = append(said, fmt.Sprintf("texture pack: %d pictures", n))
			}
		}
	}
	if *dumpFlag != "" {
		if err := e.DumpTextures(*dumpFlag); err != nil {
			said = append(said, "can't dump textures: "+err.Error())
		}
	}
	return strings.Join(said, ", ")
}

// packOptions: the automatic pack's options from the flags.
func packOptions(out string) texpack.Options {
	var chars []string
	for _, c := range strings.Split(*packChars, ",") {
		if c = strings.TrimSpace(c); c != "" {
			chars = append(chars, c)
		}
	}
	style := *packStyle
	if style != "hd" {
		style = "modern"
	}
	return texpack.Options{Out: out, Scale: max(1, min(*packScale, 8)), Style: style, Characters: chars, OnlyChars: *packOnly}
}

// makeTexturePack is -make-texpack: the automatic pack, from the game file
// (and the textures dumped while playing, which have the colours the game
// sets itself, like a power-up's).
func makeTexturePack(path string) error {
	name, data, err := loadGame(path)
	if err != nil {
		return err
	}
	if sys, _ := pixelds.Detect(name, data); sys != pixelds.DS {
		return fmt.Errorf("texture packs are for DS games")
	}
	code := ""
	if len(data) >= 0x10 {
		code = strings.TrimRight(string(data[0xc:0x10]), "\x00")
	}
	out := *packOut
	if out == "" {
		out = filepath.Join(packDir(code), "auto")
	}
	o := packOptions(out)
	o.DumpDir = filepath.Join(packDir(code), "dump")
	r, err := pixelds.MakeTexturePack(data, o)
	if err != nil {
		return err
	}
	fmt.Printf("wrote %d pictures to %s (%d of the characters restyled, %d of them found in the dump folder)\n", r.Textures, out, r.Characters, r.FromDump)
	return nil
}
