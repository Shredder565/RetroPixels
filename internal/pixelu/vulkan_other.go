//go:build !windows && !linux

package pixelu

// CheckVulkan: only checked on Windows.
func CheckVulkan() error { return nil }
