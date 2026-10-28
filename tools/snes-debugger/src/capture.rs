//! A capture RetroPixels saved for the debugger: capture.json and the memory
//! and who-wrote-it files (docs/snes-debugger.md).

use serde::Deserialize;
use std::path::{Path, PathBuf};

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase")]
pub struct DmaRecord {
    pub id: i64,
    pub ch: u8,
    pub ctrl: u8,
    pub bbad: u8,
    pub src: u32,
    pub size: u32,
    pub vaddr: i32,
    pub vmain: i32,
    pub cgaddr: i32,
    pub wmadd: i32,
    pub pc: u32,
    pub reg_pc: [i64; 7],
    pub first: i64,
    pub last: i64,
    pub count: i64,
    pub line: i64,
}

#[derive(Deserialize, Clone, Debug, Default)]
pub struct Ppu {
    pub inidisp: u32,
    pub obsel: u32,
    pub bgmode: u32,
    pub mosaic: u32,
    pub bgsc: [u32; 4],
    pub bgnba: [u32; 4],
    pub hofs: [u32; 4],
    pub vofs: [u32; 4],
    pub tm: u32,
    pub ts: u32,
    pub setini: u32,
    pub cgwsel: u32,
}

#[derive(Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase")]
pub struct Meta {
    pub format: String,
    pub version: u32,
    pub frame: i64,
    pub title: String,
    pub map: String,
    pub rom_size: usize,
    pub ppu: Ppu,
    pub records: Vec<DmaRecord>,
}

/// Who wrote a byte (the *_src.bin files).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Writer {
    Never,
    Dma(u32),
    Hdma,
    Untracked,
    Cpu(u32),
}

impl Writer {
    pub fn from_code(v: i32) -> Writer {
        match v {
            v if v >= 0 => Writer::Dma(v as u32),
            -1 => Writer::Never,
            -3 => Writer::Hdma,
            v if v <= -16 => Writer::Cpu((-16 - v) as u32),
            _ => Writer::Untracked,
        }
    }
}

pub struct Capture {
    pub dir: PathBuf,
    pub meta: Meta,
    pub vram: Vec<u8>,
    pub cgram: Vec<u8>,
    pub oam: Vec<u8>,
    pub wram: Vec<u8>,
    pub vram_src: Vec<i32>,
    pub vram_from: Vec<i32>,
    pub cgram_src: Vec<i32>,
    pub cgram_from: Vec<i32>,
    pub wram_src: Vec<i32>,
    pub wram_from: Vec<i32>,
}

fn read(dir: &Path, name: &str, len: usize) -> Result<Vec<u8>, String> {
    let b = std::fs::read(dir.join(name)).map_err(|e| format!("{name}: {e}"))?;
    if b.len() != len {
        return Err(format!("{name} is {} bytes, not {len}", b.len()));
    }
    Ok(b)
}

fn read32(dir: &Path, name: &str, n: usize) -> Result<Vec<i32>, String> {
    let b = read(dir, name, n * 4)?;
    Ok(b.chunks_exact(4).map(|c| i32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect())
}

impl Capture {
    pub fn load(dir: &Path) -> Result<Capture, String> {
        let js = std::fs::read(dir.join("capture.json")).map_err(|e| format!("{}: not a capture ({e})", dir.display()))?;
        let meta: Meta = serde_json::from_slice(&js).map_err(|e| format!("capture.json: {e}"))?;
        if meta.format != "retropixels-snes-capture" {
            return Err("capture.json isn't a RetroPixels SNES capture".into());
        }
        Ok(Capture {
            dir: dir.to_path_buf(),
            vram: read(dir, "vram.bin", 0x10000)?,
            cgram: read(dir, "cgram.bin", 512)?,
            oam: read(dir, "oam.bin", 544)?,
            wram: read(dir, "wram.bin", 0x20000)?,
            vram_src: read32(dir, "vram_src.bin", 0x10000)?,
            vram_from: read32(dir, "vram_from.bin", 0x10000)?,
            cgram_src: read32(dir, "cgram_src.bin", 512)?,
            cgram_from: read32(dir, "cgram_from.bin", 512)?,
            wram_src: read32(dir, "wram_src.bin", 0x20000)?,
            wram_from: read32(dir, "wram_from.bin", 0x20000)?,
            meta,
        })
    }

    pub fn record(&self, id: u32) -> Option<&DmaRecord> {
        self.meta.records.iter().find(|r| r.id == id as i64)
    }

    pub fn vram_word(&self, w: usize) -> u16 {
        let w = w & 0x7fff;
        self.vram[w * 2] as u16 | (self.vram[w * 2 + 1] as u16) << 8
    }

    pub fn color(&self, i: usize) -> u16 {
        let i = i & 0xff;
        (self.cgram[i * 2] as u16 | (self.cgram[i * 2 + 1] as u16) << 8) & 0x7fff
    }

    /// The ROM next to the capture: the session's rom.sfc (two folders up)
    /// or a rom.sfc in the capture's own folder.
    pub fn default_rom(&self) -> Option<PathBuf> {
        let own = self.dir.join("rom.sfc");
        if own.exists() {
            return Some(own);
        }
        let s = self.dir.parent()?.parent()?.join("rom.sfc");
        s.exists().then_some(s)
    }
}

/// The WRAM address (0..0x1ffff) an A-bus address reads, if it's WRAM.
pub fn wram_addr(a: u32) -> Option<usize> {
    let bank = (a >> 16) & 0xff;
    let off = (a & 0xffff) as usize;
    if bank == 0x7e || bank == 0x7f {
        return Some(((bank as usize & 1) << 16) | off);
    }
    if bank & 0x40 == 0 && off < 0x2000 {
        return Some(off);
    }
    None
}
