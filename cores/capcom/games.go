package capcom

import (
	"fmt"
	"strconv"

	"retropixels/cores/arcade"
)

// The sets: the user's file names first, MAME's (0.264) as other names.

var files1942 = []arcade.File{
	{Name: "1-n3a.bin", Alt: []string{"sra-03.m3"}, Size: 0x4000, Role: "cpu", Offset: 0x00000},
	{Name: "1-n4.bin", Alt: []string{"sr-04.m4"}, Size: 0x4000, Role: "cpu", Offset: 0x04000},
	{Name: "1-n5.bin", Alt: []string{"sr-05.m5"}, Size: 0x4000, Role: "cpu", Offset: 0x10000},
	{Name: "1-n6.bin", Alt: []string{"sr-06.m6"}, Size: 0x2000, Role: "cpu", Offset: 0x14000},
	{Name: "1-n7.bin", Alt: []string{"sr-07.m7"}, Size: 0x4000, Role: "cpu", Offset: 0x18000},
	{Name: "1-c11.bin", Alt: []string{"sr-01.c11"}, Size: 0x4000, Role: "sound", Offset: 0},
	{Name: "1-f2.bin", Alt: []string{"sr-02.f2"}, Size: 0x2000, Role: "chars", Offset: 0},
	{Name: "2-a1.bin", Alt: []string{"sr-08.a1"}, Size: 0x2000, Role: "tiles", Offset: 0x0000},
	{Name: "2-a2.bin", Alt: []string{"sr-09.a2"}, Size: 0x2000, Role: "tiles", Offset: 0x2000},
	{Name: "2-a3.bin", Alt: []string{"sr-10.a3"}, Size: 0x2000, Role: "tiles", Offset: 0x4000},
	{Name: "2-a4.bin", Alt: []string{"sr-11.a4"}, Size: 0x2000, Role: "tiles", Offset: 0x6000},
	{Name: "2-a5.bin", Alt: []string{"sr-12.a5"}, Size: 0x2000, Role: "tiles", Offset: 0x8000},
	{Name: "2-a6.bin", Alt: []string{"sr-13.a6"}, Size: 0x2000, Role: "tiles", Offset: 0xa000},
	{Name: "2-l1.bin", Alt: []string{"sr-14.l1"}, Size: 0x4000, Role: "sprites", Offset: 0x0000},
	{Name: "2-l2.bin", Alt: []string{"sr-15.l2"}, Size: 0x4000, Role: "sprites", Offset: 0x4000},
	{Name: "2-n1.bin", Alt: []string{"sr-16.n1"}, Size: 0x4000, Role: "sprites", Offset: 0x8000},
	{Name: "2-n2.bin", Alt: []string{"sr-17.n2"}, Size: 0x4000, Role: "sprites", Offset: 0xc000},
	{Name: "08e_sb-5.bin", Alt: []string{"sb-5.e8"}, Size: 256, Role: "red", Offset: 0},
	{Name: "09e_sb-6.bin", Alt: []string{"sb-6.e9"}, Size: 256, Role: "green", Offset: 0},
	{Name: "10e_sb-7.bin", Alt: []string{"sb-7.e10"}, Size: 256, Role: "blue", Offset: 0},
	{Name: "f01_sb-0.bin", Alt: []string{"sb-0.f1"}, Size: 256, Role: "charlut", Offset: 0},
	{Name: "06d_sb-4.bin", Alt: []string{"sb-4.d6"}, Size: 256, Role: "tilelut", Offset: 0},
	{Name: "03k_sb-8.bin", Alt: []string{"sb-8.k3"}, Size: 256, Role: "spritelut", Offset: 0},
	// video timing PROMs: the board's timing is built in
	{Name: "01d_sb-2.bin", Alt: []string{"sb-2.d1"}, Size: 256, Role: "timing", Offset: arcade.Next, Optional: true},
	{Name: "02d_sb-3.bin", Alt: []string{"sb-3.d2"}, Size: 256, Role: "timing", Offset: arcade.Next, Optional: true},
	{Name: "k06_sb-1.bin", Alt: []string{"sb-1.k6"}, Size: 256, Role: "timing", Offset: arcade.Next, Optional: true},
	{Name: "01m_sb-9.bin", Alt: []string{"sb-9.m11"}, Size: 256, Role: "timing", Offset: arcade.Next, Optional: true},
}

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

var offOn = []arcade.DipOption{opt("Off", 1), opt("On", 0)} // switches that are on when 0

var coins1942 = []arcade.DipOption{opt("1 coin, 1 credit", 7), opt("1 coin, 2 credits", 6), opt("1 coin, 4 credits", 5),
	opt("2 coins, 1 credit", 4), opt("2 coins, 3 credits", 3), opt("3 coins, 1 credit", 2), opt("4 coins, 1 credit", 1), opt("Free play", 0)}

// 1942's switches (the lives table is at 7880 in the program: 5, 2, 1, 3).
var dips1942 = []arcade.Dip{
	{ID: "coina", Label: "Coin A", Default: 7, Options: coins1942},
	{ID: "coinb", Label: "Coin B", Default: 7, Options: coins1942},
	{ID: "lives", Label: "Lives", Default: 1, Options: []arcade.DipOption{opt("1", 2), opt("2", 1), opt("3", 3), opt("5", 0)}},
	{ID: "bonus", Label: "Extra planes at", Default: 3, Options: []arcade.DipOption{opt("20,000 / 80,000 / every 80,000", 3),
		opt("20,000 / 100,000 / every 100,000", 2), opt("30,000 / 80,000 / every 80,000", 1), opt("30,000 / 100,000 / every 100,000", 0)}},
	{ID: "difficulty", Label: "Difficulty", Default: 3, Options: []arcade.DipOption{opt("Easy", 2), opt("Normal", 3), opt("Difficult", 1), opt("Very difficult", 0)}},
	{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []arcade.DipOption{opt("Upright", 0), opt("Cocktail table", 1)}},
	{ID: "flip", Label: "Flip screen", Default: 1, Options: offOn},
	{ID: "stop", Label: "Screen stop (freeze)", Default: 1, Options: offOn},
	{ID: "service", Label: "Service mode (test screen)", Default: 1, Options: offOn},
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "1942", Title: "Capcom 1942", Width: scrW, Height: scrH, Rotate: 270, FPS: fps,
		Buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips:    dips1942,
		Games: []arcade.Game{
			{ID: "1942", Title: "1942 (Revision A)", Year: 1984, Maker: "Capcom", Files: files1942},
		},
		New: new1942,
	})
	arcade.Register(&arcade.BoardDef{
		ID: "1943", Title: "Capcom 1943", Width: scrW, Height: scrH, Rotate: 270, FPS: fps,
		Buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Coin 2"},
		Dips:    dips1943,
		Games: []arcade.Game{
			{ID: "1943", Title: "1943: The Battle of Midway (US, Rev C)", Year: 1987, Maker: "Capcom", Files: files1943},
			{ID: "1943kai", Title: "1943 Kai: Midway Kaisen", Year: 1987, Maker: "Capcom", Parent: "1943", Files: files1943kai},
		},
		New: new1943,
	})
}

// set43 lists a 1943 set: chips 01-25 (the user's name is prefix.NN, MAME's
// is mame[NN]) and the twelve PROMs (user's name, MAME's name), in the order
// red, green, blue, characters, front low / high, back low / high, sprites
// low / high, and the two timing PROMs.
func set43(prefix string, mame map[int]string, proms [12][2]string) []arcade.File {
	f := func(n, size int, role string, off int) arcade.File {
		return arcade.File{Name: fmt.Sprintf("%s.%02d", prefix, n), Alt: []string{mame[n]}, Size: size, Role: role, Offset: off}
	}
	l := []arcade.File{f(1, 0x8000, "cpu", 0), f(2, 0x10000, "cpu", 0x10000), f(3, 0x10000, "cpu", 0x20000),
		f(5, 0x8000, "sound", 0), f(4, 0x8000, "chars", 0), f(14, 0x8000, "frontmap", 0), f(23, 0x8000, "backmap", 0)}
	for k := 0; k < 8; k++ {
		l = append(l, f(15+k, 0x8000, "front", k*0x8000), f(6+k, 0x8000, "sprites", k*0x8000))
	}
	l = append(l, f(24, 0x8000, "back", 0), f(25, 0x8000, "back", 0x8000))
	roles := []string{"red", "green", "blue", "charlut", "frontlut", "frontluth", "backlut", "backluth", "spritelut", "spriteluth", "timing", "timing"}
	for i, p := range proms {
		l = append(l, arcade.File{Name: p[0], Alt: []string{p[1]}, Size: 256, Role: roles[i], Offset: 0, Optional: roles[i] == "timing"})
	}
	// the protection MCU (the user's sets don't have it): the board answers
	// the game's questions itself
	return append(l, arcade.File{Name: "bm.7k", Size: 0x1000, Role: "mcu", Offset: 0, Optional: true})
}

var files1943 = set43("1943", map[int]string{1: "bmu01c.12d", 2: "bmu02c.13d", 3: "bmu03c.14d", 4: "bm04.5h", 5: "bm05.4k",
	6: "bm06.10a", 7: "bm07.11a", 8: "bm08.12a", 9: "bm09.14a", 10: "bm10.10c", 11: "bm11.11c", 12: "bm12.12c", 13: "bm13.14c",
	14: "bm14.5f", 15: "bm15.10f", 16: "bm16.11f", 17: "bm17.12f", 18: "bm18.14f", 19: "bm19.10j", 20: "bm20.11j", 21: "bm21.12j",
	22: "bm22.14j", 23: "bm23.8k", 24: "bm24.14k", 25: "bm25.14l"},
	[12][2]string{{"bmprom.01", "bm1.12a"}, {"bmprom.02", "bm2.13a"}, {"bmprom.03", "bm3.14a"}, {"bmprom.05", "bm5.7f"},
		{"bmprom.10", "bm10.7l"}, {"bmprom.09", "bm9.6l"}, {"bmprom.12", "bm12.12m"}, {"bmprom.11", "bm11.12l"},
		{"bmprom.08", "bm8.8c"}, {"bmprom.07", "bm7.7c"}, {"bmprom.04", "bm4.12c"}, {"bmprom.06", "bm6.4b"}})

var files1943kai = set43("1943kai", map[int]string{1: "bmk01.12d", 2: "bmk02.13d", 3: "bmk03.14d", 4: "bmk04.5h", 5: "bmk05.4k",
	6: "bmk06.10a", 7: "bmk07.11a", 8: "bmk08.12a", 9: "bmk09.14a", 10: "bmk10.10c", 11: "bmk11.11c", 12: "bmk12.12c", 13: "bmk13.14c",
	14: "bmk14.5f", 15: "bm15.10f", 16: "bmk16.11f", 17: "bmk17.12f", 18: "bmk18.14f", 19: "bm19.10j", 20: "bmk20.11j", 21: "bmk21.12j",
	22: "bmk22.14j", 23: "bmk23.8k", 24: "bmk24.14k", 25: "bmk25.14l"},
	[12][2]string{{"bmk01.bin", "bmk1.12a"}, {"bmk02.bin", "bmk2.13a"}, {"bmk03.bin", "bmk3.14a"}, {"bmk05.bin", "bmk5.7f"},
		{"bmk10.bin", "bmk10.7l"}, {"bmk09.bin", "bmk9.6l"}, {"bmk12.bin", "bmk12.12m"}, {"bmk11.bin", "bmk11.12l"},
		{"bmk08.bin", "bmk8.8c"}, {"bmk07.bin", "bmk7.7c"}, {"bmprom.04", "bm4.12c"}, {"bmprom.06", "bm6.4b"}})

var coins1943 = []arcade.DipOption{opt("1 coin, 1 credit", 7), opt("1 coin, 2 credits", 6), opt("1 coin, 3 credits", 5),
	opt("1 coin, 4 credits", 4), opt("1 coin, 5 credits", 3), opt("2 coins, 1 credit", 2), opt("3 coins, 1 credit", 1), opt("4 coins, 1 credit", 0)}

func difficulty1943() []arcade.DipOption {
	var o []arcade.DipOption
	for v := 15; v >= 0; v-- {
		l := strconv.Itoa(16 - v)
		switch v {
		case 15:
			l += " (easiest)"
		case 8:
			l += " (normal)"
		case 0:
			l += " (hardest)"
		}
		o = append(o, opt(l, v))
	}
	return o
}

var dips1943 = []arcade.Dip{
	{ID: "coina", Label: "Coin A", Default: 7, Options: coins1943},
	{ID: "coinb", Label: "Coin B", Default: 7, Options: coins1943},
	{ID: "difficulty", Label: "Difficulty", Default: 8, Options: difficulty1943()},
	{ID: "twoplayer", Label: "2-player game", Default: 1, Options: []arcade.DipOption{opt("2 credits", 1), opt("1 credit", 0)}},
	{ID: "continue", Label: "Allow continue", Default: 1, Options: []arcade.DipOption{opt("Yes", 1), opt("No", 0)}},
	{ID: "demosound", Label: "Demo sounds", Default: 1, Options: []arcade.DipOption{opt("On", 1), opt("Off", 0)}},
	{ID: "flip", Label: "Flip screen", Default: 1, Options: offOn},
	{ID: "stop", Label: "Screen stop (freeze)", Default: 1, Options: offOn},
	{ID: "service", Label: "Service mode (test screen)", Default: 1, Options: offOn},
}
