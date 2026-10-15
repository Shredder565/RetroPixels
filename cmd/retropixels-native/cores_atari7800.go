package main

// The Atari 7800 (test build 2).

import "retropixels/cores/atari7800"

func init() {
	coreMakers["atari7800"] = atari7800.New
	// DEFAULT_MAPS.atari7800 in host.js
	defaultMaps["atari7800"] = ControlMap{
		"Button 1": b([]int{padA}, "z"), "Button 2": b([]int{padB}, "x"), "Pause": b([]int{padMenu}, "enter"),
		"Select": b([]int{padView}, "shift"), "Reset": b([]int{padY}, "r"),
		"Left difficulty": tg([]int{padLB}, "1"), "Right difficulty": tg([]int{padRB}, "2"),
	}
	systemButtons["atari7800"] = []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Pause", "Select", "Reset", "Left difficulty", "Right difficulty"}
}
