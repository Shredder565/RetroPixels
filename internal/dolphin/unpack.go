package dolphin

import (
	"os"

	"retropixels/sevenzip"
)

// Unpack7z uses Windows' own tar.exe when it can read .7z (Windows 11), and
// RetroPixels' own extractor otherwise.
func Unpack7z(archive, dest string) error {
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}
	if err := systemUnpack7z(archive, dest); err == nil {
		return nil
	}
	os.RemoveAll(dest)
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}
	return sevenzip.ExtractAll(archive, dest, nil)
}
