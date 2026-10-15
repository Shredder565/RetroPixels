// RetroPixels — Archipelago support, the page's side. Original code.
//
// Archipelago's own clients (the SNI Client for Super Nintendo games, the
// BizHawk Client for most others) read and write the running game's memory.
// RetroPixels.exe answers them (see archipelago.go) and asks this page, over
// /api/ap/ws, to carry out small batches of memory operations. A batch runs
// between two frames, all at once.
//
// Memory is named the way BizHawk names it ("WRAM", "CartRAM", "EWRAM",
// "System Bus"...), because that's what the BizHawk Client's game handlers
// ask for. The SNI Client's SNES addresses are turned into SNES domains
// ("WRAM", "CARTRAM", "CARTROM"...) by RetroPixels.exe.
"use strict";

(function (global) {
  // ---- memory domains: { size(), rd(i), wr(i, v) } ----
  const bytes = (get, size) => ({
    size: size || (() => { const a = get(); return a ? a.length : 0; }),
    rd: (i) => get()[i], wr: (i, v) => { get()[i] = v; },
  });
  // 16-bit memory seen as little-endian bytes (SNES VRAM and colours)
  const words = (get, size) => ({
    size: () => size,
    rd: (i) => (get()[i >> 1] >> ((i & 1) * 8)) & 0xff,
    wr: (i, v) => { const a = get(), k = i >> 1; a[k] = i & 1 ? (a[k] & 0xff) | (v << 8) : (a[k] & 0xff00) | v; },
  });

  // ---- Game Boy / Color (named like BizHawk's Gambatte core) ----
  function gbCartRam(c, a) {
    const cart = c.cart, ram = cart.ram;
    if (cart.ramOn) return cart.ramRead(a);
    return ram && ram.length ? ram[((cart.ramOff || 0) + (a & 0x1fff)) % ram.length] : 0xff;
  }
  function gbPeek(c, a) {
    if (a < 0x8000) return c.cart.romRead(a);
    if (a < 0xa000) return c.vram[c.vbank * 0x2000 + (a & 0x1fff)];
    if (a < 0xc000) return gbCartRam(c, a);
    if (a < 0xd000) return c.wram[a & 0xfff];
    if (a < 0xe000) return c.wram[c.wbank * 0x1000 + (a & 0xfff)];
    if (a < 0xfe00) return gbPeek(c, a - 0x2000);
    if (a < 0xfea0) return c.oam[a - 0xfe00];
    if (a < 0xff00) return 0xff;
    if (a < 0xff80) return c.ioRead(a);
    if (a < 0xffff) return c.hram[a - 0xff80];
    return c.ie;
  }
  function gbPoke(c, a, v) {
    if (a < 0x8000) return; // cartridge registers: leave alone
    if (a < 0xa000) { c.vram[c.vbank * 0x2000 + (a & 0x1fff)] = v; return; }
    if (a < 0xc000) {
      const cart = c.cart, ram = cart.ram;
      if (cart.ramOn) cart.ramWrite(a, v);
      else if (ram && ram.length) ram[((cart.ramOff || 0) + (a & 0x1fff)) % ram.length] = v;
      return;
    }
    if (a < 0xd000) { c.wram[a & 0xfff] = v; return; }
    if (a < 0xe000) { c.wram[c.wbank * 0x1000 + (a & 0xfff)] = v; return; }
    if (a < 0xfe00) { gbPoke(c, a - 0x2000, v); return; }
    if (a < 0xfea0) { c.oam[a - 0xfe00] = v; return; }
    if (a < 0xff00) return;
    if (a < 0xff80) { c.write(a, v); return; }
    if (a < 0xffff) { c.hram[a - 0xff80] = v; return; }
    c.ie = v;
  }
  function gbDomains(c) {
    const d = {
      ROM: bytes(() => c.cart.rom),
      WRAM: bytes(() => c.wram, () => (c.cgb ? 0x8000 : 0x2000)),
      VRAM: bytes(() => c.vram, () => (c.cgb ? 0x4000 : 0x2000)),
      OAM: bytes(() => c.oam),
      HRAM: bytes(() => c.hram),
      "System Bus": { size: () => 0x10000, rd: (a) => gbPeek(c, a), wr: (a, v) => gbPoke(c, a, v) },
    };
    if (c.cart.ram && c.cart.ram.length) d.CartRAM = bytes(() => c.cart.ram);
    return d;
  }

  // ---- Game Boy Advance (named like BizHawk's mGBA core) ----
  function gbaPeek(c, a) {
    a >>>= 0;
    switch (a >>> 24) {
      case 0: return a < 0x4000 ? c.bios[a] : 0;
      case 2: return c.ewram[a & 0x3ffff];
      case 3: return c.iwram[a & 0x7fff];
      case 4: return (a & 0xffffff) < 0x400 ? (c.ioRead16(a & 0x3fe) >> ((a & 1) * 8)) & 0xff : 0;
      case 5: return c.pal[a & 0x3ff];
      case 6: return c.vram[c.vaddr(a)];
      case 7: return c.oam[a & 0x3ff];
      case 8: case 9: case 10: case 11: case 12: case 13: { const o = a & 0x1ffffff; return o < c.romSize ? c.rom[o] : 0; }
      case 14: case 15: { const s = c.save.data; return s[(a & 0xffff) % s.length]; }
    }
    return 0;
  }
  function gbaPoke(c, a, v) {
    a >>>= 0;
    switch (a >>> 24) {
      case 2: c.ewram[a & 0x3ffff] = v; return;
      case 3: c.iwram[a & 0x7fff] = v; return;
      case 5: c.pal[a & 0x3ff] = v; return;
      case 6: c.vram[c.vaddr(a)] = v; return;
      case 7: c.oam[a & 0x3ff] = v; return;
    }
  }
  function gbaDomains(c) {
    return {
      ROM: bytes(() => c.rom, () => c.romSize),
      EWRAM: bytes(() => c.ewram),
      IWRAM: bytes(() => c.iwram),
      BIOS: bytes(() => c.bios),
      PALRAM: bytes(() => c.pal),
      VRAM: bytes(() => c.vram),
      OAM: bytes(() => c.oam),
      SRAM: { size: () => c.save.data.length, rd: (i) => c.save.data[i], wr: (i, v) => { c.save.data[i] = v; c.save.dirty = true; } },
      "System Bus": { size: () => 0x10000000, rd: (a) => gbaPeek(c, a), wr: (a, v) => gbaPoke(c, a, v) },
    };
  }

  // ---- NES (named like BizHawk's NesHawk core) ----
  function nesDomains(c) {
    const m = () => c.mapper;
    const d = {
      RAM: bytes(() => c.ram),
      "PRG ROM": bytes(() => m().prg),
      WRAM: bytes(() => m().prgRam),
      "CIRAM (nametables)": bytes(() => c.ppu.vram, () => 0x800),
      OAM: bytes(() => c.ppu.oam),
      PALRAM: bytes(() => c.ppu.palRam),
      "System Bus": {
        size: () => 0x10000,
        rd: (a) => {
          if (a < 0x2000) return c.ram[a & 0x7ff];
          if (a < 0x4020) return 0; // PPU / sound / controller registers: reading them changes them
          const v = m().cpuRead(a); return v < 0 ? 0 : v & 0xff;
        },
        wr: (a, v) => {
          if (a < 0x2000) c.ram[a & 0x7ff] = v;
          else if (a >= 0x6000 && a < 0x8000 && m().prgRam) m().prgRam[a & 0x1fff] = v;
        },
      },
    };
    if (c.battery) d["Battery RAM"] = d.WRAM;
    d[m().chrRam ? "CHR VRAM" : "CHR VROM"] = bytes(() => m().chr);
    return d;
  }

  // ---- Super Nintendo (named like BizHawk's bsnes core) ----
  function snesDomains(c) {
    // the cartridge's RAM: the Super FX chip's, the SA-1's BW-RAM, or plain save RAM
    const ram = () => (c.gsu ? c.gsu.ram : c.sa1 ? c.sa1.bw : c.cart.sram);
    const ramSize = () => (c.gsu ? c.gsu.ram.length : c.sa1 ? c.sa1.bw.length : c.cart.sramSize);
    const d = {
      WRAM: bytes(() => c.wram),
      CARTROM: bytes(() => c.cart.rom, () => c.cart.size),
      CARTRAM: { size: ramSize, rd: (i) => ram()[i], wr: (i, v) => { ram()[i] = v; c.cart.sramDirty = true; } },
      VRAM: words(() => c.ppu.vram, 0x10000),
      CGRAM: words(() => c.ppu.cgram, 0x200),
      OAM: bytes(() => c.ppu.oam),
      APURAM: bytes(() => c.apu.ram),
      "System Bus": {
        size: () => 0x1000000,
        rd: (a) => c.peekA(a) & 0xff,
        wr: (a, v) => {
          const bank = a >>> 16, off = a & 0xffff;
          if (bank === 0x7e || bank === 0x7f) c.wram[a & 0x1ffff] = v;
          else if (!(bank & 0x40) && off < 0x2000) c.wram[off] = v;
        },
      },
    };
    if (c.sa1) d["SA1 IRAM"] = bytes(() => c.sa1.iram);
    return d;
  }

  // ---- Genesis (named like BizHawk's Genplus-gx core) ----
  function genesisDomains(c) {
    return {
      "68K RAM": bytes(() => c.ram),
      "Z80 RAM": bytes(() => c.zram),
      "MD CART": bytes(() => c.rom, () => c.romSize),
      SRAM: bytes(() => c.cart.sram),
      "M68K BUS": {
        size: () => 0x1000000,
        rd: (a) => (a >= 0xe00000 ? c.ram[a & 0xffff] : a < c.romSize && a < 0x400000 ? c.rom[a] : 0),
        wr: (a, v) => { if (a >= 0xe00000) c.ram[a & 0xffff] = v; },
      },
    };
  }

  // ---- Master System ----
  function smsDomains(c) {
    return {
      "Main RAM": bytes(() => c.ram),
      ROM: bytes(() => c.rom),
      "Cart (Volatile) RAM": bytes(() => c.cartRam),
    };
  }

  const SYSTEMS = {
    gb: { id: (c) => (c.cgb ? "GBC" : "GB"), domains: gbDomains, client: "BizHawk Client" },
    gba: { id: () => "GBA", domains: gbaDomains, client: "BizHawk Client" },
    nes: { id: () => "NES", domains: nesDomains, client: "BizHawk Client" },
    snes: { id: () => "SNES", domains: snesDomains, client: "SNI Client" },
    segaMD: { id: () => "GEN", domains: genesisDomains, client: "BizHawk Client" },
    sms: { id: () => "SMS", domains: smsDomains, client: "BizHawk Client" },
    n64: { id: () => "N64", domains: null, client: "BizHawk Client" }, // memory is in RetroPixels.exe
  };

  // ---- base64 ----
  function toB64(u8) {
    let s = "";
    for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  }
  function fromB64(s) {
    if (!s) return new Uint8Array(0);
    const t = atob(s), out = new Uint8Array(t.length);
    for (let i = 0; i < t.length; i++) out[i] = t.charCodeAt(i);
    return out;
  }

  // ---- the connection ----
  class Bridge {
    constructor(player, opts) {
      this.player = player; this.opts = opts || {};
      this.system = SYSTEMS[player.system];
      this.domains = this.system && this.system.domains ? this.system.domains(player.core) : {};
      this.msgs = []; this.msgTimer = null;
      this.closed = false; this.retry = 500;
      this.status = null;
      this.ready = this.describe().then(() => this.connect());
    }

    async describe() {
      const p = this.player, c = p.core, rom = this.opts.rom;
      let hash = "", data = rom;
      // like BizHawk, an iNES file's hash leaves out its 16-byte header (and trainer)
      if (p.system === "nes" && rom && rom.length > 16 && rom[0] === 0x4e && rom[1] === 0x45 && rom[2] === 0x53 && rom[3] === 0x1a) {
        data = rom.subarray(16 + (rom[6] & 4 ? 512 : 0));
      }
      try {
        const d = await crypto.subtle.digest("SHA-1", data);
        hash = [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("").toUpperCase();
      } catch (e) { hash = String(rom.length); }
      this.game = { core: p.system, system: this.system.id(c), title: this.opts.title || "", hash, n64: c.session || "" };
    }

    connect() {
      if (this.closed) return;
      const desk = global.RETRO_DESKTOP;
      const ws = this.ws = new WebSocket(`ws://${location.host}/api/ap/ws?token=${encodeURIComponent(desk.token)}`);
      ws.onopen = () => { this.retry = 500; ws.send(JSON.stringify({ ev: "game", game: this.game })); };
      ws.onmessage = (e) => {
        let m;
        try { m = JSON.parse(e.data); } catch (x) { return; }
        if (m.ops) ws.send(JSON.stringify({ id: m.id, res: this.run(m.ops) }));
        else if (m.ev === "status") { this.status = m; if (this.opts.onStatus) this.opts.onStatus(m); }
        else if (m.ev === "msg") this.message(m.text);
      };
      ws.onclose = () => {
        if (this.closed) return;
        setTimeout(() => this.connect(), this.retry);
        this.retry = Math.min(this.retry * 2, 8000);
      };
    }

    domain(name) {
      const d = this.domains[name];
      if (!d) throw new Error(`no memory domain named "${name}"`);
      return d;
    }

    // Carries out one batch. A failed guard stops it.
    run(ops) {
      const out = [];
      for (const o of ops) {
        let r = {};
        try {
          const a = o.a || 0;
          switch (o.t) {
            case "size": r.n = this.domain(o.d).size(); break;
            case "read": {
              const d = this.domain(o.d), size = d.size(), n = o.n || 0;
              if (a < 0 || n < 0 || (!o.pad && a + n > size)) throw new Error(`address out of range for ${o.d}`);
              const v = new Uint8Array(n);
              for (let i = 0; i < n && a + i < size; i++) v[i] = d.rd(a + i) & 0xff;
              r.v = toB64(v);
              break;
            }
            case "write": case "guard": {
              const d = this.domain(o.d), size = d.size(), v = fromB64(o.v);
              if (a < 0 || (!o.pad && a + v.length > size)) throw new Error(`address out of range for ${o.d}`);
              if (o.t === "write") { for (let i = 0; i < v.length && a + i < size; i++) d.wr(a + i, v[i]); }
              else { r.ok = true; for (let i = 0; i < v.length; i++) if ((a + i < size ? d.rd(a + i) & 0xff : 0) !== v[i]) { r.ok = false; break; } }
              break;
            }
            case "msg": this.message(o.text); break;
            case "lock": this.player.holdUntil = performance.now() + 2000; break; // like BizHawk's connector: at most 2 s
            case "unlock": this.player.holdUntil = 0; break;
            default: throw new Error("unknown operation " + o.t);
          }
        } catch (e) { r = { err: e.message }; }
        out.push(r);
        if (o.t === "guard" && !r.ok) break;
      }
      return out;
    }

    // Messages from the client (items found, chat...): one at a time on the game screen.
    message(text) {
      if (!text) return;
      this.msgs.push(String(text));
      if (this.msgs.length > 20) this.msgs.shift();
      const next = () => {
        const t = this.msgs.shift();
        if (t === undefined || this.closed) { this.msgTimer = null; return; }
        this.player.flash(t);
        this.msgTimer = setTimeout(next, 1700);
      };
      if (!this.msgTimer) next();
    }

    close() {
      this.closed = true;
      clearTimeout(this.msgTimer);
      if (this.ws) { try { this.ws.send(JSON.stringify({ ev: "nogame" })); } catch (e) { /* already gone */ } this.ws.close(); }
    }
  }

  global.RPArchipelago = {
    supports: (system) => !!SYSTEMS[system],
    clientFor: (system) => (SYSTEMS[system] ? SYSTEMS[system].client : null),
    attach: (player, opts) => new Bridge(player, opts),
  };
})(window);
