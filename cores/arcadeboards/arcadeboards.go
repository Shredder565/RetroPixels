// Package arcadeboards brings in the arcade boards written only in Go (they
// register with package arcade). RetroPixels.exe runs them through its
// WebSocket host (arcadehost.go); RetroPixels Native and Android get them
// through internal/games/allcores.
package arcadeboards

import (
	_ "retropixels/cores/atari"         // Atari Centipede, Asteroids Deluxe
	_ "retropixels/cores/berzerk"       // Stern Berzerk
	_ "retropixels/cores/capcom"        // Capcom 1942, 1943, 1943 Kai
	_ "retropixels/cores/cps1"          // Capcom CP System: 1941
	_ "retropixels/cores/ddragon3"      // Technos Double Dragon 3
	_ "retropixels/cores/dec8"          // Data East DEC8: The Real Ghostbusters
	_ "retropixels/cores/dkong3"        // Nintendo Donkey Kong 3
	_ "retropixels/cores/irem"          // Irem M62 (Kung-Fu Master)
	_ "retropixels/cores/kangaroo"      // Sun Electronics Kangaroo
	_ "retropixels/cores/konami/boards" // Konami X-Men (and TMNT, Turtles in Time, The Simpsons, which RetroPixels.exe runs from web/emu)
	_ "retropixels/cores/mario"         // Nintendo Mario Bros.
	_ "retropixels/cores/model3"        // Sega Model 3: Star Wars Trilogy Arcade
	_ "retropixels/cores/namco"         // Namco Galaga, Dig Dug
	_ "retropixels/cores/popeye"        // Nintendo Popeye
	_ "retropixels/cores/psikyo"        // Psikyo PS5V2: Dragon Blaze
	_ "retropixels/cores/sega16"        // Sega System 16B: Altered Beast
	_ "retropixels/cores/technos"       // Technos Double Dragon, Double Dragon II
	_ "retropixels/cores/vsnes"         // Nintendo Vs. System: Vs. Castlevania
)
