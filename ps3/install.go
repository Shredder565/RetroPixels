package ps3

import (
	"errors"
	"os"
	"path/filepath"
)

// Install unpacks a PSN package into dir (the game's folder: PARAM.SFO,
// USRDIR, ...) and saves the game's key next to it.
func Install(pkgPath, dir string, progress func(done, total uint64)) error {
	f, err := os.Open(pkgPath)
	if err != nil {
		return err
	}
	defer f.Close()
	p, err := OpenPKG(f)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	if err := p.Extract(dir, progress); err != nil {
		return err
	}
	// the game's own key, for its modules and data files
	eboot, err := os.ReadFile(filepath.Join(dir, "USRDIR", "EBOOT.BIN"))
	if err != nil {
		return errors.New("the package has no USRDIR/EBOOT.BIN: it isn't a game")
	}
	elf, err := DecryptSELF(eboot, nil)
	if err != nil {
		return err
	}
	ents, _ := os.ReadDir(filepath.Join(dir, "USRDIR"))
	for _, e := range ents {
		if e.IsDir() {
			continue
		}
		b := make([]byte, 0x80)
		ff, err := os.Open(filepath.Join(dir, "USRDIR", e.Name()))
		if err != nil {
			continue
		}
		ff.ReadAt(b, 0)
		ff.Close()
		if string(b[:4]) == "NPD\x00" {
			if k, ok := FindKlicensee(elf, b); ok {
				return os.WriteFile(filepath.Join(dir, "klicensee.bin"), k, 0o644)
			}
		}
	}
	return nil
}
