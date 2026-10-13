package main

import "testing"

func TestPickDolphinRelease(t *testing.T) {
	page := `<a href="https://dl.dolphin-emu.org/releases/2503/dolphin-2503-x64.7z">old</a>
<a href="https://dl.dolphin-emu.org/releases/2506/dolphin-2506-x64.7z">new</a>
<a href="https://dl.dolphin-emu.org/releases/2506/dolphin-2506-universal.dmg">mac</a>
<a href="https://dl.dolphin-emu.org/releases/2506/dolphin-2505-x64.7z">odd</a>`
	link, ver, err := pickDolphinRelease(page)
	if err != nil || ver != "2506" || link != "https://dl.dolphin-emu.org/releases/2506/dolphin-2506-x64.7z" {
		t.Fatalf("got %q %q %v", link, ver, err)
	}
	if _, _, err := pickDolphinRelease("nothing here"); err == nil {
		t.Fatal("expected an error")
	}
}
