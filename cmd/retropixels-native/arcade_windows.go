package main

import (
	"fmt"
	"strings"

	"retropixels/cores/arcade"
)

// The Game menu's "Machine settings (DIP switches)": the switches on an
// arcade game's circuit board (coins, lives...), kept per game in
// settings.json, as the web page's Machine settings box.

const (
	cmdDip0      = 600 // + dip*16 + option
	cmdDipEnd    = cmdDip0 + 32*16
	dipMenuLabel = "&Machine settings (DIP switches)"
)

// dipCore is a core with machine settings (the arcade core).
type dipCore interface {
	Dips() []arcade.DipSetting
	SetDip(id string, v int) bool
}

func (a *app) addDipMenu() {
	a.dipMenu = call(pCreatePopupMenu)
	appendMenu(a.gameMenu, mfSeparator, 0, "")
	appendMenu(a.gameMenu, mfPopup, a.dipMenu, dipMenuLabel)
}

// currentDips: the running game's settings (read on the game's thread).
func (a *app) currentDips() []arcade.DipSetting {
	s := a.sess
	if s == nil {
		return nil
	}
	d, ok := s.core.(dipCore)
	if !ok {
		return nil
	}
	var list []arcade.DipSetting
	s.call(func() error { list = d.Dips(); return nil })
	return list
}

// fillDips builds the menu as it opens: one submenu per setting.
func (a *app) fillDips() {
	for call(pGetMenuItemCount, a.dipMenu) > 0 {
		call(pDeleteMenu, a.dipMenu, 0, mfByPosition)
	}
	list := a.currentDips()
	if len(list) == 0 {
		text := "(for arcade games)"
		if a.sess != nil && a.sess.game.System == "arcade" {
			text = "(this game has none)"
		}
		appendMenu(a.dipMenu, mfString|mfGrayed, 0, text)
		return
	}
	for i, d := range list {
		if i >= 32 {
			break
		}
		sub := call(pCreatePopupMenu)
		cur := ""
		for k, o := range d.Options {
			if k >= 16 {
				break
			}
			f := uint32(mfString)
			if o.Value == d.Value {
				f |= mfChecked
				cur = o.Label
			}
			appendMenu(sub, f, uintptr(cmdDip0+i*16+k), strings.ReplaceAll(o.Label, "&", "&&"))
		}
		appendMenu(a.dipMenu, mfPopup, sub, strings.ReplaceAll(d.Label+": "+cur, "&", "&&"))
	}
	appendMenu(a.dipMenu, mfSeparator, 0, "")
	appendMenu(a.dipMenu, mfString|mfGrayed, 0, "Most games read these when they start: Reset (Ctrl+R) after a change")
}

func (a *app) dipCommand(id int) {
	s := a.sess
	if s == nil {
		return
	}
	d, ok := s.core.(dipCore)
	if !ok {
		return
	}
	list := a.currentDips()
	i, k := (id-cmdDip0)/16, (id-cmdDip0)%16
	if i >= len(list) || k >= len(list[i].Options) {
		return
	}
	dip, v := list[i], list[i].Options[k].Value
	s.call(func() error { d.SetDip(dip.ID, v); return nil })
	if a.settings.Dips == nil {
		a.settings.Dips = map[string]map[string]int{}
	}
	m := a.settings.Dips[s.game.Name]
	if m == nil {
		m = map[string]int{}
		a.settings.Dips[s.game.Name] = m
	}
	m[dip.ID] = v
	saveSettings(a.settings)
	a.showMessage(fmt.Sprintf("%s: %s (Reset if it doesn't take effect)", dip.Label, list[i].Options[k].Label))
}
