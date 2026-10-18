//go:build !windows

package pixelu

// CheckVulkan: only checked on Windows.
func CheckVulkan() error { return nil }
