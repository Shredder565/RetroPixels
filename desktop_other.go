//go:build !linux

package main

import "net/http"

// Desktop integration (the applications menu, games opened from the file
// manager) is Linux only: see desktop_linux.go. On Windows the web app opens
// no game files (RetroPixels Native does, in its assoc_windows.go).

func registerDesktopRoutes(mux *http.ServeMux) {}

func desktopHandOff(args []string) string { return "" }

func desktopStart(args []string) string { return "" }
