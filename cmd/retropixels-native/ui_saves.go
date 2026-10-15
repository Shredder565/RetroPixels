package main

// Save state slots, battery save import and export, and screenshots: the
// parts of the web page's "Save states" card (four slots with a picture and
// the time, Undo last load, Import state file, Download, Delete, Import /
// Download battery save) and the player's screenshot button, for the native
// app. The window code (ui_*_windows.go) calls these; they run anywhere so
// they can be tested.

import (
	"bytes"
	"errors"
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// slotCount: four slots, as the web page.
const slotCount = 4

// slotPath: slot 1 is the game's .state file (the one earlier test builds
// kept, so old save states are slot 1); slots 2-4 are "<game>.slot2.state"...
func slotPath(sys, game string, slot int) string {
	if slot <= 0 {
		return statePath(sys, game)
	}
	return filepath.Join(savesDir(sys), safeName(game)+fmt.Sprintf(".slot%d.state", slot+1))
}

// slotThumbPath: the slot's picture, next to it.
func slotThumbPath(sys, game string, slot int) string { return slotPath(sys, game, slot) + ".png" }

// slotInfo: what the slot holds.
type slotInfo struct {
	Used bool
	Time time.Time
}

func readSlots(sys, game string) [slotCount]slotInfo {
	var out [slotCount]slotInfo
	for i := range out {
		if st, err := os.Stat(slotPath(sys, game, i)); err == nil && !st.IsDir() {
			out[i] = slotInfo{Used: true, Time: st.ModTime()}
		}
	}
	return out
}

// slotTime: "14:03" today, "12 Oct 14:03" before (fmtTime in index.html).
func slotTime(t, now time.Time) string {
	if t.IsZero() {
		return ""
	}
	y1, m1, d1 := t.Date()
	y2, m2, d2 := now.Date()
	if y1 == y2 && m1 == m2 && d1 == d2 {
		return t.Format("15:04")
	}
	return t.Format("2 Jan 15:04")
}

// slotLabel: "Slot 2 - 14:03" or "Slot 2 (empty)", for the menu.
func slotLabel(i int, si slotInfo, now time.Time) string {
	if !si.Used {
		return fmt.Sprintf("Slot %d (empty)", i+1)
	}
	return fmt.Sprintf("Slot %d - %s", i+1, slotTime(si.Time, now))
}

// ---- the session's side (game thread) ----

// saveSlot saves a state, and a small picture of the game for the Save states window.
func (s *session) saveSlot(slot int) error {
	return s.call(func() error {
		if s.failed != nil {
			return errors.New("the game has stopped")
		}
		st, err := s.core.SaveState()
		if err != nil {
			return err
		}
		if len(st) == 0 {
			return errors.New("the emulator returned an empty state")
		}
		if err := writeFileAtomic(slotPath(s.game.System, s.game.Name, slot), encodeState(s.game.System, st)); err != nil {
			return err
		}
		w, h := s.core.Width(), s.core.Height()
		if pix := s.core.Screen(); w > 0 && h > 0 && len(pix) >= w*h {
			if b, err := thumbPNG(pix[:w*h], w, h, s.aspect, thumbHeight); err == nil {
				writeFileAtomic(slotThumbPath(s.game.System, s.game.Name, slot), b)
			}
		}
		return nil
	})
}

// loadSlot loads a slot's state (Undo last load goes back to before it).
func (s *session) loadSlot(slot int) error {
	return s.call(func() error {
		file, err := os.ReadFile(slotPath(s.game.System, s.game.Name, slot))
		if os.IsNotExist(err) {
			if slot == 0 {
				return errors.New("there's no save state for this game yet (Game > Save state, or F2)")
			}
			return fmt.Errorf("slot %d is empty", slot+1)
		}
		if err != nil {
			return err
		}
		st, err := decodeState(s.game.System, file)
		if err != nil {
			return err
		}
		return s.applyWithUndo(st)
	})
}

// applyWithUndo loads a state, keeping where the game was for Undo. Game thread.
func (s *session) applyWithUndo(st []byte) error {
	before, _ := s.core.SaveState()
	if err := s.applyState(st); err != nil {
		return err
	}
	s.undo = before
	return nil
}

// importState loads a state file from anywhere (Import state file...): a
// RetroPixels Native save state, or else the bytes go to the emulator as they
// are (the N64 also takes RetroPixels.exe's). It isn't kept in a slot until
// it's saved to one, as on the web page.
func (s *session) importState(file []byte) error {
	return s.call(func() error {
		st := file
		if bytes.HasPrefix(file, stateMagic) {
			var err error
			if st, err = decodeState(s.game.System, file); err != nil {
				return err
			}
		}
		if len(st) == 0 {
			return errors.New("that file is empty")
		}
		return s.applyWithUndo(st)
	})
}

// sram: a copy of the game's battery save (nil if the game has none).
func (s *session) sram() []byte {
	var out []byte
	s.call(func() error {
		if sr := s.core.SRAM(); sr != nil {
			out = append([]byte{}, sr...)
		}
		return nil
	})
	return out
}

// hasSRAM: the game keeps a battery save.
func (s *session) hasSRAM() bool {
	ok := false
	s.call(func() error { ok = s.core.SRAM() != nil; return nil })
	return ok
}

// ---- files ----

// deleteSlot removes a slot's state and picture.
func deleteSlot(sys, game string, slot int) error {
	err := os.Remove(slotPath(sys, game, slot))
	os.Remove(slotThumbPath(sys, game, slot))
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

// maxSRAMImport: no game's battery save comes close (N64 Controller Paks: 128 KB).
const maxSRAMImport = 8 << 20

// sramFileExts: files that are battery saves when dropped on a running game.
var sramFileExts = map[string]bool{"sav": true, "srm": true, "sram": true, "eep": true, "sra": true, "fla": true, "mpk": true}

// importSRAM puts a battery save from another emulator in place of the
// game's (the old one is kept as .sav.bak). The game must be closed first,
// so it doesn't write its own back.
func importSRAM(sys, game string, data []byte) error {
	if len(data) == 0 {
		return errors.New("that file is empty")
	}
	if len(data) > maxSRAMImport {
		return errors.New("that file is too big to be a battery save")
	}
	p := sramPath(sys, game)
	if old, err := os.ReadFile(p); err == nil && len(old) > 0 {
		if err := writeFileAtomic(p+".bak", old); err != nil {
			return fmt.Errorf("couldn't keep a copy of the old battery save: %v", err)
		}
	}
	return writeFileAtomic(p, data)
}

// ---- pictures ----

const thumbHeight = 120 // the Save states window's pictures

// toRGBA: a picture from the emulator (0x00RRGGBB) as an image.
func toRGBA(pix []uint32, w, h int) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i, p := range pix[:w*h] {
		o := i * 4
		img.Pix[o], img.Pix[o+1], img.Pix[o+2], img.Pix[o+3] = byte(p>>16), byte(p>>8), byte(p), 255
	}
	return img
}

// thumbPNG: the picture at its proper shape, outH lines high (nearest neighbour).
func thumbPNG(pix []uint32, w, h int, aspect float64, outH int) ([]byte, error) {
	if w <= 0 || h <= 0 || len(pix) < w*h {
		return nil, errors.New("no picture")
	}
	ow := int(float64(outH)*aspect + 0.5)
	if ow < 1 {
		ow = 1
	}
	dst := make([]uint32, ow*outH)
	scaleTo(dst, ow, outH, pix, w, h)
	var buf bytes.Buffer
	err := png.Encode(&buf, toRGBA(dst, ow, outH))
	return buf.Bytes(), err
}

// readThumb: a slot's picture as 0x00RRGGBB pixels (for drawing), or nil.
func readThumb(path string) ([]uint32, int, int) {
	f, err := os.Open(path)
	if err != nil {
		return nil, 0, 0
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		return nil, 0, 0
	}
	b := img.Bounds()
	w, h := b.Dx(), b.Dy()
	if w <= 0 || h <= 0 || w*h > 4<<20 {
		return nil, 0, 0
	}
	out := make([]uint32, w*h)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			r, g, bb, _ := img.At(b.Min.X+x, b.Min.Y+y).RGBA()
			out[y*w+x] = (r>>8)<<16 | (g>>8)<<8 | bb>>8
		}
	}
	return out, w, h
}

// screenshotsDir: Pictures\RetroPixels (RETRO_SHOT_DIR overrides, for tests).
func screenshotsDir() string {
	if d := os.Getenv("RETRO_SHOT_DIR"); d != "" {
		return d
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, "Pictures", "RetroPixels")
}

// saveScreenshot writes the game's picture (only the game: no bars, menus or
// messages) as "<game> <date time>.png", at its proper shape and about 720
// lines high like the recordings.
func saveScreenshot(dir, game string, pix []uint32, w, h int, aspect float64, now time.Time) (string, error) {
	if w <= 0 || h <= 0 || len(pix) < w*h {
		return "", errors.New("there's no picture yet")
	}
	ow, oh := recordSize(h, aspect)
	dst := make([]uint32, ow*oh)
	scaleTo(dst, ow, oh, pix, w, h)
	var buf bytes.Buffer
	if err := png.Encode(&buf, toRGBA(dst, ow, oh)); err != nil {
		return "", err
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	base := filepath.Join(dir, safeName(game)+" "+now.Format("2006-01-02 15-04-05"))
	out := base + ".png"
	for n := 2; ; n++ {
		if _, err := os.Stat(out); err != nil {
			break
		}
		out = fmt.Sprintf("%s (%d).png", base, n)
	}
	return out, writeFileAtomic(out, buf.Bytes())
}

// ---- recordings ----

// recentRecordings: the newest .mp4 files in the folder, newest first.
func recentRecordings(dir string, n int) []string {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	type rec struct {
		path string
		t    time.Time
	}
	var list []rec
	for _, e := range ents {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".mp4") {
			continue
		}
		if info, err := e.Info(); err == nil {
			list = append(list, rec{filepath.Join(dir, e.Name()), info.ModTime()})
		}
	}
	sort.Slice(list, func(i, j int) bool { return list[i].t.After(list[j].t) })
	var out []string
	for i := 0; i < len(list) && i < n; i++ {
		out = append(out, list[i].path)
	}
	return out
}
