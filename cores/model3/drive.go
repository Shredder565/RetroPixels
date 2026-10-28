package model3

import (
	"fmt"
	"os"
)

var debugDrive = os.Getenv("M3_DEBUG_DRIVE") != ""

// The force feedback board of the joystick (Star Wars Trilogy).
type driveBoard struct {
	kind string
	m    *Machine

	phase   int
	timer   int
	reply   uint8
	last    uint8
	lastCmd uint8
}

func newDriveBoard(g *GameDef) *driveBoard { return &driveBoard{kind: g.Drive} }

func (d *driveBoard) reset() {
	d.phase, d.timer, d.reply = 0, 20, 0xCF
}
func (d *driveBoard) attached() bool { return d.kind != "" }
func (d *driveBoard) read() uint8 {
	v := d.reply
	if debugDrive && v != d.last {
		fmt.Printf("frame %d drive read %02X\n", d.m.frame, v)
	}
	d.last = v
	return v
}

func (d *driveBoard) write(v uint8) {
	if debugDrive && v != d.lastCmd {
		fmt.Printf("frame %d drive write %02X\n", d.m.frame, v)
	}
	d.lastCmd = v
	switch d.phase {
	case 1: // waiting for the game's hello
		if v == 0x80 {
			d.phase, d.reply = 2, 0xCD
		}
	case 2:
		if v == 0xFF {
			d.phase, d.timer = 3, 30
		}
	case 4:
		if v == 0xC0 {
			d.phase = 5
		}
	case 5:
		d.reply = v
	}
}

func (d *driveBoard) runFrame() {
	switch d.phase {
	case 0:
		d.timer--
		if d.timer <= 0 {
			d.phase, d.reply = 1, 0xCE
		}
	case 3:
		d.timer--
		if d.timer <= 0 {
			d.phase, d.reply = 4, 0xCB
		}
	}
}
