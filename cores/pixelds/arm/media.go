package arm

// media: the ARMv6 media instructions (packing, saturation, parallel add,
// byte reversal and extension), filled in with the 3DS stage.
func (c *CPU) media(op uint32) { c.undefined(op) }
