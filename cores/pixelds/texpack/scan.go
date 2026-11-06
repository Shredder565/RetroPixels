package texpack

import "strings"

// Scan finds every texture of a DS game's 3D models and texture files,
// through compressed files and NARC archives. characters names the models
// (by a part of their file path, like "mario") whose textures should be
// tried with every palette of their file.
func Scan(rom []byte, characters []string) ([]Texture, error) {
	files, err := ROMFiles(rom)
	if err != nil {
		return nil, err
	}
	var out []Texture
	var visit func(f File, depth int)
	visit = func(f File, depth int) {
		d := f.Data
		if u := Unpack(d); u != nil {
			d = u
		}
		if IsNitroModel(d) {
			out = append(out, Textures(f.Path, d, IsCharacter(f.Path, characters))...)
			return
		}
		if depth < 2 {
			for _, sub := range NARC(d, f.Path) {
				visit(sub, depth+1)
			}
		}
	}
	for _, f := range files {
		visit(f, 0)
	}
	return out, nil
}

// IsCharacter: the file belongs to one of the named characters.
func IsCharacter(path string, characters []string) bool {
	p := strings.ToLower(path)
	for _, c := range characters {
		if c != "" && strings.Contains(p, strings.ToLower(c)) {
			return true
		}
	}
	return false
}
