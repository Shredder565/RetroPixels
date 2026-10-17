// Package allcores brings every RetroPixels Native emulator core into a
// program: importing it registers each core with package games, with its
// buttons and default controls (DEFAULT_MAPS in web/emu/host.js).
package allcores

import (
	"errors"

	"retropixels/cores/arcade"
	_ "retropixels/cores/arcadeboards" // the Go-only arcade boards (1941, 1942, 1943, Altered Beast, ...)
	"retropixels/cores/atari2600"
	"retropixels/cores/atari7800"
	"retropixels/cores/gb"
	"retropixels/cores/gba"
	"retropixels/cores/genesis"
	_ "retropixels/cores/konami/boards" // TMNT, Turtles in Time, The Simpsons (they register with the arcade core)
	"retropixels/cores/n64"
	"retropixels/cores/nes"
	"retropixels/cores/sms"
	"retropixels/cores/snes"
	"retropixels/cores/wiiu"
	. "retropixels/internal/games"
)

func init() {
	// The emulators of test build 1.
	Makers["nes"] = nes.New
	Makers["atari2600"] = atari2600.New

	// The Master System: Xbox A / B = buttons 1 / 2 (the same spots as on the
	// SMS pad); Menu = the console's Pause button; R = its Reset button.
	Makers["sms"] = sms.New
	DefaultMaps["sms"] = ControlMap{"1": B([]int{0}, "z"), "2": B([]int{1}, "x"),
		"Pause": B([]int{9}, "enter"), "Reset": B([]int{}, "r")}
	SystemButtons["sms"] = sms.Buttons

	// The Game Boy / Color: Xbox B (right) = A, Xbox A (bottom) = B, like the
	// original pad. The tilt keys only show for tilt cartridges (their game
	// lists them); the right stick tilts too.
	Makers["gb"] = gb.New
	DefaultMaps["gb"] = ControlMap{"A": B([]int{1}, "x"), "B": B([]int{0}, "z"), "Select": B([]int{8}, "shift"), "Start": B([]int{9}, "enter"),
		"Tilt Up": B([]int{}, "i"), "Tilt Down": B([]int{}, "k"), "Tilt Left": B([]int{}, "j"), "Tilt Right": B([]int{}, "l")}
	SystemButtons["gb"] = gb.Buttons

	// The Atari 7800.
	Makers["atari7800"] = atari7800.New
	DefaultMaps["atari7800"] = ControlMap{
		"Button 1": B([]int{PadA}, "z"), "Button 2": B([]int{PadB}, "x"), "Pause": B([]int{PadMenu}, "enter"),
		"Select": B([]int{PadView}, "shift"), "Reset": B([]int{PadY}, "r"),
		"Left difficulty": TG([]int{PadLB}, "1"), "Right difficulty": TG([]int{PadRB}, "2"),
	}
	SystemButtons["atari7800"] = []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Pause", "Select", "Reset", "Left difficulty", "Right difficulty"}

	// The Sega Genesis / Mega Drive: Xbox X / A / B = A / B / C (the same row
	// as on the Genesis pad), LB / Y / RB = X / Y / Z, View = Mode. Players 3
	// and 4 are the J-Cart's pads (the third and fourth controllers).
	Makers["segaMD"] = genesis.New
	DefaultMaps["segaMD"] = ControlMap{"A": B([]int{2}, "a"), "B": B([]int{0}, "s"), "C": B([]int{1}, "d"),
		"X": B([]int{4}, "q"), "Y": B([]int{3}, "w"), "Z": B([]int{5}, "e"),
		"Start": B([]int{9}, "enter"), "Mode": B([]int{8}, "shift")}
	SystemButtons["segaMD"] = genesis.Buttons

	// The Game Boy Advance: Xbox B (right) = A, Xbox A (bottom) = B, like the
	// original pad; LB / RB = L / R. The sensor keys only show for the carts
	// that have them (their game lists them); the right stick works them too.
	Makers["gba"] = gba.New
	DefaultMaps["gba"] = ControlMap{"A": B([]int{1}, "x"), "B": B([]int{0}, "z"), "L": B([]int{4}, "q"), "R": B([]int{5}, "w"),
		"Select": B([]int{8}, "shift"), "Start": B([]int{9}, "enter"),
		"Tilt Up": B([]int{}, "i"), "Tilt Down": B([]int{}, "k"), "Tilt Left": B([]int{}, "j"), "Tilt Right": B([]int{}, "l"),
		"Turn Left": B([]int{}, "j"), "Turn Right": B([]int{}, "l"), "More Sun": B([]int{}, "i"), "Less Sun": B([]int{}, "k")}
	SystemButtons["gba"] = gba.Buttons

	// The Nintendo 64: Xbox A / X = A / B (same spots as the N64's big
	// buttons), LT = Z. The left stick is the analog stick and the right
	// stick the C buttons; on the keyboard the arrows move the stick.
	Makers["n64"] = n64.New
	DefaultMaps["n64"] = ControlMap{
		"A": B([]int{PadA}, "x"), "B": B([]int{PadX}, "z"), "Z": B([]int{PadLT}, "a"), "Start": B([]int{PadMenu}, "enter"),
		"Up": B([]int{}, "t"), "Down": B([]int{}, "g"), "Left": B([]int{}, "f"), "Right": B([]int{}, "h"),
		"L": B([]int{PadLB}, "q"), "R": B([]int{PadRB}, "w"),
		"C-Up": B([]int{}, "i"), "C-Down": B([]int{}, "k"), "C-Left": B([]int{}, "j"), "C-Right": B([]int{}, "l"),
		"Stick Up": B([]int{}, "arrowup"), "Stick Down": B([]int{}, "arrowdown"),
		"Stick Left": B([]int{}, "arrowleft"), "Stick Right": B([]int{}, "arrowright"),
	}
	SystemButtons["n64"] = n64.Buttons
	SystemNotes["n64"] = n64.ControlsNote

	// The Wii U: the same button positions as the GamePad's diamond (Xbox B =
	// A, A = B, Y = X, X = Y), LB / RB = L / R, LT / RT = ZL / ZR, Menu / View
	// = Plus / Minus. Both sticks are the GamePad's; on the keyboard the
	// arrows move the left stick.
	Makers["wiiu"] = wiiu.New
	DefaultMaps["wiiu"] = ControlMap{
		"A": B([]int{PadB}, "x"), "B": B([]int{PadA}, "z"), "X": B([]int{PadY}, "s"), "Y": B([]int{PadX}, "a"),
		"L": B([]int{PadLB}, "q"), "R": B([]int{PadRB}, "w"), "ZL": B([]int{PadLT}, "1"), "ZR": B([]int{PadRT}, "2"),
		"Plus": B([]int{PadMenu}, "enter"), "Minus": B([]int{PadView}, "shift"), "Home": B([]int{}, "h"),
		"Up": B([]int{}, "i"), "Down": B([]int{}, "k"), "Left": B([]int{}, "j"), "Right": B([]int{}, "l"),
		"L Stick Click": B([]int{}, "c"), "R Stick Click": B([]int{}, "v"), // (L3 / R3 load and save states)
		"Stick Up": B([]int{}, "arrowup"), "Stick Down": B([]int{}, "arrowdown"),
		"Stick Left": B([]int{}, "arrowleft"), "Stick Right": B([]int{}, "arrowright"),
		"TV / GamePad": B([]int{}, "g"),
	}
	SystemButtons["wiiu"] = wiiu.Buttons
	SystemNotes["wiiu"] = wiiu.ControlsNote

	// The Super Nintendo: the same button positions as the Xbox pad's diamond
	// (Xbox B = A, A = B, Y = X, X = Y), LB / RB = L / R.
	Makers["snes"] = snes.New
	DefaultMaps["snes"] = ControlMap{"A": B([]int{1}, "x"), "B": B([]int{0}, "z"), "X": B([]int{3}, "s"), "Y": B([]int{2}, "a"),
		"L": B([]int{4}, "q"), "R": B([]int{5}, "w"), "Select": B([]int{8}, "shift"), "Start": B([]int{9}, "enter")}
	SystemButtons["snes"] = snes.Buttons
	// games whose cartridge chip runs a program the player supplies
	NeedsFirmware = func(err error) (chip, file, msg string, ok bool) {
		var fe *snes.FirmwareError
		if errors.As(err, &fe) {
			return fe.Chip, fe.File, fe.Msg, true
		}
		return "", "", "", false
	}

	// Arcade games: the classic boards in cores/arcade, and the ones other
	// packages register with it. Coin = View / 5, Start = Menu / 1 (the keys
	// arcade players know), the first button = Xbox A.
	Makers["arcade"] = arcade.New
	DefaultMaps["arcade"] = ControlMap{"Coin": B([]int{8}, "5", "c"), "Coin 2": B([]int{}, "6"),
		"Start (1P)": B([]int{9}, "1", "enter"), "Start (2P)": B([]int{}, "2"),
		"Fire": B([]int{0}, "z"), "Jump": B([]int{0}, "z"), "Button 1": B([]int{0}, "z"), "Button 2": B([]int{1}, "x"),
		"Button 3": B([]int{2}, "a"), "Button 4": B([]int{3}, "s"), "Service": B([]int{}, "9")}
	// every control a board may use, so all of them can be changed before a game is open
	SystemButtons["arcade"] = arcade.ButtonNames
	SystemNotes["arcade"] = arcade.ControlsNote
	Arcade = ArcadeHooks{
		Identify: func(names []string, sizes []int, zipName string) bool {
			_, ok := arcade.Identify(names, sizes, zipName)
			return ok
		},
		Supported: arcade.SupportedText,
	}
}
