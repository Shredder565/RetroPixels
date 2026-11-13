// Command pixelds is PixelDS.exe, RetroPixels' own DS and 3DS emulator
// (package cores/pixelds) in its own window. The 3DS / DS tab of
// RetroPixels.exe and RetroPixels-Native.exe starts it with a game:
//
//	PixelDS.exe [-f | -w] game.nds   (or a 3DS program, game.3dsx)
//
// -f starts fullscreen, -w in a window. The game's save sits next to it
// (game.sav); save
// states go to the RetroPixels folder in the user's settings, and so do
// the 3DS's SD card and 3DS save data.
//
// For tests (and on systems without the window):
//
//	pixelds -frames N [-shot out.png] [-press A@60,Start@120] game.nds
//
// runs N frames without a window and writes the last picture.
//
// DS games can draw 3D at a higher resolution (-scale, F2) and use texture
// packs; -make-texpack makes one from the game (see textures.go).
package main

import (
	"archive/zip"
	"bytes"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"retropixels/cores/pixelds"
)

var (
	fullscreen = flag.Bool("f", false, "start fullscreen")
	windowed   = flag.Bool("w", false, "start in a window")
	frames     = flag.Int("frames", 0, "run this many frames without a window, then stop")
	shot       = flag.String("shot", "", "with -frames: write the last picture here (PNG)")
	press      = flag.String("press", "", "with -frames: buttons to press, as Name@frame,... (held 6 frames)")
	touch      = flag.String("touch", "", "with -frames: touches, as x:y@frame,... (held 6 frames)")
	showLog    = flag.Bool("log", false, "with -frames: print what the 3DS side reports")
	recordDir  = flag.String("record", "", "record the game into this folder (Frames and Audio), with -ffmpeg")
	ffmpegPath = flag.String("ffmpeg", "", "the ffmpeg program -record uses")
)

const sampleRate = 48000

// loadGame reads a game file (or the first DS / 3DS game inside a .zip).
func loadGame(path string) (string, []byte, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", nil, err
	}
	if strings.EqualFold(filepath.Ext(path), ".zip") {
		zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
		if err != nil {
			return "", nil, err
		}
		for _, f := range zr.File {
			switch strings.ToLower(filepath.Ext(f.Name)) {
			case ".nds", ".srl", ".dsi", ".3ds", ".cci", ".cxi", ".3dsx", ".cia":
				rc, err := f.Open()
				if err != nil {
					return "", nil, err
				}
				b, err := io.ReadAll(rc)
				rc.Close()
				return f.Name, b, err
			}
		}
		return "", nil, errors.New("there's no DS or 3DS game in this zip")
	}
	return filepath.Base(path), data, nil
}

// savePath: the game's save, next to it.
func savePath(game string) string {
	return strings.TrimSuffix(game, filepath.Ext(game)) + ".sav"
}

// fallbackSavePath: where saves go when the game's folder can't be written.
func fallbackSavePath(game string) string {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = os.TempDir()
	}
	base := strings.TrimSuffix(filepath.Base(game), filepath.Ext(game))
	return filepath.Join(dir, "RetroPixels", "PixelDS", "saves", base+".sav")
}

func readSave(game string) []byte {
	for _, p := range []string{savePath(game), fallbackSavePath(game)} {
		if b, err := os.ReadFile(p); err == nil && len(b) > 0 {
			return b
		}
	}
	return nil
}

func writeSave(game string, data []byte) error {
	if len(data) == 0 {
		return nil
	}
	p := savePath(game)
	if err := writeFileAtomic(p, data); err == nil {
		return nil
	}
	p = fallbackSavePath(game)
	os.MkdirAll(filepath.Dir(p), 0o755)
	return writeFileAtomic(p, data)
}

func writeFileAtomic(p string, data []byte) error {
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, p)
}

func statePath(game, title string, slot int) string {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = os.TempDir()
	}
	base := strings.TrimSuffix(filepath.Base(game), filepath.Ext(game))
	return filepath.Join(dir, "RetroPixels", "PixelDS", "states", fmt.Sprintf("%s.%d.state", base, slot))
}

func open(path string) (*pixelds.Emulator, error) {
	name, data, err := loadGame(path)
	if err != nil {
		return nil, err
	}
	o := pixelds.Options{SampleRate: sampleRate}
	if sys, _ := pixelds.Detect(name, data); sys == pixelds.ThreeDS {
		o.SDCard, o.SaveDir = threeDSFolders(path)
		o.Log = threeDSLog()
	} else {
		o.Save = readSave(path)
	}
	return pixelds.Open(name, data, o)
}

// threeDSLog: what the 3DS side reports goes to the terminal with -log,
// else to PixelDS\3DS\log.txt in the user's settings (for bug reports).
func threeDSLog() func(string) {
	if *frames > 0 {
		if !*showLog {
			return nil
		}
		return func(s string) { fmt.Fprintln(os.Stderr, s) }
	}
	dir, err := os.UserConfigDir()
	if err != nil {
		return nil
	}
	p := filepath.Join(dir, "RetroPixels", "PixelDS", "3DS")
	os.MkdirAll(p, 0o755)
	f, err := os.Create(filepath.Join(p, "log.txt"))
	if err != nil {
		return nil
	}
	return func(s string) { fmt.Fprintln(f, time.Now().Format("15:04:05.000"), s) }
}

// threeDSFolders: the 3DS's SD card (shared by all 3DS programs) and the
// game's own save data, in the RetroPixels folder of the user's settings.
func threeDSFolders(game string) (sd, save string) {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = os.TempDir()
	}
	base := strings.TrimSuffix(filepath.Base(game), filepath.Ext(game))
	root := filepath.Join(dir, "RetroPixels", "PixelDS", "3DS")
	sd, save = filepath.Join(root, "sdmc"), filepath.Join(root, "saves", base)
	os.MkdirAll(sd, 0o755)
	os.MkdirAll(save, 0o755)
	return sd, save
}

func buttonBit(name string) uint32 {
	for i, b := range pixelds.Buttons {
		if strings.EqualFold(b, name) {
			return 1 << i
		}
	}
	return 0
}

type timed struct {
	at   int
	bits uint32
	x, y int
}

func parseTimed(s string, isTouch bool) []timed {
	var out []timed
	for _, part := range strings.Split(s, ",") {
		if part == "" {
			continue
		}
		at := strings.LastIndex(part, "@")
		if at < 0 {
			continue
		}
		f, _ := strconv.Atoi(part[at+1:])
		t := timed{at: f}
		if isTouch {
			xy := strings.SplitN(part[:at], ":", 2)
			if len(xy) == 2 {
				t.x, _ = strconv.Atoi(xy[0])
				t.y, _ = strconv.Atoi(xy[1])
			}
		} else {
			t.bits = buttonBit(part[:at])
		}
		out = append(out, t)
	}
	return out
}

// headless runs frames without a window (tests, and comparing pictures).
func headless(path string) error {
	e, err := open(path)
	if err != nil {
		return err
	}
	if msg := setupTextures(e, false); msg != "" {
		fmt.Println(msg)
	}
	presses, touches := parseTimed(*press, false), parseTimed(*touch, true)
	rec := newRecorder(e)
	defer rec.stop(sampleRate)
	start := time.Now()
	for f := 0; f < *frames; f++ {
		var bits uint32
		for _, p := range presses {
			if f >= p.at && f < p.at+6 {
				bits |= p.bits
			}
		}
		e.SetButtons(bits)
		down := false
		for _, t := range touches {
			if f >= t.at && f < t.at+6 {
				e.Touch(t.x, t.y, true)
				down = true
			}
		}
		if !down {
			e.Touch(0, 0, false)
		}
		e.RunFrame()
		rec.frame(e.Screen(), e.TakeAudio(), e.FPS(), sampleRate)
		if err := e.Err(); err != nil {
			fmt.Fprintf(os.Stderr, "PixelDS: the game stopped at frame %d: %v\n", f, err)
			break
		}
	}
	el := time.Since(start)
	fmt.Printf("%s (%s): %d frames in %v (%.1f fps)\n", e.Title(), e.GameCode(), *frames, el.Round(time.Millisecond), float64(*frames)/el.Seconds())
	if e.SaveChanged() {
		writeSave(path, e.Save())
	}
	if *dumpFlag != "" {
		fmt.Printf("%d textures in %s\n", e.TexturesDumped(), *dumpFlag)
	}
	if *shot != "" {
		px, w, h := e.Display()
		img := image.NewRGBA(image.Rect(0, 0, w, h))
		for i, c := range px {
			img.Set(i%w, i/w, color.RGBA{uint8(c >> 16), uint8(c >> 8), uint8(c), 255})
		}
		f, err := os.Create(*shot)
		if err != nil {
			return err
		}
		defer f.Close()
		return png.Encode(f, img)
	}
	return nil
}

func main() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "PixelDS %s, RetroPixels' DS and 3DS emulator\n\nusage: PixelDS [-f | -w] game.nds\n\n", pixelds.Version)
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() < 1 {
		if !runWindow("") {
			flag.Usage()
			os.Exit(2)
		}
		return
	}
	path := flag.Arg(0)
	if *makePack {
		if err := makeTexturePack(path); err != nil {
			fmt.Fprintln(os.Stderr, "PixelDS:", err)
			os.Exit(1)
		}
		return
	}
	if *frames > 0 {
		if err := headless(path); err != nil {
			fmt.Fprintln(os.Stderr, "PixelDS:", err)
			os.Exit(1)
		}
		return
	}
	if !runWindow(path) {
		fmt.Fprintln(os.Stderr, "PixelDS: the window is only on Windows; use -frames here")
		os.Exit(1)
	}
}
