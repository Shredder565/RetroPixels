//! Finds each background layer in a capture (its tilemap, tile graphics and
//! palettes, from the PPU registers) and where every byte of them came from:
//! the DMA transfer that wrote it and the ROM address that transfer read,
//! following transfers through WRAM, or the CPU code that wrote it.

use crate::capture::{wram_addr, Capture, DmaRecord, Writer};
use crate::gfx::{self, Image};
use crate::rom::Rom;

/// Where one byte of VRAM or CGRAM came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Origin {
    /// read from ROM file offset `off` by DMA `dma` (through WRAM by DMA `via` first, if set)
    Rom { off: usize, dma: u32, via: Option<u32> },
    /// found by searching the ROM for the data (written by the CPU, so no DMA says where)
    Found { off: usize },
    /// copied from WRAM by DMA `dma`; the CPU at `pc` wrote it there (decompressed, most likely)
    CpuWram { wram: usize, dma: u32, pc: Option<u32> },
    /// the CPU at `pc` wrote it straight to the PPU
    Cpu { pc: u32 },
    /// a DMA from somewhere else (WRAM nobody traced, or a cartridge chip)
    Other { dma: u32, from: i32 },
    Hdma,
    Never,
    Untracked,
}

impl Origin {
    pub fn rom_off(self) -> Option<usize> {
        match self {
            Origin::Rom { off, .. } | Origin::Found { off } => Some(off),
            _ => None,
        }
    }
    pub fn dma(self) -> Option<u32> {
        match self {
            Origin::Rom { dma, .. } | Origin::CpuWram { dma, .. } | Origin::Other { dma, .. } => Some(dma),
            _ => None,
        }
    }
}

pub fn origin(cap: &Capture, rom: &Rom, src: i32, from: i32) -> Origin {
    match Writer::from_code(src) {
        Writer::Never => Origin::Never,
        Writer::Hdma => Origin::Hdma,
        Writer::Untracked => Origin::Untracked,
        Writer::Cpu(pc) => Origin::Cpu { pc },
        Writer::Dma(id) => {
            if from < 0 {
                return Origin::Other { dma: id, from };
            }
            let a = from as u32;
            if let Some(off) = rom.bus_to_off(a) {
                return Origin::Rom { off, dma: id, via: None };
            }
            if let Some(w) = wram_addr(a) {
                match Writer::from_code(cap.wram_src[w]) {
                    Writer::Dma(id2) => {
                        let f2 = cap.wram_from[w];
                        if f2 >= 0 {
                            if let Some(off) = rom.bus_to_off(f2 as u32) {
                                return Origin::Rom { off, dma: id, via: Some(id2) };
                            }
                        }
                        return Origin::Other { dma: id, from };
                    }
                    Writer::Cpu(pc) => return Origin::CpuWram { wram: w, dma: id, pc: Some(pc) },
                    _ => return Origin::CpuWram { wram: w, dma: id, pc: None },
                }
            }
            Origin::Other { dma: id, from }
        }
    }
}

/// Bits per pixel of each BG in each mode (0: no such layer; 7: Mode 7).
pub fn layer_bpp(mode: u32, bg: usize) -> u32 {
    const T: [[u32; 4]; 8] = [[2, 2, 2, 2], [4, 4, 2, 0], [4, 4, 0, 0], [8, 4, 0, 0], [8, 2, 0, 0], [4, 2, 0, 0], [4, 0, 0, 0], [7, 0, 0, 0]];
    T[(mode & 7) as usize][bg]
}

#[derive(Clone, Debug)]
pub struct Layer {
    pub bg: usize,
    pub mode: u32,
    pub bpp: u32,
    /// tilemap's VRAM word address
    pub map_base: usize,
    /// tilemap size in entries (32 or 64 each way)
    pub map_w: usize,
    pub map_h: usize,
    pub tile16: bool,
    /// tile graphics' VRAM word address
    pub char_base: usize,
    pub on_main: bool,
    pub on_sub: bool,
    pub hofs: u32,
    pub vofs: u32,
}

impl Layer {
    pub fn name(&self) -> String {
        format!("BG{}", self.bg + 1)
    }
    pub fn cell(&self) -> usize {
        if self.tile16 {
            16
        } else {
            8
        }
    }
    pub fn px_w(&self) -> usize {
        self.map_w * self.cell()
    }
    pub fn px_h(&self) -> usize {
        self.map_h * self.cell()
    }
    pub fn entries(&self) -> usize {
        self.map_w * self.map_h
    }
    /// VRAM word of the tilemap entry for cell (tx, ty).
    pub fn entry_word(&self, tx: usize, ty: usize) -> usize {
        let screen = tx / 32 + (ty / 32) * if self.map_w == 64 { 2 } else { 1 };
        (self.map_base + screen * 0x400 + (ty % 32) * 32 + (tx % 32)) & 0x7fff
    }
    pub fn tile_words(&self) -> usize {
        4 * self.bpp as usize
    }
    /// VRAM word where 8x8 tile number n (0..1023) starts.
    pub fn tile_word(&self, n: usize) -> usize {
        (self.char_base + (n & 0x3ff) * self.tile_words()) & 0x7fff
    }
    pub fn colors_per_palette(&self) -> usize {
        1 << self.bpp
    }
    pub fn palettes(&self) -> usize {
        if self.bpp == 8 {
            1
        } else {
            8
        }
    }
    /// CGRAM index of colour 0 of palette p.
    pub fn palette_base(&self, p: usize) -> usize {
        match self.bpp {
            2 if self.mode == 0 => self.bg * 32 + p * 4,
            2 => p * 4,
            4 => p * 16,
            _ => 0,
        }
    }
    /// The 8x8 tile numbers a cell's entry uses, as (tile, dx, dy) in the
    /// cell (flips already applied): one for 8x8 cells, four for 16x16.
    pub fn cell_tiles(&self, e: u16) -> Vec<(usize, usize, usize)> {
        let t = (e & 0x3ff) as usize;
        if !self.tile16 {
            return vec![(t, 0, 0)];
        }
        let (h, v) = (e & 0x4000 != 0, e & 0x8000 != 0);
        let mut out = Vec::with_capacity(4);
        for dy in 0..2 {
            for dx in 0..2 {
                let sx = if h { 1 - dx } else { dx };
                let sy = if v { 1 - dy } else { dy };
                out.push(((t + sx + sy * 16) & 0x3ff, dx, dy));
            }
        }
        out
    }
}

/// The background layers the captured PPU settings describe.
pub fn layers(cap: &Capture) -> Vec<Layer> {
    let p = &cap.meta.ppu;
    let mode = p.bgmode & 7;
    let mut out = vec![];
    for bg in 0..4 {
        let bpp = layer_bpp(mode, bg);
        if bpp == 0 {
            continue;
        }
        let sc = p.bgsc[bg] as usize;
        let (map_w, map_h) = match sc & 3 {
            0 => (32, 32),
            1 => (64, 32),
            2 => (32, 64),
            _ => (64, 64),
        };
        let mut l = Layer {
            bg,
            mode,
            bpp,
            map_base: (sc & 0xfc) << 8,
            map_w,
            map_h,
            tile16: p.bgmode & (0x10 << bg) != 0,
            char_base: ((p.bgnba[bg] as usize) & 0xf) << 12,
            on_main: p.tm & (1 << bg) != 0,
            on_sub: p.ts & (1 << bg) != 0,
            hofs: p.hofs[bg],
            vofs: p.vofs[bg],
        };
        if bpp == 7 {
            // Mode 7: 128x128 map of bytes in the low halves, tiles in the high halves
            l.map_base = 0;
            l.map_w = 128;
            l.map_h = 128;
            l.char_base = 0;
            l.tile16 = false;
        }
        out.push(l);
    }
    out
}

#[derive(Clone, Debug)]
pub struct Segment {
    /// first byte (VRAM byte address, or CGRAM byte address for palettes)
    pub start: usize,
    pub len: usize,
    pub origin: Origin,
}

impl Segment {
    pub fn describe(&self, cap: &Capture, rom: &Rom, cgram: bool) -> String {
        let place = if cgram {
            format!("CGRAM colours {}-{}", self.start / 2, (self.start + self.len - 1) / 2)
        } else {
            format!("VRAM ${:04X}-${:04X} (words)", self.start / 2, (self.start + self.len - 1) / 2)
        };
        let dma = |id: u32| -> String {
            match cap.record(id) {
                Some(r) => format!("DMA #{id} (channel {}, started by code at {})", r.ch, bus(r.pc)),
                None => format!("DMA #{id}"),
            }
        };
        let what = match self.origin {
            Origin::Rom { off, dma: d, via } => {
                let a = rom.off_to_bus(off, 0);
                match via {
                    None => format!("ROM file offset 0x{off:06X} ({}), copied by {}", bus(a), dma(d)),
                    Some(v) => format!("ROM file offset 0x{off:06X} ({}), copied to WRAM by {}, then to VRAM by {}", bus(a), dma(v), dma(d)),
                }
            }
            Origin::Found { off } => format!("ROM file offset 0x{off:06X} ({}), found by searching the ROM (the CPU wrote it)", bus(rom.off_to_bus(off, 0))),
            Origin::CpuWram { wram, dma: d, pc } => match pc {
                Some(pc) => format!("WRAM ${:05X}, written by the CPU at {} (compressed graphics, most likely), then copied by {}", wram, bus(pc), dma(d)),
                None => format!("WRAM ${:05X}, copied by {}", wram, dma(d)),
            },
            Origin::Cpu { pc } => format!("written by the CPU at {}", bus(pc)),
            Origin::Other { dma: d, from } => format!("{} from {}", dma(d), if from >= 0 { bus(from as u32) } else { "a cartridge chip or the PPU".into() }),
            Origin::Hdma => "HDMA".into(),
            Origin::Never => "never written since the game started".into(),
            Origin::Untracked => "a DMA the tracer had no room left to note".into(),
        };
        format!("{place}: {what}")
    }
}

pub fn bus(a: u32) -> String {
    format!("${:02X}:{:04X}", a >> 16 & 0xff, a & 0xffff)
}

/// Every VRAM and CGRAM byte's origin, with the ROM searched for data the
/// CPU wrote (or decompressed) so it can be found anyway when the ROM holds it
/// uncompressed.
pub struct Origins {
    pub vram: Vec<Origin>,
    pub cgram: Vec<Origin>,
}

impl Origins {
    pub fn new(cap: &Capture, rom: &Rom) -> Origins {
        let vram = (0..0x10000).map(|i| origin(cap, rom, cap.vram_src[i], cap.vram_from[i])).collect();
        let cgram = (0..512).map(|i| origin(cap, rom, cap.cgram_src[i], cap.cgram_from[i])).collect();
        Origins { vram, cgram }
    }

    /// For byte ranges [start, start+len) of VRAM with no ROM origin, look
    /// for the same bytes in the ROM; a unique match gives them one.
    pub fn search_vram(&mut self, cap: &Capture, rom: &Rom, start: usize, len: usize) {
        search(&mut self.vram, &cap.vram, rom, start, len, 0x10000);
    }
    pub fn search_cgram(&mut self, cap: &Capture, rom: &Rom, start: usize, len: usize) {
        search(&mut self.cgram, &cap.cgram, rom, start, len, 512);
    }
}

fn search(org: &mut [Origin], mem: &[u8], rom: &Rom, start: usize, len: usize, size: usize) {
    // runs of bytes without a ROM origin (and that something wrote)
    let mut i = start;
    let end = (start + len).min(size);
    while i < end {
        if org[i].rom_off().is_some() || org[i] == Origin::Never {
            i += 1;
            continue;
        }
        let mut j = i;
        while j < end && org[j].rom_off().is_none() && org[j] != Origin::Never {
            j += 1;
        }
        let pat = &mem[i..j];
        // short or featureless runs match by chance
        if pat.len() >= 16 && pat.iter().any(|&b| b != pat[0]) {
            if let Some(off) = find_unique(&rom.data, pat) {
                for k in i..j {
                    org[k] = Origin::Found { off: off + (k - i) };
                }
            }
        }
        i = j;
    }
}

fn find_unique(hay: &[u8], pat: &[u8]) -> Option<usize> {
    let mut found = None;
    if pat.len() > hay.len() {
        return None;
    }
    let first = pat[0];
    let mut i = 0;
    while i + pat.len() <= hay.len() {
        if hay[i] == first && &hay[i..i + pat.len()] == pat {
            if found.is_some() {
                return None;
            }
            found = Some(i);
        }
        i += 1;
    }
    found
}

/// Groups bytes [start, start+len) into runs with one origin that steps
/// through the ROM byte by byte.
pub fn segments(org: &[Origin], start: usize, len: usize) -> Vec<Segment> {
    let mut out: Vec<Segment> = vec![];
    for i in start..start + len {
        let o = org[i & (org.len() - 1)];
        if let Some(last) = out.last_mut() {
            let n = last.len;
            let cont = match (last.origin, o) {
                (Origin::Rom { off: a, dma: d1, via: v1 }, Origin::Rom { off: b, dma: d2, via: v2 }) => b == a + n && d1 == d2 && v1 == v2,
                (Origin::Found { off: a }, Origin::Found { off: b }) => b == a + n,
                (Origin::CpuWram { wram: a, dma: d1, pc: p1 }, Origin::CpuWram { wram: b, dma: d2, .. }) => {
                    let _ = p1;
                    b == a + n && d1 == d2
                }
                (Origin::Cpu { .. }, Origin::Cpu { .. }) => true,
                (x, y) => x == y,
            };
            if cont && last.start + n == i {
                last.len += 1;
                continue;
            }
        }
        out.push(Segment { start: i, len: 1, origin: o });
    }
    out
}

/// What the debugger shows for one layer.
pub struct LayerInfo {
    pub layer: Layer,
    /// 8x8 tile numbers the tilemap uses, sorted
    pub tiles: Vec<usize>,
    /// palettes the tilemap uses
    pub palettes: Vec<usize>,
    pub map_segments: Vec<Segment>,
    pub char_segments: Vec<Segment>,
    pub pal_segments: Vec<Segment>,
    /// bytes with a ROM address / all bytes, for tilemap, tiles, palettes
    pub traced: [(usize, usize); 3],
    pub notes: Vec<String>,
}

impl LayerInfo {
    pub fn map_bytes(&self) -> (usize, usize) {
        let l = &self.layer;
        (l.map_base * 2, l.map_w * l.map_h * 2)
    }
}

/// The tile numbers and palettes a layer's tilemap uses.
pub fn usage(cap: &Capture, l: &Layer) -> (Vec<usize>, Vec<usize>) {
    let mut tiles = vec![false; 1024];
    let mut pals = [false; 8];
    for ty in 0..l.map_h {
        for tx in 0..l.map_w {
            let e = cap.vram_word(l.entry_word(tx, ty));
            for (t, _, _) in l.cell_tiles(e) {
                tiles[t] = true;
            }
            pals[((e >> 10) & 7) as usize] = true;
        }
    }
    let tiles = (0..1024).filter(|&t| tiles[t]).collect();
    let pals = if l.bpp == 8 { vec![0] } else { (0..8).filter(|&p| pals[p]).collect() };
    (tiles, pals)
}

pub fn analyze_layer(cap: &Capture, rom: &Rom, org: &mut Origins, l: &Layer) -> LayerInfo {
    let mut notes = vec![];
    if l.bpp == 7 {
        notes.push("Mode 7 background: shown, but swapping Mode 7 isn't supported yet.".into());
    }
    if !l.on_main && !l.on_sub {
        notes.push("This layer is switched off on screen right now.".into());
    }
    let (tiles, palettes) = if l.bpp == 7 { ((0..256).collect(), vec![0]) } else { usage(cap, l) };
    // tilemap
    let (ms, ml) = if l.bpp == 7 { (0, 0x8000) } else { (l.map_base * 2, l.entries() * 2) };
    org.search_vram(cap, rom, ms, ml);
    // tile graphics: each run of used tiles
    let tb = if l.bpp == 7 { 128 } else { l.tile_words() * 2 };
    let mut char_ranges: Vec<(usize, usize)> = vec![];
    if l.bpp == 7 {
        char_ranges.push((0, 0x8000));
    } else {
        for &t in &tiles {
            let s = l.tile_word(t) * 2;
            if let Some(last) = char_ranges.last_mut() {
                if last.0 + last.1 == s {
                    last.1 += tb;
                    continue;
                }
            }
            char_ranges.push((s, tb));
        }
    }
    for &(s, n) in &char_ranges {
        org.search_vram(cap, rom, s, n);
    }
    // palettes
    let mut pal_ranges: Vec<(usize, usize)> = vec![];
    for &p in &palettes {
        let s = l.palette_base(p) * 2;
        let n = if l.bpp == 7 { 512 } else { l.colors_per_palette() * 2 };
        pal_ranges.push((s, n));
    }
    for &(s, n) in &pal_ranges {
        org.search_cgram(cap, rom, s, n);
    }
    let map_segments = if l.bpp == 7 { vec![] } else { segments(&org.vram, ms, ml) };
    let mut char_segments = vec![];
    for &(s, n) in &char_ranges {
        char_segments.extend(segments(&org.vram, s, n));
    }
    let mut pal_segments = vec![];
    for &(s, n) in &pal_ranges {
        pal_segments.extend(segments(&org.cgram, s, n));
    }
    let count = |org: &[Origin], ranges: &[(usize, usize)]| -> (usize, usize) {
        let mut t = 0;
        let mut all = 0;
        for &(s, n) in ranges {
            for i in s..s + n {
                all += 1;
                if org[i % org.len()].rom_off().is_some() {
                    t += 1;
                }
            }
        }
        (t, all)
    };
    let traced = [count(&org.vram, &[(ms, ml)]), count(&org.vram, &char_ranges), count(&org.cgram, &pal_ranges)];
    if traced[1].0 < traced[1].1 {
        let cpu = char_segments.iter().any(|s| matches!(s.origin, Origin::CpuWram { pc: Some(_), .. } | Origin::Cpu { .. }));
        if cpu {
            notes.push("Some tile graphics were written by the game's code (decompressed from packed data, most likely) and aren't in the ROM as they are; those can't be swapped in place.".into());
        }
    }
    LayerInfo { layer: l.clone(), tiles, palettes, map_segments, char_segments, pal_segments, traced, notes }
}

/// The whole tilemap drawn with the captured tiles and colours; colour 0 is
/// transparent.
pub fn render(cap: &Capture, l: &Layer) -> Image {
    if l.bpp == 7 {
        let mut img = Image::new(1024, 1024);
        for ty in 0..128 {
            for tx in 0..128 {
                let t = cap.vram[(ty * 128 + tx) * 2] as usize;
                for y in 0..8 {
                    for x in 0..8 {
                        let c = cap.vram[(t * 64 + y * 8 + x) * 2 + 1] as usize;
                        if c != 0 {
                            let [r, g, b] = gfx::rgb(cap.color(c));
                            img.set(tx * 8 + x, ty * 8 + y, [r, g, b, 255]);
                        }
                    }
                }
            }
        }
        return img;
    }
    let mut img = Image::new(l.px_w(), l.px_h());
    let cell = l.cell();
    let tb = gfx::tile_bytes(l.bpp);
    for ty in 0..l.map_h {
        for tx in 0..l.map_w {
            let e = cap.vram_word(l.entry_word(tx, ty));
            let (h, v) = (e & 0x4000 != 0, e & 0x8000 != 0);
            let pb = l.palette_base(((e >> 10) & 7) as usize);
            for (t, dx, dy) in l.cell_tiles(e) {
                let w = l.tile_word(t);
                let mut bytes = vec![0u8; tb];
                for (i, b) in bytes.iter_mut().enumerate() {
                    *b = cap.vram[(w * 2 + i) & 0xffff];
                }
                let px = gfx::flip(&gfx::decode_tile(&bytes, l.bpp), h, v);
                for y in 0..8 {
                    for x in 0..8 {
                        let c = px[y * 8 + x] as usize;
                        if c == 0 {
                            continue;
                        }
                        let [r, g, b] = gfx::rgb(cap.color(pb + c));
                        img.set(tx * cell + dx * 8 + x, ty * cell + dy * 8 + y, [r, g, b, 255]);
                    }
                }
            }
        }
    }
    img
}

/// A text report of a capture: each layer and where its data came from.
pub fn report(cap: &Capture, rom: &Rom) -> String {
    let mut org = Origins::new(cap, rom);
    let mut s = String::new();
    let p = &cap.meta.ppu;
    s += &format!("{} — frame {} — {} — Mode {}{}\n", if cap.meta.title.is_empty() { rom.title() } else { cap.meta.title.clone() }, cap.meta.frame, rom.map.name(), p.bgmode & 7, if p.bgmode & 8 != 0 { " (BG3 priority)" } else { "" });
    for l in layers(cap) {
        let info = analyze_layer(cap, rom, &mut org, &l);
        s += &format!(
            "\n{}: {} bpp, tilemap {}x{} at VRAM ${:04X}, tiles at ${:04X}{}, {} tiles used, palettes {:?}{}\n",
            l.name(),
            if l.bpp == 7 { 8 } else { l.bpp },
            l.map_w,
            l.map_h,
            l.map_base,
            l.char_base,
            if l.tile16 { " (16x16)" } else { "" },
            info.tiles.len(),
            info.palettes,
            if l.on_main || l.on_sub { "" } else { " — off" }
        );
        if !l.on_main && !l.on_sub {
            continue;
        }
        let pct = |(a, b): (usize, usize)| if b == 0 { 0 } else { a * 100 / b };
        s += &format!("  in the ROM: tilemap {}%, tiles {}%, palettes {}%\n", pct(info.traced[0]), pct(info.traced[1]), pct(info.traced[2]));
        for n in &info.notes {
            s += &format!("  note: {n}\n");
        }
        for (title, segs, cg) in [("tilemap", &info.map_segments, false), ("tiles", &info.char_segments, false), ("palettes", &info.pal_segments, true)] {
            s += &format!("  {title}:\n");
            for seg in segs.iter().take(24) {
                s += &format!("    {}\n", seg.describe(cap, rom, cg));
            }
            if segs.len() > 24 {
                s += &format!("    … {} more\n", segs.len() - 24);
            }
        }
    }
    s += "\nDMA transfers to VRAM and CGRAM (latest first):\n";
    let mut recs: Vec<&DmaRecord> = cap.meta.records.iter().filter(|r| r.vaddr >= 0 || r.cgaddr >= 0 || r.wmadd >= 0).collect();
    recs.sort_by(|a, b| b.last.cmp(&a.last).then(b.id.cmp(&a.id)));
    for r in recs.iter().take(64) {
        let dest = if r.vaddr >= 0 {
            format!("VRAM ${:04X}", r.vaddr)
        } else if r.cgaddr >= 0 {
            format!("CGRAM {}", r.cgaddr)
        } else {
            format!("WRAM ${:05X}", r.wmadd)
        };
        let src = match rom.bus_to_off(r.src) {
            Some(o) => format!("{} (ROM 0x{o:06X})", bus(r.src)),
            None => bus(r.src),
        };
        s += &format!("  #{} ch{} {} bytes {} -> {}, code {}, frames {}-{} x{}\n", r.id, r.ch, r.size, src, dest, bus(r.pc), r.first, r.last, r.count);
    }
    s
}
