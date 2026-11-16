package dolphin

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPadMapRoundTrip(t *testing.T) {
	user := t.TempDir()
	cfg := filepath.Join(user, "Config")
	os.MkdirAll(cfg, 0o755)
	os.WriteFile(filepath.Join(cfg, "GCPadNew.ini"), []byte("[GCPad1]\r\nDevice = XInput/0/Gamepad\r\nButtons/A = `Button A`\r\nMain Stick/Dead Zone = 12.0\r\n"), 0o644)
	os.WriteFile(filepath.Join(cfg, "Dolphin.ini"), []byte("[Core]\r\nSIDevice0 = 6\r\nGFXBackend = Vulkan\r\n"), 0o644)

	gc := ReadPadMap(user, KindGC, 0)
	if gc.Device != XInputPad(0) || gc.Values["Buttons/A"] != "`Button A`" || gc.Plugged != "6" {
		t.Fatalf("read %+v", gc)
	}
	if p2 := ReadPadMap(user, KindGC, 1); p2.Device != "" || p2.Plugged != "0" {
		t.Fatalf("port 2 %+v", p2)
	}
	wm := ReadPadMap(user, KindWiimote, 0)
	if wm.Device != "" || wm.Plugged != "1" || wm.Ext != "None" {
		t.Fatalf("wiimote %+v", wm)
	}

	edit := gc.Clone()
	edit.Values["Buttons/A"] = "SPACE"
	edit.Values["Buttons/B"] = "`Click 0`"
	edit.Plugged = "12"
	if edit.Equal(gc) || gc.Values["Buttons/A"] != "`Button A`" {
		t.Fatal("clone shares its values")
	}
	wm.Device = KeyboardDevice
	wm.Ext = "Classic"
	wm.Defaults()
	if err := SavePadMaps(user, []*PadMap{edit, wm}); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(filepath.Join(cfg, "GCPadNew.ini"))
	if !strings.Contains(string(b), "Main Stick/Dead Zone = 12.0") {
		t.Fatalf("lost a setting the window doesn't show:\n%s", b)
	}
	back := ReadPadMap(user, KindGC, 0)
	if !back.Equal(edit) {
		t.Fatalf("read back %+v want %+v", back, edit)
	}
	if v, _ := ReadINI(ConfigFile(user, "Dolphin.ini")).Get("Core", "GFXBackend"); v != "Vulkan" {
		t.Fatal("Dolphin.ini's other settings changed")
	}
	wb := ReadPadMap(user, KindWiimote, 0)
	if wb.Device != KeyboardDevice || wb.Ext != "Classic" || wb.Values["Classic/Buttons/A"] != "X" || wb.Values["IR/Up"] != "`Cursor Y-`" {
		t.Fatalf("wiimote read back %+v", wb)
	}
	if len(wb.Controls()) != len(WiimoteControls)+len(ClassicControls) {
		t.Fatal("controls for the Classic Controller")
	}
}

func TestPadDefaults(t *testing.T) {
	p := &PadMap{Kind: KindGC, Device: XInputPad(1), Values: map[string]string{"Buttons/A": "Q"}}
	p.Defaults()
	if p.Values["Buttons/A"] != "`Button A`" || p.Values["Triggers/L"] != "`Trigger L`" {
		t.Fatalf("xbox %v", p.Values)
	}
	p.Device = "DInput/0/Some Joystick"
	p.Defaults()
	if p.Values["Buttons/A"] != "" {
		t.Fatal("an unknown device keeps Xbox names")
	}
	w := &PadMap{Kind: KindWiimote, Device: XInputPad(0)}
	w.Defaults()
	if w.Values["Nunchuk/Buttons/Z"] != "`Trigger L`" || w.Values["Classic/Buttons/ZR"] != "`Trigger R`" {
		t.Fatalf("wiimote xbox %v", w.Values)
	}
	for _, c := range append(append([]PadControl{}, GCControls...), w.AllControls()...) {
		if c.Label == "" || strings.Contains(c.Label, "/") {
			t.Errorf("label %q for %s", c.Label, c.Key)
		}
	}
}

func TestDescribeInput(t *testing.T) {
	x := XInputPad(0)
	for _, c := range []struct{ dev, expr, want string }{
		{x, "`Button A`", "A"},
		{x, "Start", "Menu"},
		{x, "`Left Y+`", "Left stick up"},
		{x, "`Motor L`|`Motor R`", "Both motors"},
		{x, "`Button A`|`Button B`", "`Button A`|`Button B`"},
		{x, "`DInput/0/Keyboard Mouse:SPACE`", "Space (Keyboard and mouse)"},
		{KeyboardDevice, "X", "X"},
		{KeyboardDevice, "RETURN", "Enter"},
		{KeyboardDevice, "`Click 1`", "Right mouse button"},
		{KeyboardDevice, "NUMPAD4", "Numpad 4"},
		{KeyboardDevice, "`XInput/1/Gamepad:Button Y`", "Y (Xbox controller 2)"},
		{KeyboardDevice, "", ""},
	} {
		if got := DescribeInput(c.dev, c.expr); got != c.want {
			t.Errorf("DescribeInput(%q, %q) = %q, want %q", c.dev, c.expr, got, c.want)
		}
	}
	if QuoteInput("SPACE") != "SPACE" || QuoteInput("Button A") != "`Button A`" || QuoteInput("Left X+") != "`Left X+`" {
		t.Error("QuoteInput")
	}
	if n, ok := IsXInput("XInput/3/Gamepad"); !ok || n != 3 {
		t.Error("IsXInput")
	}
	if _, ok := IsXInput("XInput/0/Gamepad2"); ok {
		t.Error("IsXInput took a longer name")
	}
}

func TestPadLabels(t *testing.T) {
	want := map[string]string{"Main Stick/Up": "Control stick up", "Buttons/A": "A", "Nunchuk/Buttons/C": "Nunchuk C",
		"Classic/Triggers/L-Analog": "Classic L (analog)", "Tilt/Forward": "Tilt forward", "Classic/Buttons/ZL": "Classic ZL"}
	all := append(append([]PadControl{}, GCControls...), (&PadMap{Kind: KindWiimote}).AllControls()...)
	for _, c := range all {
		if w, ok := want[c.Key]; ok && c.Label != w {
			t.Errorf("%s: %q, want %q", c.Key, c.Label, w)
		}
	}
}
