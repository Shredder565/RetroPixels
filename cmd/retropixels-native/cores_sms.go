package main

// The Master System (test build 2).

import "retropixels/cores/sms"

func init() {
	coreMakers["sms"] = sms.New
	// DEFAULT_MAPS.sms in host.js: Xbox A / B = buttons 1 / 2 (the same spots
	// as on the SMS pad); Menu = the console's Pause button; R = its Reset button.
	defaultMaps["sms"] = ControlMap{"1": b([]int{0}, "z"), "2": b([]int{1}, "x"),
		"Pause": b([]int{9}, "enter"), "Reset": b([]int{}, "r")}
	systemButtons["sms"] = sms.Buttons
}
