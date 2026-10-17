//go:build amd64 && (linux || windows)

package wiiu

// jitCall runs recompiled code (jit_amd64.s).
//
//go:noescape
func jitCall(entry uintptr, a *jitArgs) uint64

const jitSupported = true
