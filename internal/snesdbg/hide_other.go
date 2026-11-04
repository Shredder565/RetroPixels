//go:build !windows

package snesdbg

import "os/exec"

func hideWindow(cmd *exec.Cmd) {}
