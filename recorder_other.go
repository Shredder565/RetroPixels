//go:build !windows && !linux

package main

import "os/exec"

func hiddenCommand(name string, args ...string) *exec.Cmd { return exec.Command(name, args...) }

func lowPriority(cmd *exec.Cmd) *exec.Cmd { return cmd }
