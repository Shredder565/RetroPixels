package main

// PlayStation 3 in the player's RPCS3: the parts of ps3_windows.go that
// don't need Windows.

import (
	"path/filepath"
	"strings"

	"retropixels/internal/rpcs3"
)

// ps3FileKind: what a file opened or dropped is to RPCS3: "pkg" (a game
// package to install), "licence" (a .rap or .edat to add), "" (not a PS3 file).
func ps3FileKind(path string) string {
	switch strings.ToLower(filepath.Ext(path)) {
	case ".pkg":
		return "pkg"
	case ".rap", ".edat":
		return "licence"
	}
	return ""
}

// ps3GameLabel is a game in the PS3 > Play menu: its name (& shown as it
// is), title ID, and what it still needs.
func ps3GameLabel(g rpcs3.Game) string {
	label := strings.ReplaceAll(g.Title, "&", "&&") + "  (" + g.TitleID + ")"
	if !g.Licence.OK() {
		label += " - needs its licence"
	}
	return label
}

// ps3InUseLabel is the PS3 > Play menu's line saying which RPCS3 is in use.
func ps3InUseLabel(st rpcs3.Status) string {
	switch st.Source {
	case rpcs3.SourceBundled:
		v := strings.TrimPrefix(st.Version, "RPCS3 ")
		if v != "" {
			v = " " + v
		}
		return "Using the RPCS3" + v + " that comes with RetroPixels"
	case rpcs3.SourceChosen:
		return "Using your chosen RPCS3: " + strings.ReplaceAll(st.Root, "&", "&&")
	}
	return "Using your own RPCS3: " + strings.ReplaceAll(st.Root, "&", "&&")
}
