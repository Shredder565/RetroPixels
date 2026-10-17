package namco

// starfield is Galaga's 05xx star generator (see drawStars).
type starfield struct {
	scroll int
}

func (s *starfield) reset() { *s = starfield{} }

// step moves the stars on at the end of a frame, at the speed the game set.
func (s *starfield) step(b *board) {}

func (v *video) drawStars(b *board) {}
