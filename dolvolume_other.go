//go:build !windows && !linux

package main

// Setting one program's volume is done on Windows and Linux; elsewhere the setting is only saved.

func followDolphinVolume(pid int, exited <-chan struct{}) {}

func applyDolphinVolumeNow() {}
