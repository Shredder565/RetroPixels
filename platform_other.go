//go:build !windows

package main

import (
	"fmt"
	"os"
	"os/exec"
)

// Non-Windows build exists only for testing the server.

func showError(msg string) { fmt.Fprintln(os.Stderr, msg) }

func openAppWindow(url string, wait bool) error {
	fmt.Println("Serving", url)
	if wait {
		waitForPageToClose()
	}
	return nil
}

// pickFile reads the "picked" path from an env var so tests can drive it.
func pickFile(title, filter, startDir string) (string, error) {
	return os.Getenv("RETRO_TEST_PICK"), nil
}

func pickFolder(title, startDir string) (string, error) {
	return os.Getenv("RETRO_TEST_PICK_DIR"), nil
}

func execCommand(name string, args ...string) *exec.Cmd { return exec.Command(name, args...) }

func processRunning(image string) bool { return os.Getenv("RETRO_TEST_RYU_RUNNING") == "1" }

func openFolder(p string) error {
	if _, err := os.Stat(p); err != nil {
		return fmt.Errorf("can't find %s", p)
	}
	fmt.Println("OPEN", p)
	return nil
}

func systemUnpack7z(archive, dest string) error { return fmt.Errorf("no system 7z unpacker") }
