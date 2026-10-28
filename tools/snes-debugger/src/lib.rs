//! RetroPixels SNES Debugger: reads the captures RetroPixels saves while a
//! SNES game plays (VRAM, CGRAM, WRAM and who wrote each byte), finds where a
//! background's tiles, tilemap and palettes are in the ROM, and swaps them for
//! a picture.

pub mod analyze;
pub mod capture;
pub mod gfx;
pub mod quantize;
pub mod repoint;
pub mod rom;
pub mod session;
pub mod swap;
