//go:build !(amd64 && (linux || windows))

package wiiu

const jitSupported = false

func jitCall(entry uintptr, a *jitArgs) uint64 { panic("no recompiler here") }

func jitAlloc(size int) []byte { return nil }

func jitFree(b []byte) {}
