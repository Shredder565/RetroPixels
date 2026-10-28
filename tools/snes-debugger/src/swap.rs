//! Swapping a background for a picture: the picture is reduced to the
//! layer's colour limits (palettes of 3, 15 or 255 colours, one per tile),
//! cut into tiles (repeats and flipped repeats shared), and the tiles,
//! tilemap and (optionally) palettes are written back to the ROM addresses
//! the captured data came from. When there are more tiles than room, the
//! tile data moves to free ROM space with room for the extra tiles, and the
//! game's transfer is repointed there (or, without that, similar tiles are
//! merged until they fit).

use crate::analyze::{self, Layer, Origin, Origins};
use crate::capture::Capture;
use crate::gfx::{self, Image};
use crate::quantize::{self, C};
use crate::repoint;
use crate::rom::Rom;
use std::collections::HashMap;

#[derive(Clone, Debug)]
pub struct Options {
    /// make new palettes for the picture (written to the ROM) instead of
    /// using the layer's own colours
    pub new_palette: bool,
    pub dither: bool,
    /// more tiles than room: move the tiles to free ROM space with room for
    /// more and repoint the transfer (otherwise merge similar tiles)
    pub grow: bool,
    /// may add a bank to the ROM when no free space is big enough
    pub expand_rom: bool,
}

impl Default for Options {
    fn default() -> Self {
        Options { new_palette: true, dither: false, grow: true, expand_rom: true }
    }
}

pub struct SwapResult {
    pub rom: Rom,
    /// the layer as it will look
    pub preview: Image,
    pub log: Vec<String>,
}

/// Writes to the ROM, through the origins of VRAM / CGRAM bytes.
struct Writer {
    writes: HashMap<usize, u8>,
    conflicts: usize,
}

impl Writer {
    fn put(&mut self, off: usize, v: u8) {
        if let Some(&old) = self.writes.get(&off) {
            if old != v {
                self.conflicts += 1;
            }
        }
        self.writes.insert(off, v);
    }
}

pub fn swap(cap: &Capture, rom: &Rom, layer: &Layer, picture: &Image, opt: &Options) -> Result<SwapResult, String> {
    let l = layer;
    if l.bpp == 7 {
        return Err("Mode 7 backgrounds can't be swapped yet".into());
    }
    let mut log = vec![];
    let mut org = Origins::new(cap, rom);
    let info = analyze::analyze_layer(cap, rom, &mut org, l);
    let all = analyze::layers(cap);

    // --- tilemap: every entry must have a ROM address
    let (ms, ml) = info.map_bytes();
    for i in ms..ms + ml {
        if org.vram[i & 0xffff].rom_off().is_none() {
            let seg = analyze::segments(&org.vram, i & 0xffff, 1).remove(0);
            return Err(format!(
                "this layer's tilemap isn't in the ROM as it is, so it can't be swapped: {}. (Scrolling levels build their tilemap from level data as they go; title screens and fixed backdrops are the ones that can be swapped.)",
                seg.describe(cap, rom, false)
            ));
        }
    }

    // --- VRAM other layers, sprites and tilemaps use: not to be touched
    let mut protected = vec![false; 0x10000];
    for i in ms..ms + ml {
        protected[i & 0xffff] = true;
    }
    let mut other_cg = vec![false; 256];
    for o in &all {
        if o.bg == l.bg || o.bpp == 7 {
            continue;
        }
        let (ts, ps) = analyze::usage(cap, o);
        let visible = o.on_main || o.on_sub;
        for i in o.map_base * 2..o.map_base * 2 + o.entries() * 2 {
            if visible {
                protected[i & 0xffff] = true;
            }
        }
        if visible {
            for t in ts {
                let w = o.tile_word(t);
                for i in 0..o.tile_words() * 2 {
                    protected[(w * 2 + i) & 0xffff] = true;
                }
            }
            for p in ps {
                for c in 0..o.colors_per_palette() {
                    other_cg[(o.palette_base(p) + c) & 0xff] = true;
                }
            }
        }
    }
    // sprite tiles (both tables)
    let obsel = cap.meta.ppu.obsel as usize;
    let obj0 = (obsel & 7) << 13;
    let obj1 = (obj0 + (((obsel >> 3) & 3) + 1) * 0x1000) & 0x7fff;
    if cap.meta.ppu.tm & 0x10 != 0 || cap.meta.ppu.ts & 0x10 != 0 {
        for base in [obj0, obj1] {
            for w in base..base + 0x1000 {
                protected[(w * 2) & 0xffff] = true;
                protected[(w * 2 + 1) & 0xffff] = true;
            }
        }
    }

    // --- palettes the picture may use
    let n_col = l.colors_per_palette();
    let used: Vec<usize> = info.palettes.clone();
    let cg_traced = |p: usize| (1..n_col).all(|c| {
        let i = (l.palette_base(p) + c) & 0xff;
        org.cgram[i * 2].rom_off().is_some() && org.cgram[i * 2 + 1].rom_off().is_some()
    });
    // new colours go in the layer's own palettes and in ones nothing else on screen uses
    let candidates: Vec<usize> = (0..l.palettes())
        .filter(|&p| {
            let free = (1..n_col).all(|c| !other_cg[(l.palette_base(p) + c) & 0xff]);
            (used.contains(&p) || free) && cg_traced(p)
        })
        .collect();
    let mut new_pal = opt.new_palette;
    if new_pal && candidates.is_empty() {
        log.push("The layer's palettes aren't in the ROM as they are, so the picture uses the game's own colours.".into());
        new_pal = false;
    }
    let pal_ids: Vec<usize> = if new_pal {
        candidates
    } else if used.is_empty() {
        vec![0]
    } else {
        used.clone()
    };

    // --- the picture, at the tilemap's size
    let (pw, ph) = (l.px_w(), l.px_h());
    let pic = picture.resized(pw, ph);
    if picture.w != pw || picture.h != ph {
        log.push(format!("The picture ({}x{}) was scaled to the tilemap's {}x{}.", picture.w, picture.h, pw, ph));
    }
    let px: Vec<Option<C>> = (0..pw * ph)
        .map(|i| {
            let c = &pic.px[i * 4..i * 4 + 4];
            if c[3] < 128 {
                None
            } else {
                Some(quantize::from_rgb8(c[0], c[1], c[2]))
            }
        })
        .collect();
    let cell = l.cell();
    let (cw, chh) = (l.map_w, l.map_h);
    let cell_px = |cx: usize, cy: usize| -> Vec<C> {
        let mut v = vec![];
        for y in 0..cell {
            for x in 0..cell {
                if let Some(c) = px[(cy * cell + y) * pw + cx * cell + x] {
                    v.push(c);
                }
            }
        }
        v
    };
    let cells: Vec<Vec<C>> = (0..cw * chh).map(|i| cell_px(i % cw, i / cw)).collect();

    // --- palettes: colour 0 of each stays (transparent / backdrop)
    let pal_colors: Vec<Vec<C>>; // per palette in pal_ids: colours 1..n
    let group: Vec<usize>;
    if new_pal {
        let (g, pals) = quantize::group_palettes(&cells, pal_ids.len(), n_col - 1);
        group = g;
        pal_colors = pals;
        log.push(format!("New colours: {} palette{} of up to {} colours.", pal_ids.len(), if pal_ids.len() == 1 { "" } else { "s" }, n_col - 1));
    } else {
        pal_colors = pal_ids
            .iter()
            .map(|&p| (1..n_col).map(|c| quantize::from555(cap.color(l.palette_base(p) + c))).collect())
            .collect();
        group = cells
            .iter()
            .map(|c| {
                let mut best = 0;
                let mut be = f32::MAX;
                for (g, pc) in pal_colors.iter().enumerate() {
                    let e = quantize::error(c, pc);
                    if e < be {
                        be = e;
                        best = g;
                    }
                }
                best
            })
            .collect();
        log.push(format!("Using the game's own colours (palette{} {:?}).", if pal_ids.len() == 1 { "" } else { "s" }, pal_ids));
    }

    // --- colour indices for every pixel (with error diffusion when dithering)
    let mut idx = vec![0u8; pw * ph];
    let mut err = vec![[0f32; 3]; pw * ph];
    for y in 0..ph {
        for x in 0..pw {
            let i = y * pw + x;
            let Some(c0) = px[i] else { continue };
            let g = group[(y / cell) * cw + x / cell];
            let pal = &pal_colors[g];
            if pal.is_empty() {
                continue;
            }
            let c = if opt.dither { [c0[0] + err[i][0], c0[1] + err[i][1], c0[2] + err[i][2]] } else { c0 };
            let k = quantize::nearest(pal, c);
            idx[i] = (k + 1) as u8;
            if opt.dither {
                let q = pal[k];
                let e = [c[0] - q[0], c[1] - q[1], c[2] - q[2]];
                for (dx, dy, f) in [(1i32, 0i32, 7.0 / 16.0), (-1, 1, 3.0 / 16.0), (0, 1, 5.0 / 16.0), (1, 1, 1.0 / 16.0)] {
                    let (nx, ny) = (x as i32 + dx, y as i32 + dy);
                    if nx < 0 || ny < 0 || nx >= pw as i32 || ny >= ph as i32 {
                        continue;
                    }
                    let j = ny as usize * pw + nx as usize;
                    for k in 0..3 {
                        err[j][k] = (err[j][k] + e[k] * f).clamp(-8.0, 8.0);
                    }
                }
            }
        }
    }

    // --- cells as tiles; repeats (also flipped) share one
    let cell_idx = |cx: usize, cy: usize| -> Vec<u8> {
        let mut v = Vec::with_capacity(cell * cell);
        for y in 0..cell {
            for x in 0..cell {
                v.push(idx[(cy * cell + y) * pw + cx * cell + x]);
            }
        }
        v
    };
    let flipc = |v: &[u8], h: bool, vf: bool| -> Vec<u8> {
        let mut o = vec![0u8; cell * cell];
        for y in 0..cell {
            for x in 0..cell {
                let sx = if h { cell - 1 - x } else { x };
                let sy = if vf { cell - 1 - y } else { y };
                o[y * cell + x] = v[sy * cell + sx];
            }
        }
        o
    };
    let mut uniq: Vec<Vec<u8>> = vec![];
    let mut uses: Vec<usize> = vec![];
    let mut seen: HashMap<Vec<u8>, usize> = HashMap::new();
    let mut cell_tile: Vec<(usize, bool, bool)> = vec![]; // per cell: unique tile, h, v
    for cy in 0..chh {
        for cx in 0..cw {
            let v = cell_idx(cx, cy);
            let mut found = None;
            for (h, vf) in [(false, false), (true, false), (false, true), (true, true)] {
                if let Some(&u) = seen.get(&flipc(&v, h, vf)) {
                    found = Some((u, h, vf));
                    break;
                }
            }
            let t = match found {
                Some(t) => t,
                None => {
                    seen.insert(v.clone(), uniq.len());
                    uniq.push(v);
                    uses.push(0);
                    (uniq.len() - 1, false, false)
                }
            };
            uses[t.0] += 1;
            cell_tile.push(t);
        }
    }

    // --- room: tile slots whose bytes come from the ROM, from the same data as the layer's tiles
    let tb = l.tile_words() * 2;
    let mut feeds: Vec<Origin> = vec![]; // the transfers / found blocks the layer's tiles came from
    for &t in &info.tiles {
        let b = l.tile_word(t) * 2;
        let o = org.vram[b & 0xffff];
        if o.rom_off().is_some() && !feeds.iter().any(|f| same_feed(*f, o)) {
            feeds.push(o);
        }
    }
    let tile_ok = |org: &Origins, t: usize| -> bool {
        let b = l.tile_word(t) * 2;
        (0..tb).all(|i| {
            let k = (b + i) & 0xffff;
            let o = org.vram[k];
            !protected[k] && o.rom_off().is_some() && feeds.iter().any(|f| same_feed(*f, o))
        })
    };
    // a slot is one tile (8x8) or a 2x2 block (16x16)
    let slot_tiles = |s: usize| -> Vec<usize> {
        if l.tile16 {
            vec![s, (s + 1) & 0x3ff, (s + 16) & 0x3ff, (s + 17) & 0x3ff]
        } else {
            vec![s]
        }
    };
    let slot_starts: Vec<usize> = if l.tile16 { (0..1024).filter(|s| s % 2 == 0 && (s / 16) % 2 == 0).collect() } else { (0..1024).collect() };
    let mut slots: Vec<usize> = slot_starts.iter().copied().filter(|&s| slot_tiles(s).iter().all(|&t| tile_ok(&org, t))).collect();
    // the layer's own tiles first, then the rest in order
    slots.sort_by_key(|s| (!info.tiles.contains(s), *s));

    let mut rom2 = rom.clone();
    if uniq.len() > slots.len() && opt.grow {
        match grow(cap, &mut rom2, l, &mut org, &protected, &info.tiles, uniq.len() - slots.len(), opt, &mut log) {
            Ok(()) => {
                slots = slot_starts.iter().copied().filter(|&s| slot_tiles(s).iter().all(|&t| tile_ok_any(&org, &protected, l, t))).collect();
                slots.sort_by_key(|s| (!info.tiles.contains(s), *s));
            }
            Err(e) => log.push(format!("Couldn't make room for more tiles ({e}); similar tiles are merged instead.")),
        }
    }
    if slots.is_empty() {
        return Err("none of the layer's tiles can be written: their graphics aren't in the ROM as they are (compressed, most likely)".into());
    }
    log.push(format!("The picture needs {} different tiles; there is room for {}.", uniq.len(), slots.len()));

    // --- too many tiles: keep the most used, draw the others with the nearest kept one
    let mut remap: Vec<(usize, bool, bool)> = (0..uniq.len()).map(|u| (u, false, false)).collect();
    if uniq.len() > slots.len() {
        let mut order: Vec<usize> = (0..uniq.len()).collect();
        order.sort_by(|a, b| uses[*b].cmp(&uses[*a]).then(a.cmp(b)));
        let keep: Vec<usize> = order[..slots.len()].to_vec();
        // compare in colour: each unique tile drawn with the palette of a cell that uses it,
        // against the kept tiles drawn with that same palette
        let mut pal_of = vec![0usize; uniq.len()];
        for (ci, &(u, _, _)) in cell_tile.iter().enumerate() {
            pal_of[u] = group[ci];
        }
        let color = |g: usize, v: &[u8]| -> Vec<C> {
            v.iter().map(|&k| if k == 0 { [-20.0, -20.0, -20.0] } else { pal_colors[g].get(k as usize - 1).copied().unwrap_or([0.0; 3]) }).collect()
        };
        const FLIPS: [(bool, bool); 4] = [(false, false), (true, false), (false, true), (true, true)];
        let flipped: Vec<Vec<Vec<u8>>> = keep.iter().map(|&k| FLIPS.iter().map(|&(h, v)| flipc(&uniq[k], h, v)).collect()).collect();
        let mut kept_col: Vec<Option<Vec<Vec<Vec<C>>>>> = vec![None; pal_colors.len()];
        for &u in &order[slots.len()..] {
            let g = pal_of[u];
            if kept_col[g].is_none() {
                kept_col[g] = Some(flipped.iter().map(|fl| fl.iter().map(|v| color(g, v)).collect()).collect());
            }
            let kc = kept_col[g].as_ref().unwrap();
            let me = color(g, &uniq[u]);
            let mut best = (f32::MAX, 0usize, false, false);
            for (ki, flips) in kc.iter().enumerate() {
                for (fi, &(h, v)) in FLIPS.iter().enumerate() {
                    let mut d = 0f32;
                    for (a, b) in me.iter().zip(&flips[fi]) {
                        d += quantize::dist(*a, *b);
                        if d >= best.0 {
                            break;
                        }
                    }
                    if d < best.0 {
                        best = (d, keep[ki], h, v);
                    }
                }
            }
            // uniq[u] == flip(kept, h, v)  =>  draw u's cells with kept and the flips combined
            remap[u] = (best.1, best.2, best.3);
        }
        log.push(format!("{} similar tiles were merged to fit.", uniq.len() - slots.len()));
    }

    // --- write
    let mut w = Writer { writes: HashMap::new(), conflicts: 0 };
    let mut vram = cap.vram.clone();
    let mut cgram = cap.cgram.clone();
    let mut slot_of: HashMap<usize, usize> = HashMap::new();
    let mut next = 0;
    for u in 0..uniq.len() {
        if remap[u].0 != u {
            continue;
        }
        let s = slots[next];
        next += 1;
        slot_of.insert(u, s);
        // the cell's 8x8 tiles
        let v = &uniq[u];
        for (k, t) in slot_tiles(s).into_iter().enumerate() {
            let (dx, dy) = (k % 2, k / 2);
            let mut p = [0u8; 64];
            for y in 0..8 {
                for x in 0..8 {
                    p[y * 8 + x] = v[(dy * 8 + y) * cell + dx * 8 + x];
                }
            }
            let bytes = gfx::encode_tile(&p, l.bpp);
            let b = l.tile_word(t) * 2;
            for (i, &val) in bytes.iter().enumerate() {
                let k = (b + i) & 0xffff;
                vram[k] = val;
                w.put(org.vram[k].rom_off().unwrap(), val);
            }
        }
    }
    for cy in 0..chh {
        for cx in 0..cw {
            let ci = cy * cw + cx;
            let (u, h1, v1) = cell_tile[ci];
            let (k, h2, v2) = remap[u];
            let slot = slot_of[&k];
            let (h, v) = (h1 ^ h2, v1 ^ v2);
            let wd = l.entry_word(cx, cy);
            let old = cap.vram_word(wd);
            let e = (slot as u16 & 0x3ff) | (pal_ids[group[ci]] as u16) << 10 | (old & 0x2000) | (h as u16) << 14 | (v as u16) << 15;
            for (i, val) in [e as u8, (e >> 8) as u8].into_iter().enumerate() {
                let b = (wd * 2 + i) & 0xffff;
                vram[b] = val;
                w.put(org.vram[b].rom_off().unwrap(), val);
            }
        }
    }
    if new_pal {
        for (g, &p) in pal_ids.iter().enumerate() {
            for (c, &col) in pal_colors[g].iter().enumerate() {
                let i = (l.palette_base(p) + c + 1) & 0xff;
                let v = quantize::to555(col);
                for (j, val) in [v as u8, (v >> 8) as u8].into_iter().enumerate() {
                    cgram[i * 2 + j] = val;
                    w.put(org.cgram[i * 2 + j].rom_off().unwrap(), val);
                }
            }
        }
    }
    if w.conflicts > 0 {
        log.push(format!(
            "Note: {} bytes are used by the game in more than one place (the same ROM data loaded twice), so the two places can't differ; the last one written wins.",
            w.conflicts
        ));
    }
    let n = w.writes.len();
    for (off, v) in w.writes {
        rom2.data[off] = v;
    }
    rom2.fix_checksum();
    log.push(format!("{n} bytes of the ROM changed; the checksum was updated."));
    let preview = render_with(&vram, &cgram, l);
    Ok(SwapResult { rom: rom2, preview, log })
}

/// Two origins are from the same data (same transfer, or both found by search).
fn same_feed(a: Origin, b: Origin) -> bool {
    match (a, b) {
        (Origin::Rom { dma: d1, via: v1, .. }, Origin::Rom { dma: d2, via: v2, .. }) => d1 == d2 && v1 == v2,
        (Origin::Found { .. }, Origin::Found { .. }) => true,
        _ => false,
    }
}

fn tile_ok_any(org: &Origins, protected: &[bool], l: &Layer, t: usize) -> bool {
    let b = l.tile_word(t) * 2;
    (0..l.tile_words() * 2).all(|i| {
        let k = (b + i) & 0xffff;
        !protected[k] && org.vram[k].rom_off().is_some()
    })
}

/// Makes room for `more` tiles: the transfer that loads most of the layer's
/// tiles moves to free ROM space with room for them after its data, and is
/// repointed there. Updates org so the moved and new bytes point at their new
/// ROM place.
#[allow(clippy::too_many_arguments)]
fn grow(cap: &Capture, rom: &mut Rom, l: &Layer, org: &mut Origins, protected: &[bool], tiles: &[usize], more: usize, opt: &Options, log: &mut Vec<String>) -> Result<(), String> {
    // the transfer straight from ROM that loads most of the layer's tiles
    let mut count: HashMap<u32, usize> = HashMap::new();
    for &t in tiles {
        if let Origin::Rom { dma, via: None, .. } = org.vram[(l.tile_word(t) * 2) & 0xffff] {
            *count.entry(dma).or_default() += 1;
        }
    }
    let Some((&id, _)) = count.iter().max_by_key(|(id, n)| (**n, std::cmp::Reverse(**id))) else {
        return Err("the tiles don't come straight from the ROM by DMA".into());
    };
    let rec = cap.record(id).ok_or("the transfer isn't in the capture")?.clone();
    if rec.vaddr < 0 || rec.vmain & 0x0c != 0 || rec.vmain & 3 != 0 {
        return Err("the transfer isn't a plain copy to VRAM".into());
    }
    if rec.ctrl & 0x18 != 0 {
        return Err("the transfer doesn't step through the ROM".into());
    }
    let src_off = rom.bus_to_off(rec.src).ok_or("the transfer's source isn't ROM")?;
    let tb = l.tile_words() * 2;
    let extra_tiles = if l.tile16 { more * 4 } else { more };
    // how far past the transfer's end VRAM is free for this layer
    let start_b = rec.vaddr as usize * 2;
    let end_b = start_b + rec.size as usize;
    let mut room = 0;
    let char_end_b = (l.char_base + 1024 * l.tile_words()) * 2;
    while room < extra_tiles * tb + if l.tile16 { 16 * tb * 2 } else { 0 } {
        let k = end_b + room;
        if k >= 0x10000 || k >= char_end_b || protected[k] {
            break;
        }
        room += 1;
    }
    let want = (room / tb).min(extra_tiles + if l.tile16 { 32 } else { 0 });
    if want == 0 {
        return Err(format!("VRAM after the tiles (${:04X}) is in use", end_b / 2));
    }
    let new_size = rec.size as usize + want * tb;
    if new_size > 0xffff {
        return Err("the tiles would be more than one transfer can move".into());
    }
    let avoid = vec![(src_off, rec.size as usize)];
    let at = match rom.find_free(new_size, 32, &avoid) {
        Some(a) => a,
        None if opt.expand_rom && new_size <= rom.map.bank_size() => {
            let a = rom.expand()?;
            log.push(format!("No free space was big enough, so the ROM grew to {} KB.", rom.data.len() / 1024));
            a
        }
        None => return Err(format!("no free space of {new_size} bytes in the ROM")),
    };
    let new_bus = rom.off_to_bus(at, rec.src);
    let patches = repoint::repoint(rom, &rec, new_bus, new_size as u32, true)?;
    // copy the data, then the extra room (zeros for now)
    let old: Vec<u8> = rom.data[src_off..src_off + rec.size as usize].to_vec();
    rom.data[at..at + old.len()].copy_from_slice(&old);
    for b in &mut rom.data[at + old.len()..at + new_size] {
        *b = 0;
    }
    for p in &patches {
        rom.data[p.off..p.off + p.new.len()].copy_from_slice(&p.new);
        log.push(format!("Repointed the {} (ROM 0x{:06X}: {:02X?} -> {:02X?}).", p.what, p.off, p.old, p.new));
    }
    // VRAM bytes this transfer wrote now come from the new place; the new room too
    for i in 0..new_size {
        let k = (start_b + i) & 0xffff;
        if i < rec.size as usize {
            if let Origin::Rom { off, dma, via } = org.vram[k] {
                if dma == id {
                    org.vram[k] = Origin::Rom { off: at + (off - src_off), dma, via };
                }
            }
        } else {
            org.vram[k] = Origin::Rom { off: at + i, dma: id, via: None };
        }
    }
    log.push(format!(
        "The tiles moved to free ROM space at 0x{at:06X} ({}) with room for {want} more, and the transfer now moves {new_size} bytes.",
        analyze::bus(new_bus)
    ));
    Ok(())
}

/// The layer drawn from the given VRAM and CGRAM.
pub fn render_with(vram: &[u8], cgram: &[u8], l: &Layer) -> Image {
    let word = |w: usize| vram[(w & 0x7fff) * 2] as u16 | (vram[(w & 0x7fff) * 2 + 1] as u16) << 8;
    let color = |i: usize| (cgram[(i & 0xff) * 2] as u16 | (cgram[(i & 0xff) * 2 + 1] as u16) << 8) & 0x7fff;
    let mut img = Image::new(l.px_w(), l.px_h());
    let cell = l.cell();
    let tb = gfx::tile_bytes(l.bpp);
    for ty in 0..l.map_h {
        for tx in 0..l.map_w {
            let e = word(l.entry_word(tx, ty));
            let (h, v) = (e & 0x4000 != 0, e & 0x8000 != 0);
            let pb = l.palette_base(((e >> 10) & 7) as usize);
            for (t, dx, dy) in l.cell_tiles(e) {
                let w = l.tile_word(t);
                let bytes: Vec<u8> = (0..tb).map(|i| vram[(w * 2 + i) & 0xffff]).collect();
                let p = gfx::flip(&gfx::decode_tile(&bytes, l.bpp), h, v);
                for y in 0..8 {
                    for x in 0..8 {
                        let c = p[y * 8 + x] as usize;
                        if c != 0 {
                            let [r, g, b] = gfx::rgb(color(pb + c));
                            img.set(tx * cell + dx * 8 + x, ty * cell + dy * 8 + y, [r, g, b, 255]);
                        }
                    }
                }
            }
        }
    }
    img
}
