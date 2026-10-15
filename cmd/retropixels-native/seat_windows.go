package main

import "strings"

// The Game menu's Player menu (seat.go): "Player 1 (Leonardo)" ...
// "Player 4 (Raphael)" in arcade games for 3 or 4 players. No keys of its
// own (the number keys are the games' Start and Coin keys).

const cmdSeat0 = 1200 // ..1203

const seatMenuLabel = "Pla&yer"

func (a *app) addSeatMenu() {
	a.seatMenu = call(pCreatePopupMenu)
	appendMenu(a.gameMenu, mfPopup, a.seatMenu, seatMenuLabel)
}

// fillSeats builds the menu as it opens, the current player ticked.
func (a *app) fillSeats() {
	for call(pGetMenuItemCount, a.seatMenu) > 0 {
		call(pDeleteMenu, a.seatMenu, 0, mfByPosition)
	}
	s := a.sess
	n := 0
	if s != nil {
		n = s.seats()
	}
	if n == 0 {
		appendMenu(a.seatMenu, mfString|mfGrayed, 0, "(for arcade games for 3 or 4 players)")
		return
	}
	for i := 0; i < n; i++ {
		// "Player &1 (Leonardo)": the menu's own underlined key
		label := strings.ReplaceAll(s.seatName(i), "&", "&&")
		label = strings.Replace(label, "Player ", "Player &", 1)
		appendMenu(a.seatMenu, mfString, uintptr(cmdSeat0+i), label)
	}
	cur := int(s.seat.Load())
	call(pCheckMenuRadioItem, a.seatMenu, cmdSeat0, uintptr(cmdSeat0+n-1), uintptr(cmdSeat0+cur), mfByCommand)
}

// pickSeat: the menu's choice, kept for the game and said in the title bar.
func (a *app) pickSeat(n int) {
	s := a.sess
	if s == nil || s.seats() == 0 {
		return
	}
	n = s.setSeat(n)
	if a.settings.Seats == nil {
		a.settings.Seats = map[string]int{}
	}
	if n == 0 {
		delete(a.settings.Seats, s.game.Name)
	} else {
		a.settings.Seats[s.game.Name] = n
	}
	saveSettings(a.settings)
	a.showMessage(s.seatMessage(n))
}

// restoreSeat: a newly opened game starts as the player picked last time
// (quietly, as the web page).
func (a *app) restoreSeat(s *session) {
	if n, ok := a.settings.Seats[s.game.Name]; ok {
		s.setSeat(n)
	}
}
