//! Colour reduction for SNES backgrounds: each tile picks one of a few small
//! palettes (3, 15 or 255 colours plus transparent), so the picture is split
//! into groups of tiles that share colours, each group gets its own palette,
//! and every pixel takes the nearest colour of its tile's palette.

/// A colour as 5-bit-per-channel floats (the SNES's own steps).
pub type C = [f32; 3];

pub fn from555(c: u16) -> C {
    [(c & 31) as f32, ((c >> 5) & 31) as f32, ((c >> 10) & 31) as f32]
}

pub fn to555(c: C) -> u16 {
    let q = |v: f32| v.round().clamp(0.0, 31.0) as u16;
    q(c[0]) | q(c[1]) << 5 | q(c[2]) << 10
}

pub fn from_rgb8(r: u8, g: u8, b: u8) -> C {
    [r as f32 * 31.0 / 255.0, g as f32 * 31.0 / 255.0, b as f32 * 31.0 / 255.0]
}

/// Distance as the eye weighs it, roughly (green counts most).
#[inline]
pub fn dist(a: C, b: C) -> f32 {
    let dr = a[0] - b[0];
    let dg = a[1] - b[1];
    let db = a[2] - b[2];
    3.0 * dr * dr + 4.0 * dg * dg + 2.0 * db * db
}

/// Index (into pal) of the colour nearest c.
pub fn nearest(pal: &[C], c: C) -> usize {
    let mut best = 0;
    let mut bd = f32::MAX;
    for (i, &p) in pal.iter().enumerate() {
        let d = dist(p, c);
        if d < bd {
            bd = d;
            best = i;
        }
    }
    best
}

/// Up to n colours that represent the pixels well: median cut, then a few
/// rounds of k-means. Colours come back snapped to the SNES's 5-bit steps,
/// without repeats.
pub fn palette(px: &[C], n: usize) -> Vec<C> {
    if px.is_empty() || n == 0 {
        return vec![];
    }
    // distinct 5-bit colours first: when there are few enough, use them as they are
    let mut uniq: Vec<u16> = px.iter().map(|&c| to555(c)).collect();
    uniq.sort_unstable();
    uniq.dedup();
    if uniq.len() <= n {
        return uniq.into_iter().map(from555).collect();
    }
    // median cut
    let mut boxes: Vec<Vec<C>> = vec![px.to_vec()];
    while boxes.len() < n {
        // split the box with the widest spread (weighted by its size)
        let mut pick = None;
        let mut best = -1.0f32;
        for (i, b) in boxes.iter().enumerate() {
            if b.len() < 2 {
                continue;
            }
            let (ch, range) = widest(b);
            let score = range * (b.len() as f32).sqrt();
            let _ = ch;
            if score > best && range > 0.0 {
                best = score;
                pick = Some(i);
            }
        }
        let Some(i) = pick else { break };
        let mut b = boxes.swap_remove(i);
        let (ch, _) = widest(&b);
        b.sort_by(|x, y| x[ch].partial_cmp(&y[ch]).unwrap());
        let hi = b.split_off(b.len() / 2);
        boxes.push(b);
        boxes.push(hi);
    }
    let mut cents: Vec<C> = boxes.iter().filter(|b| !b.is_empty()).map(|b| mean(b)).collect();
    // k-means refinement
    for _ in 0..4 {
        let mut acc = vec![([0f32; 3], 0usize); cents.len()];
        for &c in px {
            let k = nearest(&cents, c);
            acc[k].0[0] += c[0];
            acc[k].0[1] += c[1];
            acc[k].0[2] += c[2];
            acc[k].1 += 1;
        }
        for (k, (s, m)) in acc.into_iter().enumerate() {
            if m > 0 {
                cents[k] = [s[0] / m as f32, s[1] / m as f32, s[2] / m as f32];
            }
        }
    }
    let mut out: Vec<u16> = cents.into_iter().map(to555).collect();
    out.sort_unstable();
    out.dedup();
    out.into_iter().map(from555).collect()
}

fn widest(b: &[C]) -> (usize, f32) {
    let mut lo = [f32::MAX; 3];
    let mut hi = [f32::MIN; 3];
    for c in b {
        for k in 0..3 {
            lo[k] = lo[k].min(c[k]);
            hi[k] = hi[k].max(c[k]);
        }
    }
    let w = [3.0f32.sqrt(), 2.0, 2.0f32.sqrt()];
    let r = [(hi[0] - lo[0]) * w[0], (hi[1] - lo[1]) * w[1], (hi[2] - lo[2]) * w[2]];
    let ch = if r[0] >= r[1] && r[0] >= r[2] {
        0
    } else if r[1] >= r[2] {
        1
    } else {
        2
    };
    (ch, r[ch])
}

fn mean(b: &[C]) -> C {
    let mut s = [0f32; 3];
    for c in b {
        s[0] += c[0];
        s[1] += c[1];
        s[2] += c[2];
    }
    let n = b.len().max(1) as f32;
    [s[0] / n, s[1] / n, s[2] / n]
}

/// Error of drawing pixels with pal (nearest colours).
pub fn error(px: &[C], pal: &[C]) -> f32 {
    if pal.is_empty() {
        return if px.is_empty() { 0.0 } else { f32::MAX / 4.0 };
    }
    px.iter().map(|&c| dist(pal[nearest(pal, c)], c)).sum()
}

/// Splits cells (each a list of its opaque pixels) into k groups and makes a
/// palette of up to n colours per group. Returns (group of each cell,
/// palettes).
pub fn group_palettes(cells: &[Vec<C>], k: usize, n: usize) -> (Vec<usize>, Vec<Vec<C>>) {
    let k = k.max(1);
    if k == 1 {
        let all: Vec<C> = sample(cells.iter().flatten().copied(), 40000);
        return (vec![0; cells.len()], vec![palette(&all, n)]);
    }
    // start: cells' average colours spread out (farthest first)
    let means: Vec<Option<C>> = cells.iter().map(|c| if c.is_empty() { None } else { Some(mean(c)) }).collect();
    let mut seeds: Vec<C> = vec![];
    if let Some(first) = means.iter().flatten().next() {
        seeds.push(*first);
    }
    while seeds.len() < k {
        let mut far = None;
        let mut fd = 0.0;
        for m in means.iter().flatten() {
            let d = seeds.iter().map(|s| dist(*s, *m)).fold(f32::MAX, f32::min);
            if d > fd {
                fd = d;
                far = Some(*m);
            }
        }
        match far {
            Some(m) => seeds.push(m),
            None => break,
        }
    }
    let mut group: Vec<usize> = means.iter().map(|m| m.map_or(0, |m| nearest(&seeds, m))).collect();
    let mut pals: Vec<Vec<C>> = vec![];
    for round in 0..6 {
        // a palette per group from its cells' pixels
        pals = (0..seeds.len().max(1))
            .map(|g| {
                let px = sample(cells.iter().zip(&group).filter(|(_, &gg)| gg == g).flat_map(|(c, _)| c.iter().copied()), 30000);
                palette(&px, n)
            })
            .collect();
        // each cell to the palette that draws it best
        let mut moved = 0;
        for (i, c) in cells.iter().enumerate() {
            if c.is_empty() {
                continue;
            }
            let mut best = group[i];
            let mut be = error(c, &pals[best]);
            for (g, p) in pals.iter().enumerate() {
                if g == group[i] || p.is_empty() {
                    continue;
                }
                let e = error(c, p);
                if e < be {
                    be = e;
                    best = g;
                }
            }
            if best != group[i] {
                group[i] = best;
                moved += 1;
            }
        }
        if moved == 0 && round > 0 {
            break;
        }
    }
    // a group that ended up with no cells keeps its last palette (harmless)
    (group, pals)
}

/// At most max items, evenly picked.
fn sample(it: impl Iterator<Item = C>, max: usize) -> Vec<C> {
    let v: Vec<C> = it.collect();
    if v.len() <= max {
        return v;
    }
    let step = v.len() as f64 / max as f64;
    (0..max).map(|i| v[(i as f64 * step) as usize]).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_when_few_colours() {
        let px = vec![[1.0, 2.0, 3.0], [31.0, 0.0, 0.0], [1.0, 2.0, 3.0]];
        let p = palette(&px, 15);
        assert_eq!(p.len(), 2);
        assert_eq!(error(&px, &p), 0.0);
    }
    #[test]
    fn groups_split_by_colour() {
        let red: Vec<C> = (0..64).map(|i| [31.0, (i % 4) as f32, 0.0]).collect();
        let blue: Vec<C> = (0..64).map(|i| [0.0, (i % 4) as f32, 31.0]).collect();
        let cells = vec![red.clone(), blue.clone(), red, blue];
        let (g, pals) = group_palettes(&cells, 2, 3);
        assert_eq!(pals.len(), 2);
        assert_eq!(g[0], g[2]);
        assert_eq!(g[1], g[3]);
        assert_ne!(g[0], g[1]);
    }
}
