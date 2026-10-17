// Package arcadeboards brings in the arcade boards written only in Go (they
// register with package arcade). RetroPixels.exe runs them through its
// WebSocket host (arcadehost.go); RetroPixels Native and Android get them
// through internal/games/allcores.
package arcadeboards

import (
	_ "retropixels/cores/atari"    // Atari Centipede, Asteroids Deluxe
	_ "retropixels/cores/berzerk"  // Stern Berzerk
	_ "retropixels/cores/capcom"   // Capcom 1942, 1943, 1943 Kai
	_ "retropixels/cores/cps1"     // Capcom CP System: 1941
	_ "retropixels/cores/ddragon3" // Technos Double Dragon 3
	_ "retropixels/cores/dec8"     // Data East DEC8: The Real Ghostbusters
	_ "retropixels/cores/namco"    // Namco Galaga, Dig Dug
	_ "retropixels/cores/psikyo"   // Psikyo PS5V2: Dragon Blaze
	_ "retropixels/cores/sega16"   // Sega System 16B: Altered Beast
	_ "retropixels/cores/technos"  // Technos Double Dragon, Double Dragon II
	_ "retropixels/cores/vsnes"    // Nintendo Vs. System: Vs. Castlevania
)
