package main

import "testing"

func TestDolPadPressed(t *testing.T) {
	var before, now PadState
	now.Connected = true
	if dolPadPressed(before, now) != "" {
		t.Fatal("nothing pressed")
	}
	now.Buttons[padRT] = true
	if got := dolPadPressed(before, now); got != "Trigger R" {
		t.Fatalf("RT = %q", got)
	}
	before = now
	if dolPadPressed(before, now) != "" {
		t.Fatal("held, not newly pressed")
	}
	now.Axes[1] = -0.9 // left stick up (down is +)
	if got := dolPadPressed(before, now); got != "Left Y+" {
		t.Fatalf("stick up = %q", got)
	}
	before, now = PadState{Connected: true}, PadState{Connected: true}
	now.Axes[2] = 0.9
	if got := dolPadPressed(before, now); got != "Right X+" {
		t.Fatalf("right stick right = %q", got)
	}
	for b := 0; b < padButtons; b++ {
		if dolPadInputs[b] == "" {
			t.Errorf("pad button %d has no name", b)
		}
	}
}

func TestDolKeyInput(t *testing.T) {
	for _, c := range []struct {
		vk   int
		lp   uintptr
		want string
	}{
		{'X', 0, "X"}, {'7', 0, "7"}, {0x20, 0, "SPACE"}, {0x0D, 0, "RETURN"}, {0x0D, 1 << 24, "NUMPADENTER"},
		{0x10, 0x2A << 16, "LSHIFT"}, {0x10, 0x36 << 16, "RSHIFT"}, {0x11, 0, "LCONTROL"}, {0x11, 1 << 24, "RCONTROL"},
		{0x26, 0, "UP"}, {0x64, 0, "NUMPAD4"}, {0x70, 0, "F1"}, {0x7B, 0, "F12"}, {0xBA, 0, "SEMICOLON"}, {0xFF, 0, ""},
	} {
		if got := dolKeyInput(c.vk, c.lp); got != c.want {
			t.Errorf("dolKeyInput(%#x, %#x) = %q, want %q", c.vk, c.lp, got, c.want)
		}
	}
}
