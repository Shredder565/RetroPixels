//go:build fakecore

package main

// Test builds (go build -tags fakecore) play .rptest files on the test core.
func init() {
	systems = append(systems, system{"fake", "Test pattern", []string{"rptest"}, 4.0 / 3})
	syncSystems()
	coreMakers["fake"] = newTestCore
}
