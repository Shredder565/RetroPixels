package games

import (
	"bytes"
	"errors"
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// Store is where a program keeps its saves and chip programs:
//
//	<Dir>/saves/<system>/<game>.sav                battery save
//	<Dir>/saves/<system>/<game>.state              save state slot 1
//	<Dir>/saves/<system>/<game>.slotN.state        slots 2-4
//	<Dir>/saves/<system>/<game>...state.png        each slot's picture
//	<Dir>/firmware/<chip>                          a cartridge chip's program
type Store struct{ Dir string }

var unsafeName = regexp.MustCompile(`[<>:"/\\|?*\x00-\x1f]+`)

// SafeName makes a game's name usable as a file name (on Windows too).
func SafeName(s string) string {
	s = unsafeName.ReplaceAllString(s, "")
	s = strings.TrimSpace(strings.TrimRight(s, ". "))
	switch strings.ToUpper(strings.SplitN(s, ".", 2)[0]) {
	case "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3":
		s = "_" + s
	}
	if s == "" {
		return "game"
	}
	if r := []rune(s); len(r) > 120 {
		s = string(r[:120])
	}
	return s
}

// SlotCount: four save state slots, as the web page.
const SlotCount = 4

func (st Store) SavesDir(sys string) string { return filepath.Join(st.Dir, "saves", sys) }

// SRAMPath: saves/<system>/<game>.sav
func (st Store) SRAMPath(sys, game string) string {
	return filepath.Join(st.SavesDir(sys), SafeName(game)+".sav")
}

// StatePath: saves/<system>/<game>.state (slot 1)
func (st Store) StatePath(sys, game string) string {
	return filepath.Join(st.SavesDir(sys), SafeName(game)+".state")
}

// SlotPath: slot 1 (0 here) is the game's .state file; slots 2-4 are
// "<game>.slot2.state"...
func (st Store) SlotPath(sys, game string, slot int) string {
	if slot <= 0 {
		return st.StatePath(sys, game)
	}
	return filepath.Join(st.SavesDir(sys), SafeName(game)+fmt.Sprintf(".slot%d.state", slot+1))
}

// SlotThumbPath: the slot's picture, next to it.
func (st Store) SlotThumbPath(sys, game string, slot int) string {
	return st.SlotPath(sys, game, slot) + ".png"
}

// DeleteSlot removes a slot's state and picture.
func (st Store) DeleteSlot(sys, game string, slot int) error {
	err := os.Remove(st.SlotPath(sys, game, slot))
	os.Remove(st.SlotThumbPath(sys, game, slot))
	if os.IsNotExist(err) {
		return nil
	}
	return err
}

// MaxSRAMImport: no game's battery save comes close (N64 Controller Paks: 128 KB).
const MaxSRAMImport = 8 << 20

// ImportSRAM puts a battery save from another emulator in place of the
// game's (the old one is kept as .sav.bak). The game must be closed first,
// so it doesn't write its own back.
func (st Store) ImportSRAM(sys, game string, data []byte) error {
	if len(data) == 0 {
		return errors.New("that file is empty")
	}
	if len(data) > MaxSRAMImport {
		return errors.New("that file is too big to be a battery save")
	}
	p := st.SRAMPath(sys, game)
	if old, err := os.ReadFile(p); err == nil && len(old) > 0 {
		if err := WriteFileAtomic(p+".bak", old); err != nil {
			return fmt.Errorf("couldn't keep a copy of the old battery save: %v", err)
		}
	}
	return WriteFileAtomic(p, data)
}

// FirmwareDir: firmware/<chip>, the cartridge chip programs the player
// supplied (the Super Nintendo's DSP-3, DSP-4, ST010, ST011 and ST018 games
// need one), each asked for once.
func (st Store) FirmwareDir() string { return filepath.Join(st.Dir, "firmware") }

// LoadFirmware reads every kept chip program, by chip name.
func (st Store) LoadFirmware() map[string][]byte {
	m := map[string][]byte{}
	es, _ := os.ReadDir(st.FirmwareDir())
	for _, e := range es {
		if e.IsDir() {
			continue
		}
		if b, err := os.ReadFile(filepath.Join(st.FirmwareDir(), e.Name())); err == nil && len(b) > 0 {
			m[e.Name()] = b
		}
	}
	return m
}

// SaveFirmware keeps a chip program the player chose.
func (st Store) SaveFirmware(chip string, b []byte) error {
	if err := os.MkdirAll(st.FirmwareDir(), 0o755); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(st.FirmwareDir(), SafeName(chip)), b, 0o644)
}

// NeedsFirmware tells whether a core refused a game for want of a chip
// program (set by the cores that ask for one, through allcores): the chip's
// name, the usual file name and the message to show.
var NeedsFirmware = func(err error) (chip, file, msg string, ok bool) { return }

// Save-state files: "RPN1", the system's id length and id, then the core's state.
var StateMagic = []byte("RPN1")

func EncodeState(sys string, data []byte) []byte {
	out := make([]byte, 0, len(data)+len(sys)+5)
	out = append(out, StateMagic...)
	out = append(out, byte(len(sys)))
	out = append(out, sys...)
	return append(out, data...)
}

func DecodeState(sys string, file []byte) ([]byte, error) {
	if len(file) < 5 || !bytes.Equal(file[:4], StateMagic) {
		return nil, errors.New("that isn't a RetroPixels Native save state")
	}
	n := int(file[4])
	if len(file) < 5+n {
		return nil, errors.New("that save state is damaged")
	}
	if string(file[5:5+n]) != sys {
		return nil, errors.New("that save state is for a different system")
	}
	return file[5+n:], nil
}

// WriteFileAtomic writes to a temporary file then renames it into place, so
// a crash never leaves half a save.
func WriteFileAtomic(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}

// ---- pictures ----

// ToRGBA: a picture from the emulator (0x00RRGGBB) as an image.
func ToRGBA(pix []uint32, w, h int) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for i, p := range pix[:w*h] {
		o := i * 4
		img.Pix[o], img.Pix[o+1], img.Pix[o+2], img.Pix[o+3] = byte(p>>16), byte(p>>8), byte(p), 255
	}
	return img
}

// ScaleTo resizes a picture (nearest neighbour).
func ScaleTo(dst []uint32, dw, dh int, src []uint32, sw, sh int) {
	for y := 0; y < dh; y++ {
		sy := y * sh / dh
		row := src[sy*sw : sy*sw+sw]
		out := dst[y*dw : y*dw+dw]
		for x := range out {
			out[x] = row[x*sw/dw]
		}
	}
}

// ThumbPNG: the picture at its proper shape, outH lines high (nearest neighbour).
func ThumbPNG(pix []uint32, w, h int, aspect float64, outH int) ([]byte, error) {
	if w <= 0 || h <= 0 || len(pix) < w*h {
		return nil, errors.New("no picture")
	}
	ow := int(float64(outH)*aspect + 0.5)
	if ow < 1 {
		ow = 1
	}
	dst := make([]uint32, ow*outH)
	ScaleTo(dst, ow, outH, pix, w, h)
	var buf bytes.Buffer
	err := png.Encode(&buf, ToRGBA(dst, ow, outH))
	return buf.Bytes(), err
}

// ToPCM converts a core's float samples (mono or interleaved stereo) to
// 16-bit stereo for the sound card, at volume 0..1, clipping.
func ToPCM(dst []int16, src []float32, stereo bool, volume float32) []int16 {
	dst = dst[:0]
	conv := func(v float32) int16 {
		v *= volume * 32767
		if v > 32767 {
			return 32767
		}
		if v < -32768 {
			return -32768
		}
		if v != v { // NaN
			return 0
		}
		return int16(v)
	}
	if stereo {
		for _, v := range src {
			dst = append(dst, conv(v))
		}
		if len(dst)%2 == 1 {
			dst = append(dst, dst[len(dst)-1])
		}
		return dst
	}
	for _, v := range src {
		s := conv(v)
		dst = append(dst, s, s)
	}
	return dst
}
