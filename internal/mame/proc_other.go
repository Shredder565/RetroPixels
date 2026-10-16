//go:build !windows

package mame

import "os/exec"

func hideProbe(cmd *exec.Cmd)   {}
func hideConsole(cmd *exec.Cmd) {}
