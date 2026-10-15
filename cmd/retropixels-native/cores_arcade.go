package main

// Arcade games (test build 4): the classic boards in cores/arcade, and the
// ones other packages register with it.

import "retropixels/cores/arcade"

func init() {
	coreMakers["arcade"] = arcade.New
	// DEFAULT_MAPS.arcade in host.js: Coin = View / 5, Start = Menu / 1 (the
	// keys arcade players know), the first button = Xbox A.
	defaultMaps["arcade"] = ControlMap{"Coin": b([]int{8}, "5", "c"), "Coin 2": b([]int{}, "6"),
		"Start (1P)": b([]int{9}, "1", "enter"), "Start (2P)": b([]int{}, "2"),
		"Fire": b([]int{0}, "z"), "Jump": b([]int{0}, "z"), "Button 1": b([]int{0}, "z"), "Button 2": b([]int{1}, "x"),
		"Button 3": b([]int{2}, "a"), "Button 4": b([]int{3}, "s"), "Service": b([]int{}, "9")}
	// every control a board may use, so all of them can be changed before a game is open
	systemButtons["arcade"] = arcade.ButtonNames
	systemNotes["arcade"] = arcade.ControlsNote
	arcadeSets = arcadeHooks{
		identify: func(names []string, sizes []int, zipName string) bool {
			_, ok := arcade.Identify(names, sizes, zipName)
			return ok
		},
		supported: arcade.SupportedText,
	}
}
