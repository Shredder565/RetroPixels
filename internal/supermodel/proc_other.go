//go:build !windows

package supermodel

import "os/exec"

func hideConsole(cmd *exec.Cmd) {}

func shortPath(p string) string { return p }
