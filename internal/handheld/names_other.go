//go:build !windows

package handheld

// Linux programs have no .exe.
func exeNames(names ...string) []string { return names }
