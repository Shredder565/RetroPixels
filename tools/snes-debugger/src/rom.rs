//! The cartridge ROM: how SNES bus addresses map to file offsets (LoROM,
//! HiROM, ExHiROM, as RetroPixels' SNES emulator maps them), the copier
//! header, the checksum, and free space.

use std::path::Path;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Map {
    Lo,
    Hi,
    ExHi,
}

impl Map {
    pub fn parse(s: &str) -> Option<Map> {
        match s {
            "lo" => Some(Map::Lo),
            "hi" => Some(Map::Hi),
            "exhi" => Some(Map::ExHi),
            _ => None,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Map::Lo => "LoROM",
            Map::Hi => "HiROM",
            Map::ExHi => "ExHiROM",
        }
    }
    /// Bytes a DMA source can run through before its address wraps (a bank's ROM window).
    pub fn bank_size(self) -> usize {
        match self {
            Map::Lo => 0x8000,
            _ => 0x10000,
        }
    }
}

#[derive(Clone)]
pub struct Rom {
    /// the ROM without a copier header
    pub data: Vec<u8>,
    /// a 512-byte copier header the file had (kept when saving)
    pub copier: Option<Vec<u8>>,
    pub map: Map,
    pub header_at: usize,
}

impl Rom {
    pub fn load(path: &Path) -> Result<Rom, String> {
        let b = std::fs::read(path).map_err(|e| format!("can't read {}: {e}", path.display()))?;
        Rom::from_bytes(b)
    }

    pub fn from_bytes(b: Vec<u8>) -> Result<Rom, String> {
        let (copier, data) = if b.len() & 0x3ff == 0x200 {
            (Some(b[..0x200].to_vec()), b[0x200..].to_vec())
        } else {
            (None, b)
        };
        if data.len() < 0x8000 {
            return Err("that file is too small to be a SNES ROM".into());
        }
        let (header_at, map) = detect(&data);
        Ok(Rom { data, copier, map, header_at })
    }

    pub fn with_map(mut self, map: Map) -> Rom {
        self.map = map;
        self.header_at = match map {
            Map::Lo => 0x7fc0,
            Map::Hi => 0xffc0,
            Map::ExHi => 0x40ffc0,
        };
        self
    }

    pub fn title(&self) -> String {
        let h = self.header_at;
        if h + 21 > self.data.len() {
            return String::new();
        }
        self.data[h..h + 21].iter().map(|&c| if (0x20..0x7f).contains(&c) { c as char } else { ' ' }).collect::<String>().trim().to_string()
    }

    /// File offset (without the copier header) a CPU / DMA bus address reads,
    /// or None when that address isn't cartridge ROM. Mirrors of a ROM
    /// smaller than the window fold back the way the emulator folds them.
    pub fn bus_to_off(&self, addr: u32) -> Option<usize> {
        let bank = (addr >> 16) & 0xff;
        let off = addr & 0xffff;
        if bank == 0x7e || bank == 0x7f {
            return None; // WRAM
        }
        let b = bank & 0x7f;
        let raw = match self.map {
            Map::Lo => {
                if (0x70..0x7e).contains(&b) && off < 0x8000 {
                    return None; // SRAM
                }
                if off >= 0x8000 || b >= 0x40 {
                    (b << 15) | (off & 0x7fff)
                } else {
                    return None;
                }
            }
            Map::Hi => {
                if b >= 0x40 || off >= 0x8000 {
                    ((bank & 0x3f) << 16) | off
                } else {
                    return None;
                }
            }
            Map::ExHi => {
                if bank >= 0xc0 {
                    ((bank & 0x3f) << 16) | off
                } else if (0x40..0x7e).contains(&bank) {
                    0x400000 | ((bank & 0x3f) << 16) | off
                } else if off >= 0x8000 {
                    (if bank < 0x80 { 0x400000 } else { 0 }) | ((bank & 0x3f) << 16) | off
                } else {
                    return None;
                }
            }
        } as usize;
        Some(self.fold(raw))
    }

    fn fold(&self, raw: usize) -> usize {
        let n = self.data.len();
        if raw < n {
            return raw;
        }
        let (mut base, mut size, mut a, mut mask) = (0usize, n, raw & 0xffffff, 0x800000usize);
        while a >= size {
            while a & mask == 0 {
                mask >>= 1;
            }
            a -= mask;
            if size > mask {
                size -= mask;
                base += mask;
            }
            mask >>= 1;
        }
        base + a
    }

    /// The bus address a game would use to read file offset off (in the
    /// fast banks when like is, as most games address them).
    pub fn off_to_bus(&self, off: usize, like: u32) -> u32 {
        let fast = (like >> 16) >= 0x80;
        match self.map {
            Map::Lo => {
                let bank = (off >> 15) as u32 | if fast { 0x80 } else { 0 };
                (bank << 16) | 0x8000 | (off as u32 & 0x7fff)
            }
            Map::Hi => {
                let bank = 0xc0 | (off >> 16) as u32 & 0x3f;
                let bank = if fast { bank } else { bank & 0x7f };
                (bank << 16) | (off as u32 & 0xffff)
            }
            Map::ExHi => {
                if off >= 0x400000 {
                    (((0x40 | (off >> 16) as u32 & 0x3f)) << 16) | (off as u32 & 0xffff)
                } else {
                    (((0xc0 | (off >> 16) as u32 & 0x3f)) << 16) | (off as u32 & 0xffff)
                }
            }
        }
    }

    /// Writes the internal checksum and its complement.
    pub fn fix_checksum(&mut self) {
        let h = self.header_at;
        if h + 0x20 > self.data.len() {
            return;
        }
        self.data[h + 0x1c] = 0xff;
        self.data[h + 0x1d] = 0xff;
        self.data[h + 0x1e] = 0;
        self.data[h + 0x1f] = 0;
        // ROMs that aren't a power of two: the part past the largest power
        // of two counts repeated up to it, as the cartridge mirrors it
        let n = self.data.len();
        let mut p = 1usize;
        while p * 2 <= n {
            p *= 2;
        }
        let mut sum: u32 = self.data[..p].iter().map(|&b| b as u32).sum();
        if n > p {
            let rest = &self.data[p..];
            let rs: u32 = rest.iter().map(|&b| b as u32).sum();
            sum = sum.wrapping_add(rs.wrapping_mul((p / rest.len()) as u32));
        }
        let s = (sum & 0xffff) as u16;
        let c = s ^ 0xffff;
        self.data[h + 0x1c] = c as u8;
        self.data[h + 0x1d] = (c >> 8) as u8;
        self.data[h + 0x1e] = s as u8;
        self.data[h + 0x1f] = (s >> 8) as u8;
    }

    pub fn save(&self, path: &Path) -> Result<(), String> {
        let mut out = Vec::with_capacity(self.data.len() + 0x200);
        if let Some(c) = &self.copier {
            out.extend_from_slice(c);
        }
        out.extend_from_slice(&self.data);
        let tmp = path.with_extension("tmp-rp");
        std::fs::write(&tmp, &out).map_err(|e| format!("can't write {}: {e}", path.display()))?;
        std::fs::rename(&tmp, path).map_err(|e| format!("can't write {}: {e}", path.display()))
    }

    /// A run of len unused bytes (all 0xFF or all 0x00, padding the ROM
    /// was built with) that fits inside one bank's ROM window, at an address
    /// that's a multiple of align; avoid lists byte ranges not to use.
    /// The run must have at least `margin` more unused bytes before it, so
    /// the end of real data that happens to be 0xFF / 0x00 isn't taken.
    pub fn find_free(&self, len: usize, align: usize, avoid: &[(usize, usize)]) -> Option<usize> {
        let bank = self.map.bank_size();
        // unused bytes kept clear before the data: the end of real data can
        // look like padding (a table ending in 0xFF), so only well inside a run
        let margin = 64usize;
        let n = self.data.len();
        let mut best: Option<(usize, usize)> = None; // (start, room) — the run with the most room
        let mut i = 0;
        while i < n {
            let v = self.data[i];
            if v != 0xff && v != 0x00 {
                i += 1;
                continue;
            }
            let mut j = i + 1;
            while j < n && self.data[j] == v && j % bank != 0 {
                j += 1;
            }
            // run [i, j) inside one bank: the data goes at its end
            if j >= i + margin + len {
                let start = (j - len) / align * align;
                if start >= i + margin && !overlaps(start, len, avoid) && !self.in_header(start, len) {
                    let room = j - i;
                    if best.map_or(true, |(_, r)| room > r) {
                        best = Some((start, room));
                    }
                }
            }
            i = j;
        }
        best.map(|(s, _)| s)
    }

    fn in_header(&self, start: usize, len: usize) -> bool {
        let h = self.header_at & !0x3f;
        start < h + 0x40 && h < start + len
    }

    /// Makes the ROM one bank-window bigger (filled with 0xFF), for data
    /// that doesn't fit anywhere else; the header's size byte follows.
    pub fn expand(&mut self) -> Result<usize, String> {
        let at = self.data.len();
        let add = self.map.bank_size();
        let max = match self.map {
            Map::Lo => 0x400000,
            Map::Hi => 0x400000,
            Map::ExHi => 0x800000,
        };
        if at + add > max {
            return Err("the ROM is already as big as this cartridge type allows".into());
        }
        self.data.resize(at + add, 0xff);
        let kb = (self.data.len() + 1023) / 1024;
        let mut n = 0u8;
        while (1usize << n) < kb {
            n += 1;
        }
        let h = self.header_at;
        if h + 0x18 <= self.data.len() && self.data[h + 0x17] < n {
            self.data[h + 0x17] = n;
        }
        Ok(at)
    }
}

pub fn overlaps(start: usize, len: usize, ranges: &[(usize, usize)]) -> bool {
    ranges.iter().any(|&(s, l)| start < s + l && s < start + len)
}

/// Finds the internal header the way RetroPixels' emulator does.
fn detect(r: &[u8]) -> (usize, Map) {
    let size = r.len();
    let score = |base: usize| -> i32 {
        if base + 0x40 > size {
            return -100;
        }
        let mut s = 0;
        let chk = r[base + 0x1e] as u16 | (r[base + 0x1f] as u16) << 8;
        let cmp = r[base + 0x1c] as u16 | (r[base + 0x1d] as u16) << 8;
        if chk ^ cmp == 0xffff {
            s += 4;
        }
        let mode = r[base + 0x15];
        let reset = r[base + 0x3c] as usize | (r[base + 0x3d] as usize) << 8;
        if reset >= 0x8000 {
            s += 3;
        } else {
            s -= 4;
        }
        if base == 0x7fc0 && mode & 0xef == 0x20 {
            s += 2;
        }
        if base == 0xffc0 && mode & 0xef == 0x21 {
            s += 2;
        }
        if base == 0x40ffc0 && mode & 0xef == 0x25 {
            s += 3;
        }
        if base == 0x7fc0 && mode == 0x23 {
            s += 2;
        }
        let printable = (0..21).filter(|&i| (0x20..0x7f).contains(&r[base + i])).count();
        if printable > 16 {
            s += 1;
        }
        let op = r[(base & !0x7fff) + (reset & 0x7fff)];
        if [0x78, 0x18, 0x38, 0x9c, 0x4c, 0x5c].contains(&op) {
            s += 2;
        }
        s
    };
    let mut c = [(score(0x7fc0), 0x7fc0, Map::Lo), (score(0xffc0), 0xffc0, Map::Hi), (score(0x40ffc0), 0x40ffc0, Map::ExHi)];
    c.sort_by(|a, b| b.0.cmp(&a.0));
    (c[0].1, c[0].2)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lorom_mapping() {
        let mut d = vec![0xffu8; 0x10000];
        d[0x7fd5] = 0x20;
        d[0x7ffd] = 0x80;
        let r = Rom::from_bytes(d).unwrap().with_map(Map::Lo);
        assert_eq!(r.bus_to_off(0x018000), Some(0x8000));
        assert_eq!(r.bus_to_off(0x81c000), Some(0xc000));
        assert_eq!(r.bus_to_off(0x00f000), Some(0x7000));
        assert_eq!(r.bus_to_off(0x7e0000), None);
        assert_eq!(r.bus_to_off(0x001000), None);
        assert_eq!(r.off_to_bus(0xc000, 0x018000), 0x01c000);
        assert_eq!(r.off_to_bus(0xc000, 0x818000), 0x81c000);
    }

    #[test]
    fn hirom_mapping() {
        let d = vec![0u8; 0x20000];
        let r = Rom::from_bytes(d).unwrap().with_map(Map::Hi);
        assert_eq!(r.bus_to_off(0xc12345), Some(0x12345));
        assert_eq!(r.bus_to_off(0x018000), Some(0x18000));
        assert_eq!(r.bus_to_off(0x011000), None);
        assert_eq!(r.off_to_bus(0x12345, 0xc00000), 0xc12345);
    }
}
