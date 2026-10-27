//go:build !(amd64 && (linux || windows))

package ps3

const jitSupported = false

func jitCall(entry uintptr, a *jitArgs) uint64 { panic("no recompiler here") }

func jitAlloc(size int) []byte { return nil }

func jitFree(b []byte) {}

func cpuHasFMA() bool { return false }

func spuJitCall(entry uintptr, a *jitArgs) uint64 { panic("no recompiler here") }

func cpuHasSSE41() bool { return false }

func cpuHasAVX2() bool { return false }
