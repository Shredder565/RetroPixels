// RetroPixels SNES Debugger. With no command it opens its window (RetroPixels
// starts it that way when a SNES game starts); the commands do the same work
// from a terminal.
#![cfg_attr(all(windows, feature = "gui"), windows_subsystem = "windows")]

use snesdbg::analyze;
use snesdbg::capture::Capture;
use snesdbg::gfx::Image;
use snesdbg::rom::{Map, Rom};
use snesdbg::session;
use snesdbg::swap::{self, Options};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Duration;

#[cfg(feature = "gui")]
mod gui;

const USAGE: &str = "RetroPixels SNES Debugger

  retropixels-snes-debugger [--base DIR]
      the window (RetroPixels opens it with SNES games)
  retropixels-snes-debugger capture [--base DIR]
      asks the SNES game playing in RetroPixels for a capture; prints its folder
  retropixels-snes-debugger analyze CAPTURE [--rom ROM]
      the background layers, and where their tiles, tilemap and palettes are in the ROM
  retropixels-snes-debugger export CAPTURE --bg N --out PICTURE.png
      draws background layer N (1-4) as a PNG
  retropixels-snes-debugger swap CAPTURE --bg N --image PICTURE.png --out PATCHED.sfc
          [--rom ROM] [--keep-palette] [--dither] [--no-grow] [--no-expand] [--preview P.png]
      puts the picture in background layer N and writes the patched ROM

CAPTURE is a capture folder (sessions\\<game>\\captures\\<id> in the base folder);
ROM defaults to the game's rom.sfc in the session folder.";

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

fn flag(args: &[String], name: &str) -> bool {
    args.iter().any(|a| a == name)
}

fn load(args: &[String]) -> Result<(Capture, Rom), String> {
    let dir = args.get(1).filter(|a| !a.starts_with("--")).ok_or("which capture folder?")?;
    let cap = Capture::load(Path::new(dir))?;
    let rom_path = match arg(args, "--rom") {
        Some(r) => PathBuf::from(r),
        None => cap.default_rom().ok_or("no rom.sfc next to the capture: give the ROM with --rom")?,
    };
    let mut rom = Rom::load(&rom_path)?;
    if let Some(m) = Map::parse(&cap.meta.map) {
        rom = rom.with_map(m);
    }
    if cap.meta.rom_size != 0 && cap.meta.rom_size != rom.data.len() {
        return Err(format!("{} is {} bytes, but the game in the capture was {} bytes: is it the same game?", rom_path.display(), rom.data.len(), cap.meta.rom_size));
    }
    Ok((cap, rom))
}

fn layer(cap: &Capture, args: &[String]) -> Result<analyze::Layer, String> {
    let n: usize = arg(args, "--bg").ok_or("which layer? --bg 1, 2, 3 or 4")?.parse().map_err(|_| "--bg is a number, 1-4")?;
    analyze::layers(cap).into_iter().find(|l| l.bg + 1 == n).ok_or_else(|| format!("there's no BG{n} in this capture's mode ({})", cap.meta.ppu.bgmode & 7))
}

fn run(args: &[String]) -> Result<(), String> {
    match args.first().map(|s| s.as_str()) {
        Some("capture") => {
            let base = arg(args, "--base").map(PathBuf::from).unwrap_or_else(session::default_base);
            let a = session::active(&base).ok_or("no SNES game is playing in RetroPixels")?;
            let id = session::request_capture(Path::new(&a.session))?;
            let dir = session::wait_capture(Path::new(&a.session), &id, Duration::from_secs(10)).ok_or("RetroPixels didn't answer (is the game paused on an error, or the debugger turned off?)")?;
            println!("{}", dir.display());
            Ok(())
        }
        Some("analyze") => {
            let (cap, rom) = load(args)?;
            print!("{}", analyze::report(&cap, &rom));
            Ok(())
        }
        Some("export") => {
            let (cap, _) = load(args)?;
            let l = layer(&cap, args)?;
            let out = arg(args, "--out").ok_or("--out PICTURE.png")?;
            analyze::render(&cap, &l).save_png(Path::new(&out))
        }
        Some("swap") => {
            let (cap, rom) = load(args)?;
            let l = layer(&cap, args)?;
            let img = Image::load_png(Path::new(&arg(args, "--image").ok_or("--image PICTURE.png")?))?;
            let out = PathBuf::from(arg(args, "--out").ok_or("--out PATCHED.sfc")?);
            let opt = Options { new_palette: !flag(args, "--keep-palette"), dither: flag(args, "--dither"), grow: !flag(args, "--no-grow"), expand_rom: !flag(args, "--no-expand") };
            let r = swap::swap(&cap, &rom, &l, &img, &opt)?;
            for line in &r.log {
                println!("{line}");
            }
            r.rom.save(&out)?;
            if let Some(p) = arg(args, "--preview") {
                r.preview.save_png(Path::new(&p))?;
            }
            println!("Wrote {}", out.display());
            Ok(())
        }
        Some("help") | Some("--help") | Some("-h") => {
            println!("{USAGE}");
            Ok(())
        }
        _ => {
            #[cfg(feature = "gui")]
            {
                let base = arg(args, "--base").map(PathBuf::from).unwrap_or_else(session::default_base);
                gui::run(base)
            }
            #[cfg(not(feature = "gui"))]
            {
                println!("{USAGE}");
                Ok(())
            }
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("retropixels-snes-debugger: {e}");
            ExitCode::FAILURE
        }
    }
}
