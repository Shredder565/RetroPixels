//! The debugger's window: follows the SNES game playing in RetroPixels, asks
//! it for captures, shows each background layer with where its data is in
//! the ROM, and swaps a layer for a picture.

use eframe::egui;
use snesdbg::analyze::{self, LayerInfo, Origins};
use snesdbg::capture::Capture;
use snesdbg::gfx::Image;
use snesdbg::rom::{Map, Rom};
use snesdbg::session::{self, Active};
use snesdbg::swap::{self, Options, SwapResult};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub fn run(base: PathBuf) -> Result<(), String> {
    let hb = base.clone();
    std::thread::spawn(move || loop {
        session::heartbeat(&hb);
        std::thread::sleep(Duration::from_millis(1000));
    });
    let opts = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_title("RetroPixels SNES Debugger").with_inner_size([1180.0, 800.0]).with_min_inner_size([760.0, 520.0]),
        ..Default::default()
    };
    eframe::run_native("RetroPixels SNES Debugger", opts, Box::new(move |cc| {
        cc.egui_ctx.set_visuals(egui::Visuals::dark());
        Ok(Box::new(App::new(base)))
    }))
    .map_err(|e| e.to_string())
}

struct LayerView {
    info: LayerInfo,
    image: Image,
    tex: Option<egui::TextureHandle>,
    lines: Vec<(String, Vec<String>)>,
}

struct Loaded {
    dir: PathBuf,
    cap: Capture,
    rom: Rom,
    rom_path: PathBuf,
    layers: Vec<LayerView>,
    screen: Option<(Image, Option<egui::TextureHandle>)>,
}

struct App {
    base: PathBuf,
    active: Option<Active>,
    /// the session shown: the game playing, or one picked by hand
    session: Option<PathBuf>,
    pinned: bool,
    captures: Vec<PathBuf>,
    polled: Instant,
    waiting: Option<(String, Instant)>,
    countdown: Option<Instant>,
    loaded: Option<Loaded>,
    layer: usize,
    picture: Option<(PathBuf, Image)>,
    opt: Options,
    result: Option<(SwapResult, Option<egui::TextureHandle>)>,
    patch_rom: Option<PathBuf>,
    status: String,
    want_swap: bool,
}

impl App {
    fn new(base: PathBuf) -> App {
        let mut a = App {
            base,
            active: None,
            session: None,
            pinned: false,
            captures: vec![],
            polled: Instant::now() - Duration::from_secs(5),
            waiting: None,
            countdown: None,
            loaded: None,
            layer: 0,
            picture: None,
            opt: Options { new_palette: true, dither: false, grow: true, expand_rom: true },
            result: None,
            patch_rom: None,
            status: String::new(),
            want_swap: false,
        };
        a.poll();
        // show the newest capture of the game straight away
        if let Some(last) = a.captures.last().cloned() {
            a.load(&last);
        }
        a
    }

    fn poll(&mut self) {
        self.polled = Instant::now();
        self.active = session::active(&self.base);
        if !self.pinned {
            let now = self.active.as_ref().map(|a| PathBuf::from(&a.session));
            if now.is_some() && now != self.session {
                self.session = now;
                self.loaded = None;
                self.result = None;
                self.patch_rom = None;
            }
        }
        let before = self.captures.len();
        self.captures = self.session.as_ref().map(|s| session::captures(s)).unwrap_or_default();
        if let Some((id, since)) = self.waiting.clone() {
            if let Some(dir) = self.captures.iter().find(|p| p.file_name().map(|n| n.to_string_lossy() == id).unwrap_or(false)).cloned() {
                self.waiting = None;
                self.load(&dir);
            } else if since.elapsed() > Duration::from_secs(8) {
                self.waiting = None;
                self.status = "RetroPixels didn't answer. Is the SNES game still playing, with \"Open the SNES Debugger with SNES games\" on?".into();
            }
        } else if self.captures.len() > before && before > 0 && self.loaded.is_none() {
            if let Some(last) = self.captures.last().cloned() {
                self.load(&last);
            }
        }
    }

    fn capture(&mut self) {
        let Some(a) = &self.active else {
            self.status = "No SNES game is playing in RetroPixels right now.".into();
            return;
        };
        let s = PathBuf::from(&a.session);
        if self.pinned && self.session.as_ref() != Some(&s) {
            self.pinned = false;
            self.session = Some(s.clone());
        }
        match session::request_capture(&s) {
            Ok(id) => {
                self.status = "Capturing…".into();
                self.waiting = Some((id, Instant::now()));
            }
            Err(e) => self.status = format!("Couldn't ask for a capture: {e}"),
        }
    }

    fn load(&mut self, dir: &Path) {
        let cap = match Capture::load(dir) {
            Ok(c) => c,
            Err(e) => {
                self.status = e;
                return;
            }
        };
        let rom_path = match self.patch_rom.clone().or_else(|| cap.default_rom()) {
            Some(p) => p,
            None => {
                self.status = "The capture has no ROM next to it; choose the game's ROM with \"ROM to patch\".".into();
                return;
            }
        };
        let mut rom = match Rom::load(&rom_path) {
            Ok(r) => r,
            Err(e) => {
                self.status = e;
                return;
            }
        };
        if let Some(m) = Map::parse(&cap.meta.map) {
            rom = rom.with_map(m);
        }
        let mut org = Origins::new(&cap, &rom);
        let mut layers = vec![];
        for l in analyze::layers(&cap) {
            let info = analyze::analyze_layer(&cap, &rom, &mut org, &l);
            let image = analyze::render(&cap, &l);
            let mut lines = vec![];
            for (title, segs, cg) in [("Tilemap", &info.map_segments, false), ("Tiles", &info.char_segments, false), ("Palettes", &info.pal_segments, true)] {
                let mut v: Vec<String> = segs.iter().take(40).map(|s| s.describe(&cap, &rom, cg)).collect();
                if segs.len() > 40 {
                    v.push(format!("… {} more", segs.len() - 40));
                }
                lines.push((title.to_string(), v));
            }
            layers.push(LayerView { info, image, tex: None, lines });
        }
        let screen = Image::load_png(&dir.join("screen.png")).ok().map(|i| (i, None));
        // the first layer that's on, unless the one picked before is still there
        if self.layer >= layers.len() || !(layers[self.layer].info.layer.on_main || layers[self.layer].info.layer.on_sub) {
            self.layer = layers.iter().position(|l| l.info.layer.on_main || l.info.layer.on_sub).unwrap_or(0);
        }
        self.status = format!("Capture at frame {} loaded.", cap.meta.frame);
        self.result = None;
        self.loaded = Some(Loaded { dir: dir.to_path_buf(), cap, rom, rom_path, layers, screen });
    }

    fn make_swap(&mut self) {
        let (Some(ld), Some((_, pic))) = (&self.loaded, &self.picture) else { return };
        let Some(lv) = ld.layers.get(self.layer) else { return };
        match swap::swap(&ld.cap, &ld.rom, &lv.info.layer, pic, &self.opt) {
            Ok(r) => {
                self.status = "Preview ready: save the patched ROM to keep it.".into();
                self.result = Some((r, None));
            }
            Err(e) => {
                self.status = e;
                self.result = None;
            }
        }
    }
}

fn texture(ctx: &egui::Context, name: &str, img: &Image) -> egui::TextureHandle {
    // transparent pixels over a checkerboard
    let mut px = Vec::with_capacity(img.w * img.h * 4);
    for y in 0..img.h {
        for x in 0..img.w {
            let c = img.get(x, y);
            if c[3] == 0 {
                let v = if (x / 8 + y / 8) % 2 == 0 { 40 } else { 56 };
                px.extend_from_slice(&[v, v, v, 255]);
            } else {
                px.extend_from_slice(&[c[0], c[1], c[2], 255]);
            }
        }
    }
    ctx.load_texture(name, egui::ColorImage::from_rgba_unmultiplied([img.w, img.h], &px), egui::TextureOptions::NEAREST)
}

fn fit(tex: &egui::TextureHandle, max: egui::Vec2) -> egui::Vec2 {
    let s = tex.size_vec2();
    let k = (max.x / s.x).min(max.y / s.y).max(0.25);
    let k = if k >= 1.0 { k.floor() } else { k };
    s * k
}

fn open_folder(p: &Path) {
    #[cfg(windows)]
    let _ = std::process::Command::new("explorer").arg(p).spawn();
    #[cfg(not(windows))]
    let _ = std::process::Command::new("xdg-open").arg(p).spawn();
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if self.polled.elapsed() > Duration::from_millis(400) {
            self.poll();
        }
        if let Some(t) = self.countdown {
            if t <= Instant::now() {
                self.countdown = None;
                self.capture();
            }
        }
        ctx.request_repaint_after(Duration::from_millis(250));
        if self.want_swap {
            self.want_swap = false;
            self.make_swap();
        }

        egui::TopBottomPanel::top("top").show(ctx, |ui| {
            ui.add_space(4.0);
            ui.horizontal_wrapped(|ui| {
                match &self.active {
                    Some(a) => {
                        ui.label(egui::RichText::new(format!("Playing: {}", a.name)).strong());
                        ui.label(format!("in {}", a.app));
                    }
                    None => {
                        ui.label(egui::RichText::new("No SNES game is playing in RetroPixels").strong());
                    }
                }
                ui.separator();
                let can = self.active.is_some() && self.waiting.is_none();
                if ui.add_enabled(can, egui::Button::new("📷 Capture now")).on_hover_text("Saves VRAM, CGRAM, WRAM and the DMA transfers from the game as it is now").clicked() {
                    self.capture();
                }
                let label = match self.countdown {
                    Some(t) => format!("⏱ Capturing in {:.0} s…", t.saturating_duration_since(Instant::now()).as_secs_f32().ceil()),
                    None => "⏱ Capture in 5 s".into(),
                };
                if ui.add_enabled(can && self.countdown.is_none(), egui::Button::new(label)).on_hover_text("Time to get back to the game and to the moment you want").clicked() {
                    self.countdown = Some(Instant::now() + Duration::from_secs(5));
                }
                ui.separator();
                if ui.button("📂 Open a capture…").clicked() {
                    if let Some(d) = rfd::FileDialog::new().set_title("Choose a capture folder (it has capture.json)").set_directory(self.base.join("sessions")).pick_folder() {
                        if let Some(s) = d.parent().and_then(|p| p.parent()) {
                            self.session = Some(s.to_path_buf());
                            self.pinned = true;
                        }
                        self.load(&d);
                        self.poll();
                    }
                }
                if let Some(s) = &self.session {
                    if ui.button("Session folder").clicked() {
                        open_folder(s);
                    }
                }
                if self.pinned && self.active.is_some() && ui.button("Follow the game playing").clicked() {
                    self.pinned = false;
                    self.poll();
                }
            });
            if !self.status.is_empty() {
                ui.label(egui::RichText::new(&self.status).weak());
            }
            ui.add_space(2.0);
        });

        egui::SidePanel::left("caps").resizable(true).default_width(170.0).show(ctx, |ui| {
            ui.heading("Captures");
            if let Some(s) = &self.session {
                if let Some(info) = session::session_info(s) {
                    ui.label(egui::RichText::new(if info.title.is_empty() { info.name } else { info.title }).weak());
                }
            }
            ui.separator();
            egui::ScrollArea::vertical().show(ui, |ui| {
                let mut pick = None;
                for (i, c) in self.captures.iter().enumerate().rev() {
                    let sel = self.loaded.as_ref().map(|l| &l.dir == c).unwrap_or(false);
                    let frame = std::fs::read(c.join("capture.json"))
                        .ok()
                        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok())
                        .and_then(|v| v["frame"].as_i64())
                        .unwrap_or(0);
                    if ui.selectable_label(sel, format!("#{}  frame {}", i + 1, frame)).clicked() {
                        pick = Some(c.clone());
                    }
                }
                if let Some(p) = pick {
                    self.load(&p);
                }
                if self.captures.is_empty() {
                    ui.label("None yet. Play to the moment you want (the first level, say) and press Capture.");
                }
            });
        });

        egui::CentralPanel::default().show(ctx, |ui| {
            let Some(ld) = self.loaded.as_mut() else {
                ui.vertical_centered(|ui| {
                    ui.add_space(80.0);
                    ui.heading("RetroPixels SNES Debugger");
                    ui.label("Start a SNES game in RetroPixels, play to the screen whose background you want to change, and press Capture now.");
                    ui.label("The debugger then shows each background layer and where its tiles, tilemap and colours are in the ROM, and can swap a layer for your own picture.");
                });
                return;
            };
            ui.horizontal(|ui| {
                ui.label(format!("{} · {} · Mode {} · frame {}", if ld.cap.meta.title.is_empty() { ld.rom.title() } else { ld.cap.meta.title.clone() }, ld.rom.map.name(), ld.cap.meta.ppu.bgmode & 7, ld.cap.meta.frame));
                if ui.small_button("Capture folder").clicked() {
                    open_folder(&ld.dir);
                }
            });
            ui.horizontal(|ui| {
                for (i, lv) in ld.layers.iter().enumerate() {
                    let l = &lv.info.layer;
                    let on = l.on_main || l.on_sub;
                    let txt = format!("{}{}", l.name(), if on { "" } else { " (off)" });
                    if ui.selectable_label(self.layer == i, txt).clicked() && self.layer != i {
                        self.layer = i;
                        self.result = None;
                    }
                }
            });
            ui.separator();
            let Some(lv) = ld.layers.get_mut(self.layer) else { return };
            let l = lv.info.layer.clone();
            if lv.tex.is_none() {
                lv.tex = Some(texture(ctx, &format!("layer{}", l.bg), &lv.image));
            }
            egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
                ui.horizontal_top(|ui| {
                    ui.vertical(|ui| {
                        ui.label(egui::RichText::new(format!("{} as the game has it", l.name())).strong());
                        let t = lv.tex.as_ref().unwrap();
                        ui.image((t.id(), fit(t, egui::vec2(512.0, 512.0))));
                        if ui.button("Save this layer as PNG…").clicked() {
                            if let Some(p) = rfd::FileDialog::new().add_filter("PNG picture", &["png"]).set_file_name(format!("{}.png", l.name())).save_file() {
                                if let Err(e) = lv.image.save_png(&p) {
                                    self.status = e;
                                }
                            }
                        }
                    });
                    if let Some((img, tex)) = ld.screen.as_mut() {
                        ui.vertical(|ui| {
                            ui.label(egui::RichText::new("The screen then").strong());
                            if tex.is_none() {
                                *tex = Some(texture(ctx, "screen", img));
                            }
                            let t = tex.as_ref().unwrap();
                            ui.image((t.id(), fit(t, egui::vec2(256.0, 240.0))));
                        });
                    }
                });
                ui.add_space(6.0);
                let pct = |(a, b): (usize, usize)| if b == 0 { 0 } else { a * 100 / b };
                ui.label(format!(
                    "{} bits per pixel · tilemap {}x{} at VRAM ${:04X} · tiles at ${:04X}{} · {} tiles used · palettes {:?}",
                    if l.bpp == 7 { 8 } else { l.bpp },
                    l.map_w,
                    l.map_h,
                    l.map_base,
                    l.char_base,
                    if l.tile16 { " (16x16)" } else { "" },
                    lv.info.tiles.len(),
                    lv.info.palettes
                ));
                ui.label(format!(
                    "In the ROM as they are: tilemap {}%, tiles {}%, palettes {}%",
                    pct(lv.info.traced[0]),
                    pct(lv.info.traced[1]),
                    pct(lv.info.traced[2])
                ));
                for n in &lv.info.notes {
                    ui.label(egui::RichText::new(format!("• {n}")).color(egui::Color32::from_rgb(230, 190, 90)));
                }
                for (title, v) in &lv.lines {
                    egui::CollapsingHeader::new(format!("{title}: where it came from")).default_open(title == "Tiles").show(ui, |ui| {
                        for s in v {
                            ui.label(egui::RichText::new(s).monospace().size(12.0));
                        }
                    });
                }
                ui.add_space(8.0);
                ui.separator();
                ui.heading(format!("Swap {} for a picture", l.name()));
                ui.label(format!("The picture is scaled to {}x{} (the whole tilemap; the screen shows 256x224 of it). Transparent pixels stay see-through.", l.px_w(), l.px_h()));
                ui.horizontal(|ui| {
                    if ui.button("🖼 Choose picture…").clicked() {
                        if let Some(p) = rfd::FileDialog::new().add_filter("PNG picture", &["png"]).pick_file() {
                            match Image::load_png(&p) {
                                Ok(img) => {
                                    self.picture = Some((p, img));
                                    self.result = None;
                                }
                                Err(e) => self.status = e,
                            }
                        }
                    }
                    if let Some((p, img)) = &self.picture {
                        ui.label(format!("{} ({}x{})", p.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default(), img.w, img.h));
                    }
                });
                let mut changed = false;
                changed |= ui.checkbox(&mut self.opt.new_palette, "New colours for the picture (written to the ROM); off: use the game's own colours").changed();
                changed |= ui.checkbox(&mut self.opt.dither, "Dither (smoother shading, but more different tiles)").changed();
                changed |= ui.checkbox(&mut self.opt.grow, "When the tiles don't fit: move them to free ROM space and repoint the game's transfer (off: merge similar tiles)").changed();
                changed |= ui.checkbox(&mut self.opt.expand_rom, "May make the ROM bigger when there's no free space").changed();
                if changed {
                    self.result = None;
                }
                ui.horizontal(|ui| {
                    ui.label(format!("ROM to patch: {}", ld.rom_path.display()));
                    if ui.small_button("Change…").clicked() {
                        if let Some(p) = rfd::FileDialog::new().add_filter("SNES ROM", &["sfc", "smc", "swc", "fig"]).pick_file() {
                            match Rom::load(&p) {
                                Ok(r) if r.data.len() == ld.rom.data.len() => {
                                    self.patch_rom = Some(p.clone());
                                    let r = match Map::parse(&ld.cap.meta.map) {
                                        Some(m) => r.with_map(m),
                                        None => r,
                                    };
                                    ld.rom = r;
                                    ld.rom_path = p;
                                    self.result = None;
                                }
                                Ok(_) => self.status = "That ROM isn't the same size as the game in the capture.".into(),
                                Err(e) => self.status = e,
                            }
                        }
                    }
                });

                let can = self.picture.is_some() && l.bpp != 7;
                if ui.add_enabled(can, egui::Button::new("▶ Make preview")).clicked() {
                    self.want_swap = true;
                }
                if let Some((r, tex)) = self.result.as_mut() {
                    ui.separator();
                    if tex.is_none() {
                        *tex = Some(texture(ctx, "preview", &r.preview));
                    }
                    let t = tex.as_ref().unwrap();
                    ui.horizontal_top(|ui| {
                        ui.image((t.id(), fit(t, egui::vec2(512.0, 512.0))));
                        ui.vertical(|ui| {
                            for line in &r.log {
                                ui.label(line);
                            }
                            ui.add_space(6.0);
                            if ui.button(egui::RichText::new("💾 Save patched ROM…").strong()).clicked() {
                                let name = ld
                                    .dir
                                    .parent()
                                    .and_then(|p| p.parent())
                                    .and_then(session::session_info)
                                    .map(|i| i.name)
                                    .filter(|n| !n.is_empty())
                                    .unwrap_or_else(|| "game".into());
                                if let Some(p) = rfd::FileDialog::new().add_filter("SNES ROM", &["sfc", "smc"]).set_file_name(format!("{name} (new background).sfc")).save_file() {
                                    match r.rom.save(&p) {
                                        Ok(()) => {
                                            self.status = format!("Saved {}. Open it in RetroPixels to play it; to swap more layers, choose it as the ROM to patch.", p.display());
                                        }
                                        Err(e) => self.status = e,
                                    }
                                }
                            }
                        });
                    });
                }
            });
        });
    }
}
