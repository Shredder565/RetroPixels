package mame

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSeatConfigSwapsWithPlayer1(t *testing.T) {
	c := SeatConfig(3)
	for _, want := range []string{
		`<port type="P3_JOYSTICK_UP">`, "KEYCODE_UP OR JOYCODE_1_YAXIS_UP_SWITCH OR JOYCODE_1_HAT1UP",
		`<port type="P3_BUTTON1">`, "KEYCODE_LCONTROL OR JOYCODE_1_BUTTON1",
		`<port type="START3">`, "KEYCODE_1 OR JOYCODE_1_START",
		`<port type="COIN3">`, "KEYCODE_5 OR JOYCODE_1_SELECT",
		`<port type="P1_JOYSTICK_LEFT">`, "KEYCODE_J OR JOYCODE_3_XAXIS_LEFT_SWITCH",
		`<port type="START1">`, "KEYCODE_3 OR JOYCODE_3_START",
		`<port type="P1_BUTTON4">`, ">JOYCODE_3_BUTTON4<",
	} {
		if !strings.Contains(c, want) {
			t.Errorf("missing %q in\n%s", want, c)
		}
	}
	if strings.Contains(c, "P2_") || strings.Contains(c, "P4_") {
		t.Error("other players changed")
	}
}

func TestSeatArgs(t *testing.T) {
	dir := t.TempDir()
	if a := SeatArgs(1, dir, nil); a != nil {
		t.Errorf("player 1: %v", a)
	}
	if a := SeatArgs(2, dir, Options{"rompath": true}); a != nil {
		t.Errorf("a MAME without -ctrlr: %v", a)
	}
	a := SeatArgs(2, dir, nil)
	if len(a) != 4 || a[0] != "-ctrlrpath" || a[1] != dir || a[3] != "retropixels-player2" {
		t.Fatalf("%v", a)
	}
	if b, err := os.ReadFile(filepath.Join(dir, "retropixels-player2.cfg")); err != nil || !strings.Contains(string(b), "P2_JOYSTICK_UP") {
		t.Fatalf("%v %s", err, b)
	}
}

func TestSeatOf(t *testing.T) {
	var c Config
	if c.SeatOf("tmnt") != 1 {
		t.Error("default")
	}
	c.SetSeat("", 2)
	c.SetSeat("TMNT", 4)
	if c.SeatOf("tmnt") != 4 || c.SeatOf("simpsons") != 2 {
		t.Error(c.SeatOf("tmnt"), c.SeatOf("simpsons"))
	}
	c.SetSeat("", 9)
	if c.Seat != 1 {
		t.Error(c.Seat)
	}
}
