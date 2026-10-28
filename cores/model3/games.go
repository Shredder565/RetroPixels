package model3

// The Model 3 games RetroPixels plays.

func files(names []string, crcs []uint32, offsets []int, size int) []RegionFile {
	out := make([]RegionFile, len(names))
	for i := range names {
		out[i] = RegionFile{Name: names[i], CRC: crcs[i], Offset: offsets[i], Size: size}
	}
	return out
}

func swtRegions(crom []RegionFile) map[string]*Region {
	return map[string]*Region{
		"crom": {Stride: 8, Chunk: 2, Swap: true, Files: crom},
		"banked_crom": {Stride: 8, Chunk: 2, Swap: true, Files: files(
			[]string{"mpr-21342.04", "mpr-21341.03", "mpr-21340.02", "mpr-21339.01",
				"mpr-21346.08", "mpr-21345.07", "mpr-21344.06", "mpr-21343.05",
				"mpr-21350.12", "mpr-21349.11", "mpr-21348.10", "mpr-21347.09"},
			[]uint32{0x339525CE, 0xB2A269E4, 0xAD36040E, 0xC0CE5037,
				0xC8733594, 0x6C183A21, 0x87453D76, 0x12552D07,
				0x486195E7, 0x3D39454B, 0x1F7CC5F5, 0xECB6B934},
			[]int{0, 2, 4, 6, 0x1000000, 0x1000002, 0x1000004, 0x1000006, 0x2000000, 0x2000002, 0x2000004, 0x2000006},
			0x400000)},
		"vrom": {Stride: 32, Chunk: 2, Files: files(
			[]string{"mpr-21359.26", "mpr-21360.27", "mpr-21361.28", "mpr-21362.29", "mpr-21363.30", "mpr-21364.31",
				"mpr-21365.32", "mpr-21366.33", "mpr-21367.34", "mpr-21368.35", "mpr-21369.36", "mpr-21370.37",
				"mpr-21371.38", "mpr-21372.39", "mpr-21373.40", "mpr-21374.41"},
			[]uint32{0x34EF4122, 0x2882B95E, 0x9B61C3C1, 0x01A92169, 0xE7D18FED, 0xCB6A5468,
				0xAD5449D8, 0xDEFB6B95, 0xDFD51029, 0xAE90FD21, 0xBF17EEB4, 0x2321592A,
				0xA68782FD, 0xFC3F4E8B, 0xB76AD261, 0xAE6C4D28},
			[]int{0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30}, 0x400000)},
		"sound_program": {Stride: 2, Chunk: 2, Swap: true, Files: []RegionFile{{"epr-21383.21", 0x544D1E28, 0, 0x80000}}},
		"sound_samples": {Stride: 2, Chunk: 2, Swap: true, Files: []RegionFile{
			{"mpr-21355.22", 0xC1B2D326, 0, 0x400000}, {"mpr-21357.24", 0x02703FAB, 0x400000, 0x400000}}},
		"mpeg_program": {Stride: 2, Chunk: 2, Swap: true, Files: []RegionFile{{"epr-21384.2", 0x12FA4780, 0, 0x20000}}},
		"mpeg_music": {Stride: 1, Chunk: 1, Files: []RegionFile{
			{"mpr-21375.18", 0x735157A9, 0, 0x400000}, {"mpr-21376.20", 0xE635F81E, 0x400000, 0x400000},
			{"mpr-21377.22", 0x720621F8, 0x800000, 0x400000}, {"mpr-21378.24", 0x1FCF715E, 0xC00000, 0x400000}}},
		"driveboard_program": {Stride: 1, Chunk: 1, Files: []RegionFile{{"epr-21119.ic8", 0x65082B14, 0, 0x10000}}},
	}
}

// Games are the Model 3 games RetroPixels knows.
var Games = []*GameDef{
	{
		ID: "swtrilgy", Title: "Star Wars Trilogy Arcade", Year: 1998, Maker: "Sega / LucasArts",
		Step: 0x21, Inputs: InputAnalogJoystick, DSB: "DSB2", Drive: "joystick", Key: 0x31272A01,
		Regions: swtRegions(files(
			[]string{"epr-21382a.20", "epr-21381a.19", "epr-21380a.18", "epr-21379a.17"},
			[]uint32{0x69BAF117, 0x2DD34E28, 0x780FB4E7, 0x24DC1555},
			[]int{0, 2, 4, 6}, 0x200000)),
	},
	{
		ID: "swtrilgya", Parent: "swtrilgy", Title: "Star Wars Trilogy Arcade", Year: 1998, Maker: "Sega / LucasArts",
		Step: 0x21, Inputs: InputAnalogJoystick, DSB: "DSB2", Drive: "joystick", Key: 0x31272A01,
		Regions: map[string]*Region{
			"crom": {Stride: 8, Chunk: 2, Swap: true, Files: files(
				[]string{"epr-21382.20", "epr-21381.19", "epr-21380.18", "epr-21379.17"},
				[]uint32{0x0B9C44A0, 0xBB5757BF, 0x49B182F2, 0x61AD51D9},
				[]int{0, 2, 4, 6}, 0x200000)},
		},
	},
}
