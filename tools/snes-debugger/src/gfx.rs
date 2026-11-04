//! SNES graphics formats: BGR555 colours, 2/4/8 bits-per-pixel planar tiles,
//! tilemap entries, and PNG files.

use std::path::Path;

/// BGR555 -> 8-bit RGB, the way RetroPixels' emulator shows it.
pub fn rgb(c: u16) -> [u8; 3] {
    let f = |v: u16| -> u8 {
        let v = (v & 31) as u32;
        ((v << 3) | (v >> 2)) as u8
    };
    [f(c), f(c >> 5), f(c >> 10)]
}

/// 8-bit RGB -> BGR555 (nearest).
pub fn bgr555(r: u8, g: u8, b: u8) -> u16 {
    let q = |v: u8| ((v as u32 * 31 + 127) / 255) as u16;
    q(r) | q(g) << 5 | q(b) << 10
}

/// Bytes in one 8x8 tile.
pub fn tile_bytes(bpp: u32) -> usize {
    8 * bpp as usize
}

/// Decodes one planar 8x8 tile (bpp 2, 4 or 8) to 64 colour indices.
pub fn decode_tile(b: &[u8], bpp: u32) -> [u8; 64] {
    let mut out = [0u8; 64];
    for y in 0..8 {
        for x in 0..8 {
            let bit = 7 - x;
            let mut c = 0u8;
            for p in 0..bpp as usize {
                let byte = b[(p >> 1) * 16 + y * 2 + (p & 1)];
                c |= ((byte >> bit) & 1) << p;
            }
            out[y * 8 + x] = c;
        }
    }
    out
}

/// Encodes 64 colour indices as one planar 8x8 tile.
pub fn encode_tile(px: &[u8; 64], bpp: u32) -> Vec<u8> {
    let mut out = vec![0u8; tile_bytes(bpp)];
    for y in 0..8 {
        for x in 0..8 {
            let c = px[y * 8 + x];
            let bit = 7 - x;
            for p in 0..bpp as usize {
                if (c >> p) & 1 != 0 {
                    out[(p >> 1) * 16 + y * 2 + (p & 1)] |= 1 << bit;
                }
            }
        }
    }
    out
}

pub fn flip(px: &[u8; 64], h: bool, v: bool) -> [u8; 64] {
    let mut o = [0u8; 64];
    for y in 0..8 {
        for x in 0..8 {
            let sx = if h { 7 - x } else { x };
            let sy = if v { 7 - y } else { y };
            o[y * 8 + x] = px[sy * 8 + sx];
        }
    }
    o
}

/// A picture as 8-bit RGBA, row by row.
#[derive(Clone)]
pub struct Image {
    pub w: usize,
    pub h: usize,
    pub px: Vec<u8>,
}

impl Image {
    pub fn new(w: usize, h: usize) -> Image {
        Image { w, h, px: vec![0; w * h * 4] }
    }
    pub fn get(&self, x: usize, y: usize) -> [u8; 4] {
        let o = (y * self.w + x) * 4;
        [self.px[o], self.px[o + 1], self.px[o + 2], self.px[o + 3]]
    }
    pub fn set(&mut self, x: usize, y: usize, c: [u8; 4]) {
        let o = (y * self.w + x) * 4;
        self.px[o..o + 4].copy_from_slice(&c);
    }

    pub fn load_png(path: &Path) -> Result<Image, String> {
        let f = std::fs::File::open(path).map_err(|e| format!("can't open {}: {e}", path.display()))?;
        let mut dec = png::Decoder::new(std::io::BufReader::new(f));
        dec.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
        let mut rd = dec.read_info().map_err(|e| format!("{}: {e}", path.display()))?;
        let mut buf = vec![0; rd.output_buffer_size()];
        let info = rd.next_frame(&mut buf).map_err(|e| format!("{}: {e}", path.display()))?;
        let (w, h) = (info.width as usize, info.height as usize);
        let mut img = Image::new(w, h);
        let src = &buf[..info.buffer_size()];
        let ch = match info.color_type {
            png::ColorType::Grayscale => 1,
            png::ColorType::GrayscaleAlpha => 2,
            png::ColorType::Rgb => 3,
            png::ColorType::Rgba => 4,
            png::ColorType::Indexed => return Err("unexpected indexed PNG after expansion".into()),
        };
        for i in 0..w * h {
            let s = &src[i * ch..i * ch + ch];
            let c = match ch {
                1 => [s[0], s[0], s[0], 255],
                2 => [s[0], s[0], s[0], s[1]],
                3 => [s[0], s[1], s[2], 255],
                _ => [s[0], s[1], s[2], s[3]],
            };
            img.px[i * 4..i * 4 + 4].copy_from_slice(&c);
        }
        Ok(img)
    }

    pub fn save_png(&self, path: &Path) -> Result<(), String> {
        let f = std::fs::File::create(path).map_err(|e| format!("can't write {}: {e}", path.display()))?;
        let mut enc = png::Encoder::new(std::io::BufWriter::new(f), self.w as u32, self.h as u32);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        let mut wr = enc.write_header().map_err(|e| e.to_string())?;
        wr.write_image_data(&self.px).map_err(|e| e.to_string())
    }

    /// Scaled to w x h (area average when shrinking, nearest when growing).
    pub fn resized(&self, w: usize, h: usize) -> Image {
        if w == self.w && h == self.h {
            return self.clone();
        }
        let mut out = Image::new(w, h);
        for y in 0..h {
            let y0 = y * self.h / h;
            let y1 = (((y + 1) * self.h + h - 1) / h).max(y0 + 1).min(self.h);
            for x in 0..w {
                let x0 = x * self.w / w;
                let x1 = (((x + 1) * self.w + w - 1) / w).max(x0 + 1).min(self.w);
                let mut acc = [0u32; 4];
                let mut n = 0u32;
                for sy in y0..y1 {
                    for sx in x0..x1 {
                        let c = self.get(sx, sy);
                        // colour weighted by alpha so transparent pixels don't darken edges
                        let a = c[3] as u32;
                        acc[0] += c[0] as u32 * a;
                        acc[1] += c[1] as u32 * a;
                        acc[2] += c[2] as u32 * a;
                        acc[3] += a;
                        n += 1;
                    }
                }
                let a = acc[3];
                let c = if a == 0 {
                    [0, 0, 0, 0]
                } else {
                    [(acc[0] / a) as u8, (acc[1] / a) as u8, (acc[2] / a) as u8, (a / n) as u8]
                };
                out.set(x, y, c);
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn tile_round_trip() {
        for bpp in [2u32, 4, 8] {
            let mut px = [0u8; 64];
            for (i, p) in px.iter_mut().enumerate() {
                *p = ((i * 37) % (1 << bpp)) as u8;
            }
            let b = encode_tile(&px, bpp);
            assert_eq!(b.len(), tile_bytes(bpp));
            assert_eq!(decode_tile(&b, bpp), px);
        }
    }
    #[test]
    fn colours() {
        assert_eq!(rgb(0x7fff), [255, 255, 255]);
        assert_eq!(bgr555(255, 0, 0), 31);
        assert_eq!(bgr555(0, 0, 255), 31 << 10);
    }
}
