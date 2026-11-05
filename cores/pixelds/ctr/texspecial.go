package ctr

// Texture unit 0's other kinds of texture: cube maps (six square faces,
// picked by the largest part of the direction in (u, v, w)), projected
// textures (u and v divided by w), and shadow maps, whose texels hold a
// depth (24 bits in green, blue, alpha) and an intensity (red): the
// intensity counts only where the fragment's depth (w, less a bias)
// isn't behind the stored one.

func (t *texUnit) sampleSpecial(u, v, w float32) [4]uint8 {
	switch t.typ {
	case 3: // projected
		if w != 0 {
			u, v = u/w, v/w
		}
		return t.sample(u, v)
	case 1, 4: // cube, shadow cube
		face, fu, fv, ma := cubeFace(u, v, w)
		fu, fv = fu/ma*0.5+0.5, fv/ma*0.5+0.5
		if t.typ == 1 {
			return t.sampleIn(t.faces[face], fu, fv)
		}
		return t.shadow(t.faces[face], fu, fv, ma)
	case 2: // shadow
		if !t.shadowOrtho && w != 0 {
			u, v = u/w, v/w
		}
		return t.shadow(t.data, u, v, abs32(w))
	}
	return [4]uint8{}
}

// cubeFace picks a cube map's face for a direction and gives the
// coordinates on it (before dividing by the major axis, ma).
func cubeFace(x, y, z float32) (face int, u, v, ma float32) {
	ax, ay, az := abs32(x), abs32(y), abs32(z)
	switch {
	case ax > ay && ax > az:
		u, v, ma = -z, -y, ax
		face = 0
		if x < 0 {
			u, face = z, 1
		}
	case ay > az:
		u, v, ma = x, z, ay
		face = 2
		if y < 0 {
			v, face = -z, 3
		}
	default:
		u, v, ma = -x, -y, az
		face = 5
		if z > 0 {
			u, face = x, 4
		}
	}
	if ma == 0 {
		ma = 1
	}
	return
}

func (t *texUnit) shadow(d []byte, u, v, depth float32) [4]uint8 {
	x := int(u * float32(t.w))
	y := int(v * float32(t.h))
	c := t.texelIn(d, x, y)
	z := int32(min(depth, 1)*0xffffff) - t.shadowBias<<1
	ref := int32(c[3])<<16 | int32(c[2])<<8 | int32(c[1])
	k := uint8(0)
	if ref >= z {
		k = c[0]
	}
	return [4]uint8{k, k, k, k}
}
