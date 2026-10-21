//go:build !windows

package pixelu

const (
	// ExeName is PixelU's program file (it ships in a "PixelU" folder with
	// its resources, gameProfiles and portable folders). Linux programs
	// have no .exe.
	ExeName = "PixelU"
	// WiiExeName is the Wii / GameCube engine's program file.
	WiiExeName = "PixelU-Wii"
)
