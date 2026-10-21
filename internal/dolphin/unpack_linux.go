package dolphin

import (
	"errors"
	"fmt"
	"os/exec"
	"strings"
)

// systemUnpack7z unpacks a .7z with 7-Zip (7zz, 7z or 7za) or bsdtar when
// one is installed; Unpack7z falls back to RetroPixels' own extractor.
func systemUnpack7z(archive, dest string) error {
	for _, z := range []string{"7zz", "7z", "7za"} {
		if p, err := exec.LookPath(z); err == nil {
			if out, err := exec.Command(p, "x", "-y", "-bd", "-o"+dest, "--", archive).CombinedOutput(); err != nil {
				return fmt.Errorf("%s: %v %s", z, err, strings.TrimSpace(string(out)))
			}
			return nil
		}
	}
	if p, err := exec.LookPath("bsdtar"); err == nil {
		if out, err := exec.Command(p, "-x", "-f", archive, "-C", dest).CombinedOutput(); err != nil {
			return fmt.Errorf("bsdtar: %v %s", err, strings.TrimSpace(string(out)))
		}
		return nil
	}
	return errors.New("no system 7z unpacker")
}
