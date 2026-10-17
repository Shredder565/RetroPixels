package wiiu

func newHostGPU(g *Latte) (hwRenderer, error) {
	hw, err := newD3DBackend(g, false)
	if err != nil {
		return nil, err
	}
	return hw, nil
}
