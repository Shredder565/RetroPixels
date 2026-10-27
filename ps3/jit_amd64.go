//go:build amd64 && (linux || windows)

package ps3

// jitCall runs recompiled code (jit_amd64.s).
//
//go:noescape
func jitCall(entry uintptr, a *jitArgs) uint64

const jitSupported = true

func cpuHasFMA() bool

// spuJitCall runs recompiled SPU code (jit_amd64.s).
//
//go:noescape
func spuJitCall(entry uintptr, a *jitArgs) uint64

func cpuHasSSE41() bool

func cpuHasAVX2() bool
