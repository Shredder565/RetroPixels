package dec8

import "retropixels/cores/arcade"

// The Real Ghostbusters (US, 2 players): the user's file names first,
// MAME's (0.264, ghostb2a) as other names.
var filesGhostb = []arcade.File{
	{Name: "dz-01.rom", Alt: []string{"dz01.1d"}, Size: 0x8000, Role: "cpu", Offset: 0x08000},
	{Name: "dz-02.rom", Alt: []string{"dz02.3d"}, Size: 0x10000, Role: "cpu", Offset: 0x10000},
	{Name: "dz-03.rom", Alt: []string{"dz03.4d"}, Size: 0x10000, Role: "cpu", Offset: 0x20000},
	{Name: "dz-04.rom", Alt: []string{"dz04.6d"}, Size: 0x10000, Role: "cpu", Offset: 0x30000},
	{Name: "dz-05.rom", Alt: []string{"dz05.7d"}, Size: 0x10000, Role: "cpu", Offset: 0x40000},
	{Name: "dz-06.rom", Alt: []string{"dz06.5f"}, Size: 0x8000, Role: "sound", Offset: 0},
	{Name: "dz-00.rom", Alt: []string{"dz00.16b"}, Size: 0x8000, Role: "chars", Offset: 0},
	{Name: "dz-07.rom", Alt: []string{"dz07.12f"}, Size: 0x10000, Role: "tiles", Offset: 0x00000},
	{Name: "dz-08.rom", Alt: []string{"dz08.14f"}, Size: 0x10000, Role: "tiles", Offset: 0x10000},
	{Name: "dz-09.rom", Alt: []string{"dz09.15f"}, Size: 0x10000, Role: "tiles", Offset: 0x20000},
	{Name: "dz-10.rom", Alt: []string{"dz10.17f"}, Size: 0x10000, Role: "tiles", Offset: 0x30000},
	{Name: "dz-15.rom", Alt: []string{"dz15.14f"}, Size: 0x10000, Role: "sprites", Offset: 0x00000},
	{Name: "dz-16.rom", Alt: []string{"dz16.15f"}, Size: 0x10000, Role: "sprites", Offset: 0x10000},
	{Name: "dz-12.rom", Alt: []string{"dz12.9f"}, Size: 0x10000, Role: "sprites", Offset: 0x20000},
	{Name: "dz-14.rom", Alt: []string{"dz14.12f"}, Size: 0x10000, Role: "sprites", Offset: 0x30000},
	{Name: "dz-11.rom", Alt: []string{"dz11.8f"}, Size: 0x10000, Role: "sprites", Offset: 0x40000},
	{Name: "dz-13.rom", Alt: []string{"dz13.1f"}, Size: 0x10000, Role: "sprites", Offset: 0x50000},
	{Name: "dz-17.rom", Alt: []string{"dz17.17f"}, Size: 0x10000, Role: "sprites", Offset: 0x60000},
	{Name: "dz-18.rom", Alt: []string{"dz18.18f"}, Size: 0x10000, Role: "sprites", Offset: 0x70000},
	{Name: "dz19a.10d", Size: 0x400, Role: "proms", Offset: 0x000},
	{Name: "dz20a.11d", Size: 0x400, Role: "proms", Offset: 0x400},
	// the i8751 microcontroller (coins and protection): the board answers
	// for it, so the dump isn't needed
	{Name: "dz-1.1b", Size: 0x1000, Role: "mcu", Offset: 0, Optional: true},
}
