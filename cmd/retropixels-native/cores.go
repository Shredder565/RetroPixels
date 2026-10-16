package main

// Every emulator core (NES, Atari 2600, Master System, Game Boy / Color,
// Atari 7800, Genesis, Game Boy Advance, Nintendo 64, Super Nintendo and the
// arcade boards): package allcores registers them, with their buttons and
// default controls, in internal/games, which the Android app's engine shares.

import _ "retropixels/internal/games/allcores"
