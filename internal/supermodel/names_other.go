//go:build !windows

package supermodel

// ExeName is Supermodel's program file (Linux programs have no .exe).
const ExeName = "supermodel"

// InputSystem: controllers through SDL's game controller support, which
// gives every Xbox-style pad the same button numbers.
const InputSystem = "sdlgamepad"
