//go:build !windows && !linux

package dolphin

import "errors"

func systemUnpack7z(archive, dest string) error { return errors.New("no system 7z unpacker") }
