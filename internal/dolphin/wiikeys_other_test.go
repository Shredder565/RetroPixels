//go:build !windows

package dolphin

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

// Linux: the player's own folders (by the desktop's names), the home
// folder, then the mount folders that are there.
func TestWiiKeysRootsLinux(t *testing.T) {
	home := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", "")
	os.MkdirAll(filepath.Join(home, ".config"), 0o755)
	os.WriteFile(filepath.Join(home, ".config", "user-dirs.dirs"), []byte("XDG_DOWNLOAD_DIR=\"$HOME/Descargas\"\n"), 0o644)
	media := t.TempDir()
	got := wiiKeysRoots(home, []string{media, filepath.Join(media, "missing")})
	want := []string{filepath.Join(home, "Descargas"), filepath.Join(home, "Desktop"), filepath.Join(home, "Documents"), home, media}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("roots = %q\nwant %q", got, want)
	}
	if got := wiiKeysRoots("", nil); len(got) != 0 {
		t.Errorf("no home: %q", got)
	}
}
