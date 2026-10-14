//go:build !windows

package main

// Setting one program's volume is Windows-only; elsewhere the setting is only saved.

func followDolphinVolume(pid int, exited <-chan struct{}) {}

func applyDolphinVolumeNow() {}
