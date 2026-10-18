package wiiu

// The system font. Wii U games get the console's fonts from
// OSGetSharedData as TrueType data. RetroPixels ships no Nintendo fonts:
// it uses a CafeStd.ttf the player supplies (next to the game or
// RetroPixels, or RETROPIXELS_WIIU_FONT), otherwise a font already on the
// computer (Windows' Arial, or a common Linux font).

import (
	"os"
	"path/filepath"
)

// FontDirs are folders to look for the player's CafeStd.ttf in (the core
// sets the game's and RetroPixels' folders).
var FontDirs []string

func findSharedFont() []byte {
	var tries []string
	if p := os.Getenv("RETROPIXELS_WIIU_FONT"); p != "" {
		tries = append(tries, p)
	}
	for _, d := range FontDirs {
		tries = append(tries, filepath.Join(d, "CafeStd.ttf"))
	}
	if w := os.Getenv("WINDIR"); w != "" {
		for _, f := range []string{"arial.ttf", "segoeui.ttf", "tahoma.ttf"} {
			tries = append(tries, filepath.Join(w, "Fonts", f))
		}
	}
	tries = append(tries, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
		"/usr/share/fonts/TTF/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf")
	for _, p := range tries {
		if b, err := os.ReadFile(p); err == nil && len(b) > 12 && len(b) < 12<<20 {
			return b
		}
	}
	return nil
}

func init() {
	reg("coreinit", "OSGetSharedData", func(m *Machine, cpu *CPU, t *Thread) {
		// (type: 0 Chinese, 1 Korean, 2 standard, 3 Taiwanese; flags; out address; out size)
		if m.fontAddr == 0 && !m.fontTried {
			m.fontTried = true
			if b := findSharedFont(); b != nil {
				a := m.allocSystem(uint32(len(b)), 0x100)
				m.Mem.WriteBytes(a, b)
				m.fontAddr, m.fontSize = a, uint32(len(b))
			} else {
				m.logf("no font for the program: put a CafeStd.ttf next to the game or RetroPixels")
			}
		}
		if m.fontAddr == 0 {
			ret(cpu, 0)
			return
		}
		if p := arg(cpu, 2); p != 0 {
			m.Mem.Write32(p, m.fontAddr)
		}
		if p := arg(cpu, 3); p != 0 {
			m.Mem.Write32(p, m.fontSize)
		}
		ret(cpu, 1)
	})
}
