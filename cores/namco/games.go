package namco

import "retropixels/cores/arcade"

// dip is one switch setting with where it sits: bank 0 is MAME's DSWA, 1 is
// DSWB; mask 0 is a setting that isn't on the switch banks.
type dip struct {
	arcade.Dip
	bank int
	mask uint8
}

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

func sw(id, label string, bank int, mask uint8, def int, opts ...arcade.DipOption) dip {
	return dip{arcade.Dip{ID: id, Label: label, Options: opts, Default: def}, bank, mask}
}

var serviceDip = sw("service", "Service mode (test screen)", 0, 0, 0, opt("Off", 0), opt("On", 1))

var galagaDips = []dip{
	sw("coinage", "Coinage", 1, 0x07, 0x07,
		opt("1 coin, 1 credit", 0x07), opt("1 coin, 2 credits", 0x03), opt("1 coin, 3 credits", 0x05),
		opt("2 coins, 1 credit", 0x06), opt("2 coins, 3 credits", 0x01), opt("3 coins, 1 credit", 0x02),
		opt("4 coins, 1 credit", 0x04), opt("Free play", 0x00)),
	sw("lives", "Lives", 1, 0xc0, 0x80, opt("2", 0x00), opt("3", 0x80), opt("4", 0x40), opt("5", 0xc0)),
	sw("bonus", "Bonus fighter (with 2-4 lives)", 1, 0x38, 0x10,
		opt("20,000, 70,000, every 70,000", 0x10), opt("20,000, 60,000, every 60,000", 0x20),
		opt("20,000, 80,000, every 80,000", 0x30), opt("30,000, 100,000, every 100,000", 0x08),
		opt("30,000, 120,000, every 120,000", 0x28), opt("20,000 and 60,000", 0x18),
		opt("30,000 and 80,000", 0x38), opt("None", 0x00)),
	sw("difficulty", "Difficulty", 0, 0x03, 0x03, opt("Easy", 0x03), opt("Medium", 0x00), opt("Hard", 0x01), opt("Hardest", 0x02)),
	sw("demoSounds", "Demo sounds", 0, 0x08, 0x00, opt("On", 0x00), opt("Off", 0x08)),
	sw("cabinet", "Cabinet", 0, 0x80, 0x80, opt("Upright", 0x80), opt("Cocktail", 0x00)),
	sw("freeze", "Freeze", 0, 0x10, 0x10, opt("Off", 0x10), opt("On", 0x00)),
	sw("rackTest", "Rack test", 0, 0x20, 0x20, opt("Off", 0x20), opt("On", 0x00)),
	serviceDip,
}

var digdugDips = []dip{
	sw("coinA", "Coin slot 1", 1, 0xc0, 0x00,
		opt("1 coin, 1 credit", 0x00), opt("1 coin, 2 credits", 0x80), opt("2 coins, 1 credit", 0x40), opt("2 coins, 3 credits", 0xc0)),
	sw("coinB", "Coin slot 2", 0, 0x07, 0x01,
		opt("1 coin, 1 credit", 0x01), opt("1 coin, 2 credits", 0x06), opt("1 coin, 3 credits", 0x02),
		opt("1 coin, 6 credits", 0x04), opt("1 coin, 7 credits", 0x00), opt("2 coins, 1 credit", 0x03),
		opt("2 coins, 3 credits", 0x05), opt("3 coins, 1 credit", 0x07)),
	sw("lives", "Lives", 0, 0xc0, 0x80, opt("1", 0x00), opt("2", 0x40), opt("3", 0x80), opt("5", 0xc0)),
	sw("bonus", "Bonus life (with 1-3 lives)", 0, 0x38, 0x18,
		opt("10,000, 40,000, every 40,000", 0x20), opt("10,000, 50,000, every 50,000", 0x10),
		opt("20,000, 60,000, every 60,000", 0x30), opt("20,000, 70,000, every 70,000", 0x08),
		opt("10,000 and 40,000", 0x28), opt("20,000 and 60,000", 0x18), opt("10,000", 0x38), opt("None", 0x00)),
	sw("difficulty", "Difficulty", 1, 0x03, 0x00, opt("Easy", 0x00), opt("Medium", 0x02), opt("Hard", 0x01), opt("Hardest", 0x03)),
	sw("continue", "Allow continue", 1, 0x08, 0x00, opt("Yes", 0x00), opt("No", 0x08)),
	sw("demoSounds", "Demo sounds", 1, 0x10, 0x00, opt("On", 0x00), opt("Off", 0x10)),
	sw("cabinet", "Cabinet", 1, 0x04, 0x04, opt("Upright", 0x04), opt("Cocktail", 0x00)),
	sw("freeze", "Freeze", 1, 0x20, 0x20, opt("Off", 0x20), opt("On", 0x00)),
	serviceDip,
}

func dipsFor(k kind) []dip {
	if k == digdug {
		return digdugDips
	}
	return galagaDips
}

func plain(ds []dip) []arcade.Dip {
	out := make([]arcade.Dip, len(ds))
	for i, d := range ds {
		out[i] = d.Dip
	}
	return out
}

// f is one file: the user's name first, then MAME's names.
func f(name string, size int, role string, at int, alt ...string) arcade.File {
	return arcade.File{Name: name, Alt: alt, Size: size, Role: role, Offset: at}
}

func optional(x arcade.File) arcade.File { x.Optional = true; return x }

const k4 = 0x1000

// Galaga's graphics and PROMs (the same in every Namco set).
func galagaCommon(chars, spr0, spr1 arcade.File) []arcade.File {
	return []arcade.File{
		chars, spr0, spr1,
		f("5n.bin", 32, "palette", 0, "prom-5.5n"),
		f("2n.bin", 256, "chrlut", 0, "prom-4.2n"),
		f("1c.bin", 256, "sprlut", 0, "prom-3.1c"),
		optional(f("1d.bin", 256, "wave", 0, "prom-1.1d")),
		optional(f("5c.bin", 256, "timing", 0, "prom-2.5c")), // not used
		optional(f("51xx.bin", 1024, "mcu", 0)),              // simulated (custom.go)
		optional(f("54xx.bin", 1024, "mcu", 0x400)),
	}
}

func digdugCommon(cpu3, chars arcade.File, bg ...arcade.File) []arcade.File {
	out := []arcade.File{cpu3, chars}
	out = append(out, bg...)
	return append(out,
		f("digdug.5n", 32, "palette", 0, "136007.113"),
		f("digdug.2n", 256, "chrlut", 0, "136007.112"),
		f("digdug.1c", 256, "sprlut", 0, "136007.111"),
		optional(f("digdug.spr", 256, "wave", 0, "136007.110")),
		optional(f("136007.109", 256, "timing", 0)), // not used
		optional(f("51xx.bin", 1024, "mcu", 0)),
		optional(f("53xx.bin", 1024, "mcu", 0x400)),
	)
}

func list(a []arcade.File, b ...arcade.File) []arcade.File {
	return append(append([]arcade.File(nil), b...), a...)
}

func init() {
	galagaGfx := galagaCommon(f("07m_g08.bin", k4, "chars", 0, "gg1-9.4l", "gg1_9.4l", "2600j.bin"),
		f("07e_g10.bin", k4, "sprites", 0, "gg1-11.4d", "gg1_11.4d", "2800l.bin"),
		f("07h_g09.bin", k4, "sprites", k4, "gg1-10.4f", "gg1_10.4f", "2700k.bin"))
	arcade.Register(&arcade.BoardDef{
		ID: "galaga", Title: "Namco Galaga", Width: scrW, Height: scrH, Rotate: 90, FPS: FPS,
		Buttons: []string{"Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips:    plain(galagaDips),
		Games: []arcade.Game{
			{ID: "galaga", Title: "Galaga", Year: 1981, Maker: "Namco", Files: list(galagaGfx,
				f("04m_g01.bin", k4, "cpu1", 0, "gg1-1.3p"), f("04k_g02.bin", k4, "cpu1", k4, "gg1-2.3m"),
				f("04j_g03.bin", k4, "cpu1", 2*k4, "gg1-3.2m", "gg1_3.2m"), f("04h_g04.bin", k4, "cpu1", 3*k4, "gg1-4.2l"),
				f("04e_g05.bin", k4, "cpu2", 0, "gg1-5.3f"), f("04d_g06.bin", k4, "cpu3", 0, "gg1-7.2c"))},
			// MAME's galaga: the later revision B program (not tried: the user's set is the earlier one)
			{ID: "galaga", Title: "Galaga (rev. B)", Year: 1981, Maker: "Namco", Files: list(galagaGfx,
				f("gg1_1b.3p", k4, "cpu1", 0), f("gg1_2b.3m", k4, "cpu1", k4),
				f("gg1_3.2m", k4, "cpu1", 2*k4, "gg1-3.2m", "04j_g03.bin"), f("gg1_4b.2l", k4, "cpu1", 3*k4),
				f("gg1_5b.3f", k4, "cpu2", 0), f("gg1_7b.2c", k4, "cpu3", 0))},
			{ID: "galagamw", Title: "Galaga (Midway)", Year: 1981, Maker: "Namco (Midway license)", Parent: "galaga", Files: list(galagaGfx,
				f("3200a.bin", k4, "cpu1", 0), f("3300b.bin", k4, "cpu1", k4),
				f("3400c.bin", k4, "cpu1", 2*k4), f("3500d.bin", k4, "cpu1", 3*k4),
				f("3600e.bin", k4, "cpu2", 0), f("3700g.bin", k4, "cpu3", 0))},
		},
		New: newBoard(galaga),
	})

	ddGfx := func(chars arcade.File) []arcade.File {
		return digdugCommon(f("136007.107", k4, "cpu3", 0, "dd1.7"), chars,
			f("136007.116", k4, "sprites", 0, "dd1.15"), f("dd1.14", k4, "sprites", k4),
			f("136007.118", k4, "sprites", 2*k4, "dd1.13"), f("136007.119", k4, "sprites", 3*k4, "dd1.12"),
			f("dd1.11", k4, "bgtiles", 0), f("dd1.10b", k4, "bgmap", 0))
	}
	arcade.Register(&arcade.BoardDef{
		ID: "digdug", Title: "Namco Dig Dug", Width: scrW, Height: scrH, Rotate: 90, FPS: FPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips:    plain(digdugDips),
		Games: []arcade.Game{
			{ID: "digdug", Title: "Dig Dug", Year: 1982, Maker: "Namco", Files: list(ddGfx(f("dd1.9", 0x800, "chars", 0)),
				f("136007.101", k4, "cpu1", 0, "dd1.1"), f("136007.102", k4, "cpu1", k4, "dd1.2"),
				f("136007.103", k4, "cpu1", 2*k4, "dd1.3"), f("dd1.4b", k4, "cpu1", 3*k4),
				f("dd1.5b", k4, "cpu2", 0), f("dd1.6b", k4, "cpu2", k4))},
			// MAME's digdug: revision 2 (not tried: the user's set is revision 1)
			{ID: "digdug", Title: "Dig Dug (rev 2)", Year: 1982, Maker: "Namco", Files: list(ddGfx(f("dd1.9", 0x800, "chars", 0)),
				f("dd1a.1", k4, "cpu1", 0), f("dd1a.2", k4, "cpu1", k4),
				f("dd1a.3", k4, "cpu1", 2*k4), f("dd1a.4", k4, "cpu1", 3*k4),
				f("dd1a.5", k4, "cpu2", 0), f("dd1a.6", k4, "cpu2", k4))},
		},
		New: newBoard(digdug),
	})
}
