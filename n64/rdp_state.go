package n64

// Placeholder rasterizer state hooks; the RDP's own version replaces this file.

func (d *RDP) marshalState() []byte { return nil }

func (d *RDP) unmarshalState(b []byte) error { return nil }
