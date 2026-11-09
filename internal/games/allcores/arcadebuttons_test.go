package allcores

import (
	"os"
	"strings"
	"testing"

	"retropixels/cores/arcade"
	"retropixels/internal/games"
)

// Every control an arcade board uses must have a default key and
// controller button in both programs, or it does nothing until the player
// sets it (Popeye's and Kangaroo's Punch did).
func TestArcadeButtonsHaveDefaults(t *testing.T) {
	js, err := os.ReadFile("../../../web/emu/host.js")
	if err != nil {
		t.Fatal(err)
	}
	m := games.DefaultMaps["arcade"]
	for _, b := range arcade.Boards() {
		lists := [][]string{b.Buttons}
		for _, g := range b.Games {
			lists = append(lists, g.Buttons)
		}
		for _, l := range lists {
			for _, n := range l {
				if _, dir := games.ArrowKeys[n]; dir {
					continue
				}
				bd, ok := m[n]
				if !ok || len(bd.Keys) == 0 {
					t.Errorf("%s: %q has no default key in RetroPixels Native", b.ID, n)
				}
				if !strings.Contains(string(js), n+": {") && !strings.Contains(string(js), `"`+n+`": {`) {
					t.Errorf("%s: %q has no default in web/emu/host.js (RetroPixels.exe)", b.ID, n)
				}
			}
		}
	}
}
