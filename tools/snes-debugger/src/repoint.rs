//! Repointing a DMA transfer: finding the bytes in the ROM that hold its
//! source address, bank and size, so data moved to free space is read from
//! there. The capture says which instructions wrote the DMA registers
//! ($43x2-$43x6); the values come from immediate loads just before those
//! stores, or from a table in the ROM.

use crate::analyze::bus;
use crate::capture::DmaRecord;
use crate::rom::Rom;

#[derive(Clone, Debug)]
pub struct Patch {
    pub off: usize,
    pub old: Vec<u8>,
    pub new: Vec<u8>,
    pub what: String,
}

/// One value the transfer used: register r of the channel (2: address,
/// 4: bank, 5: size).
struct Want {
    reg: usize,
    /// current value and its width in bytes (address, size: 2; bank: 1)
    value: u32,
    width: usize,
    new: u32,
    name: &'static str,
}

fn read(rom: &Rom, a: u32) -> Option<u8> {
    rom.bus_to_off(a).map(|o| rom.data[o])
}

/// Bytes of the instruction ending just before `pc` (the address after it).
fn bytes_before(rom: &Rom, pc: u32, n: u32) -> Option<Vec<u8>> {
    let bank = pc & 0xff0000;
    (0..n).map(|i| read(rom, bank | ((pc & 0xffff).wrapping_sub(n - i) & 0xffff))).collect()
}

/// The store instruction that wrote register reg of channel ch and ended at
/// pc: its first byte's address and which register it stores (A, X or Y).
fn find_store(rom: &Rom, pc: u32, ch: u8, reg: usize) -> Option<(u32, u8)> {
    let target = 0x4300u32 + ch as u32 * 16 + reg as u32;
    let bank = pc & 0xff0000;
    // a 16-bit store also covers the register before it
    let ok = |a: u32| a == target || a + 1 == target;
    if let Some(b) = bytes_before(rom, pc, 4) {
        if b[0] == 0x8f && ok(b[1] as u32 | (b[2] as u32) << 8) && b[3] == 0 {
            return Some((bank | (pc.wrapping_sub(4) & 0xffff), b'A'));
        }
    }
    if let Some(b) = bytes_before(rom, pc, 3) {
        let op = b[0];
        let a = b[1] as u32 | (b[2] as u32) << 8;
        if ok(a) {
            let r = match op {
                0x8d => Some(b'A'),
                0x8e => Some(b'X'),
                0x8c => Some(b'Y'),
                _ => None,
            };
            if let Some(r) = r {
                return Some((bank | (pc.wrapping_sub(3) & 0xffff), r));
            }
        }
    }
    if let Some(b) = bytes_before(rom, pc, 2) {
        // direct page set to $43x0 (a common trick): STA $02
        let low = (target & 0xff) as u8;
        if (b[1] == low || b[1].wrapping_add(1) == low) && matches!(b[0], 0x85 | 0x86 | 0x84) {
            let r = match b[0] {
                0x85 => b'A',
                0x86 => b'X',
                _ => b'Y',
            };
            return Some((bank | (pc.wrapping_sub(2) & 0xffff), r));
        }
    }
    None
}

/// Length of the instruction with opcode op (m16 / x16: the accumulator /
/// index registers are 16-bit, for immediate operands).
pub fn op_len(op: u8, m16: bool, x16: bool) -> usize {
    match op {
        0x09 | 0x29 | 0x49 | 0x69 | 0x89 | 0xa9 | 0xc9 | 0xe9 => 2 + m16 as usize,
        0xa0 | 0xa2 | 0xc0 | 0xe0 => 2 + x16 as usize,
        0x00 | 0x02 | 0x42 | 0xc2 | 0xe2 | 0x10 | 0x30 | 0x50 | 0x70 | 0x80 | 0x90 | 0xb0 | 0xd0 | 0xf0 | 0xd4 => 2,
        0x20 | 0x4c | 0x6c | 0x7c | 0xdc | 0xfc | 0x44 | 0x54 | 0x62 | 0x82 | 0xf4 => 3,
        0x22 | 0x5c => 4,
        _ => match op & 0x1f {
            0x01 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 | 0x11 | 0x12 | 0x13 | 0x14 | 0x15 | 0x16 | 0x17 => 2,
            0x0c | 0x0d | 0x0e | 0x19 | 0x1c | 0x1d | 0x1e => 3,
            0x0f | 0x1f => 4,
            0x08 | 0x0a | 0x0b | 0x18 | 0x1a | 0x1b => 1,
            _ => match op {
                0x40 | 0x60 | 0x6b | 0xcb | 0xdb | 0x88 | 0x8a | 0x8b | 0x98 | 0x9a | 0x9b | 0xa8 | 0xaa | 0xab | 0xb8 | 0xba | 0xbb | 0xc8 | 0xca | 0xd8 | 0xda
                | 0xe8 | 0xea | 0xeb | 0xf8 | 0xfa | 0xfb | 0x5a | 0x7a | 0x3b | 0x7b | 0x5b => 1,
                0xa4 | 0xa6 | 0xb4 | 0xb6 | 0xc4 | 0xe4 | 0x84 | 0x86 | 0x94 | 0x96 | 0x64 | 0x74 | 0x24 | 0x34 | 0xc6 | 0xe6 | 0xd6 | 0xf6 => 2,
                _ => 3,
            },
        },
    }
}

/// Whether op can change register r ('A', 'X', 'Y'), or leaves the code
/// path (then the walk gives up).
fn spoils(op: u8, r: u8) -> bool {
    // jumps, branches, returns, interrupts, block moves, stack pulls of anything
    if matches!(op, 0x00 | 0x02 | 0x10 | 0x30 | 0x50 | 0x70 | 0x80 | 0x82 | 0x90 | 0xb0 | 0xd0 | 0xf0 | 0x20 | 0x22 | 0x4c | 0x5c | 0x6c | 0x7c | 0xdc | 0xfc | 0x40 | 0x60 | 0x6b | 0x44 | 0x54 | 0xdb | 0xcb | 0x28 | 0xfb) {
        return true;
    }
    match r {
        b'A' => {
            let g = op >> 5;
            let alu = matches!(op & 0x1f, 0x01 | 0x03 | 0x05 | 0x07 | 0x09 | 0x0d | 0x0f | 0x11 | 0x12 | 0x13 | 0x15 | 0x17 | 0x19 | 0x1d | 0x1f);
            (alu && matches!(g, 0 | 1 | 2 | 3 | 5 | 7)) || matches!(op, 0x0a | 0x2a | 0x4a | 0x6a | 0x1a | 0x3a | 0x8a | 0x98 | 0x68 | 0x7b | 0x3b | 0xeb)
        }
        b'X' => matches!(op, 0xa2 | 0xa6 | 0xae | 0xb6 | 0xbe | 0xe8 | 0xca | 0xaa | 0xba | 0xbb | 0xfa),
        _ => matches!(op, 0xa0 | 0xa4 | 0xac | 0xb4 | 0xbc | 0xc8 | 0x88 | 0xa8 | 0x9b | 0x7a),
    }
}

/// An immediate load of value (width bytes) into register r, from which the
/// code runs straight to the store without changing r. Returns the file
/// offset of the immediate operand.
fn find_immediate(rom: &Rom, store: u32, r: u8, value: u32, width: usize) -> Option<(usize, usize)> {
    let op = match r {
        b'A' => 0xa9,
        b'X' => 0xa2,
        _ => 0xa0,
    };
    let bank = store & 0xff0000;
    let at = |a: u32| bank | (a & 0xffff);
    // nearest first
    for back in 2..=32u32 {
        let a = at((store & 0xffff).wrapping_sub(back));
        if read(rom, a) != Some(op) {
            continue;
        }
        if !(0..width).all(|i| read(rom, at(a + 1 + i as u32)) == Some((value >> (8 * i)) as u8)) {
            continue;
        }
        // the load's operand might be 16 bits with the value in the low byte (8-bit
        // value stored from a 16-bit register) — try the widths that fit
        // an 8-bit value (the bank) may come from a 16-bit load whose high byte goes to the next register
        let widths: &[bool] = if width == 2 { &[true] } else { &[false, true] };
        for &reg16 in widths {
            for other16 in [false, true] {
                let (mut m16, mut x16) = if r == b'A' { (reg16, other16) } else { (other16, reg16) };
                let mut pc = a;
                let mut ok = false;
                for _ in 0..16 {
                    let Some(o) = read(rom, pc) else { break };
                    let len = op_len(o, m16, x16) as u32;
                    if pc != a && spoils(o, r) {
                        break;
                    }
                    if o == 0xc2 || o == 0xe2 {
                        let v = read(rom, at(pc + 1)).unwrap_or(0);
                        let set = o == 0xc2;
                        if v & 0x20 != 0 {
                            m16 = set;
                        }
                        if v & 0x10 != 0 {
                            x16 = set;
                        }
                        // the register's width changed between the load and the store
                        if (r == b'A' && v & 0x20 != 0) || (r != b'A' && v & 0x10 != 0) {
                            if (r == b'A' && m16 != reg16) || (r != b'A' && x16 != reg16) {
                                break;
                            }
                        }
                    }
                    pc = at(pc + len);
                    if pc == store {
                        ok = true;
                        break;
                    }
                    if (pc & 0xffff) > (store & 0xffff) {
                        break;
                    }
                }
                if ok {
                    return rom.bus_to_off(at(a + 1)).map(|o| (o, back as usize));
                }
            }
        }
    }
    None
}

fn find_all(hay: &[u8], pat: &[u8]) -> Vec<usize> {
    let mut out = vec![];
    if pat.is_empty() || pat.len() > hay.len() {
        return out;
    }
    for i in 0..=hay.len() - pat.len() {
        if hay[i] == pat[0] && &hay[i..i + pat.len()] == pat {
            out.push(i);
            if out.len() > 64 {
                break;
            }
        }
    }
    out
}

/// The changes that make transfer rec read new_src (bus address) and move
/// new_size bytes. size_too: the size must change as well.
pub fn repoint(rom: &Rom, rec: &DmaRecord, new_src: u32, new_size: u32, size_too: bool) -> Result<Vec<Patch>, String> {
    let wants = [
        Want { reg: 2, value: rec.src & 0xffff, width: 2, new: new_src & 0xffff, name: "source address" },
        Want { reg: 4, value: rec.src >> 16, width: 1, new: new_src >> 16, name: "source bank" },
        Want { reg: 5, value: rec.size & 0xffff, width: 2, new: new_size & 0xffff, name: "size" },
    ];
    let mut patches = vec![];
    let mut missing: Vec<&Want> = vec![];
    for w in &wants {
        if w.value == w.new {
            continue; // nothing to change (same bank, same size)
        }
        if w.reg == 5 && !size_too {
            continue;
        }
        let pc = rec.reg_pc[w.reg];
        let mut done = false;
        if pc >= 0 {
            let pc = pc as u32;
            // 16-bit store of the address (one store wrote $43x2 and $43x3) or two 8-bit ones
            if let Some((store, r)) = find_store(rom, pc, rec.ch, w.reg) {
                let wide = w.width == 2 && rec.reg_pc[w.reg + 1] == rec.reg_pc[w.reg];
                if wide || w.width == 1 {
                    if let Some((off, _)) = find_immediate(rom, store, r, w.value, w.width) {
                        let mut new = vec![];
                        for i in 0..w.width {
                            new.push((w.new >> (8 * i)) as u8);
                        }
                        patches.push(Patch { off, old: rom.data[off..off + w.width].to_vec(), new, what: format!("{}: immediate load before the store at {}", w.name, bus(store)) });
                        done = true;
                    }
                } else if w.width == 2 {
                    // two 8-bit stores: low byte then high byte, each from its own load
                    let pc_hi = rec.reg_pc[w.reg + 1];
                    let lo = find_immediate(rom, store, r, w.value & 0xff, 1);
                    let hi = if pc_hi >= 0 {
                        find_store(rom, pc_hi as u32, rec.ch, w.reg + 1).and_then(|(s2, r2)| find_immediate(rom, s2, r2, w.value >> 8, 1))
                    } else {
                        None
                    };
                    if let (Some((lo, _)), Some((hi, _))) = (lo, hi) {
                        patches.push(Patch { off: lo, old: vec![rom.data[lo]], new: vec![w.new as u8], what: format!("{} (low byte): immediate load", w.name) });
                        patches.push(Patch { off: hi, old: vec![rom.data[hi]], new: vec![(w.new >> 8) as u8], what: format!("{} (high byte): immediate load", w.name) });
                        done = true;
                    }
                }
            }
        }
        if !done {
            missing.push(w);
        }
    }
    if missing.is_empty() {
        return Ok(patches);
    }
    // the rest from a table: the 24-bit address (low, high, bank) is in the ROM
    let ptr = [(rec.src & 0xff) as u8, (rec.src >> 8 & 0xff) as u8, (rec.src >> 16) as u8];
    let hits = find_all(&rom.data, &ptr);
    let need_addr = missing.iter().any(|w| w.reg == 2 || w.reg == 4);
    let need_size = missing.iter().any(|w| w.reg == 5);
    if hits.len() == 1 {
        let at = hits[0];
        if need_addr {
            let nb = [(new_src & 0xff) as u8, (new_src >> 8 & 0xff) as u8, (new_src >> 16) as u8];
            patches.push(Patch { off: at, old: rom.data[at..at + 3].to_vec(), new: nb.to_vec(), what: format!("source address in a table at ROM 0x{at:06X}") });
        }
        if need_size {
            let sz = [(rec.size & 0xff) as u8, (rec.size >> 8 & 0xff) as u8];
            // the size next to the address (after it, or before it)
            let near: Vec<usize> = (at.saturating_sub(4)..(at + 9).min(rom.data.len() - 1))
                .filter(|&o| (o + 2 <= at || o >= at + 3) && rom.data[o] == sz[0] && rom.data[o + 1] == sz[1])
                .collect();
            if near.len() == 1 {
                let o = near[0];
                patches.push(Patch { off: o, old: rom.data[o..o + 2].to_vec(), new: vec![(new_size & 0xff) as u8, (new_size >> 8) as u8], what: format!("size in the table at ROM 0x{o:06X}") });
            } else {
                return Err(format!("found the source address in a table at ROM 0x{at:06X}, but not the size of the transfer next to it"));
            }
        }
        return Ok(patches);
    }
    let names: Vec<&str> = missing.iter().map(|w| w.name).collect();
    if hits.is_empty() {
        Err(format!(
            "couldn't find where the game keeps the transfer's {} (the code at {} doesn't load it as a number, and {} isn't written in the ROM as a 24-bit address)",
            names.join(" and "),
            bus(rec.pc),
            bus(rec.src)
        ))
    } else {
        Err(format!(
            "the transfer's {} could be any of {} places in the ROM ({}); not changing any of them",
            names.join(" and "),
            hits.len(),
            hits.iter().take(8).map(|h| format!("0x{h:06X}")).collect::<Vec<_>>().join(", ")
        ))
    }
}
