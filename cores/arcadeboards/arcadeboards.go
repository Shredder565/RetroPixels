// Package arcadeboards brings in the arcade boards written only in Go (they
// register with package arcade). RetroPixels.exe runs them through its
// WebSocket host (arcadehost.go); RetroPixels Native and Android get them
// through internal/games/allcores.
package arcadeboards

import (
	_ "retropixels/cores/berzerk" // Stern Berzerk
	_ "retropixels/cores/capcom"  // Capcom 1942, 1943, 1943 Kai
	_ "retropixels/cores/cps1"    // Capcom CP System: 1941
	_ "retropixels/cores/vsnes"   // Nintendo Vs. System: Vs. Castlevania
)
