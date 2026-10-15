package main

// Which player you are in arcade games for 3 or 4 players (the Game menu's
// Player menu, like the web page's player picker and setSeat in host.js):
// the keyboard and the first controller play as the player picked, and the
// other controllers move round with it, so controller p plays as player
// (p + seat) mod players. Kept per game in settings.json.

import "fmt"

// characterCore is a core that names its players' characters (the arcade
// core; nil names for a game without them).
type characterCore interface{ Characters() []string }

// seats: how many players the Player menu offers for this game (0: none).
func (s *session) seats() int {
	if _, ok := s.core.(characterCore); !ok || s.info.Players < 3 {
		return 0
	}
	return min(s.info.Players, 4)
}

// seatName: "Player 2 (Michelangelo)", or "Player 2" in a game without names.
func (s *session) seatName(n int) string {
	t := fmt.Sprintf("Player %d", n+1)
	if c, ok := s.core.(characterCore); ok {
		if names := c.Characters(); n < len(names) && names[n] != "" {
			t += " (" + names[n] + ")"
		}
	}
	return t
}

// setSeat makes the keyboard and first controller play as player n (one
// out of range: player 1) and says which it is.
func (s *session) setSeat(n int) int {
	if n < 0 || n >= s.seats() {
		n = 0
	}
	s.seat.Store(int32(n))
	return n
}

// seatOf: the player controller p plays as.
func (s *session) seatOf(p, players int) int {
	if seat := int(s.seat.Load()); seat > 0 && players > 0 {
		return (p + seat) % players
	}
	return p
}

// seatMessage: the title bar's "You are player 2 (Michelangelo)".
func (s *session) seatMessage(n int) string {
	return "You are p" + s.seatName(n)[1:]
}
