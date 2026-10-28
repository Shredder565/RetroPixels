// RetroPixels — connects a SNES game playing in RetroPixels.exe to the
// RetroPixels SNES Debugger (a separate program, tools/snes-debugger), which
// opens alongside the game. Original code.
//
// RetroPixels.exe (snesdbg.go) starts the debugger and keeps the session's
// files; this page turns on the SNES core's tracer (who wrote each byte of
// VRAM, CGRAM and WRAM, and the DMA transfers), asks every half second
// whether the debugger wants a capture, and sends it the capture when it does.
"use strict";

(function (global) {
  const enc = new TextEncoder();

  // "RPSC", u32 count, then per file: u16 name length, name, u32 size, bytes
  function pack(files) {
    const names = Object.keys(files);
    let size = 8;
    const parts = names.map((n) => { const nb = enc.encode(n); size += 2 + nb.length + 4 + files[n].length; return [nb, files[n]]; });
    const out = new Uint8Array(size), dv = new DataView(out.buffer);
    out.set(enc.encode("RPSC"), 0); dv.setUint32(4, names.length, true);
    let p = 8;
    for (const [nb, data] of parts) {
      dv.setUint16(p, nb.length, true); p += 2; out.set(nb, p); p += nb.length;
      dv.setUint32(p, data.length, true); p += 4; out.set(data, p); p += data.length;
    }
    return out;
  }

  // attach(player, { rom, name, token }) -> { close() } or null when the core can't trace
  function attach(player, opts) {
    const core = player.core;
    if (!core || !core.enableTrace) return null;
    core.enableTrace();
    const hdr = { "X-Retro-Token": opts.token };
    let id = null, timer = null, busy = false, closed = false;
    const post = (path, body) => fetch(path, { method: "POST", headers: Object.assign({ "Content-Type": "application/octet-stream" }, hdr), body });
    async function poll() {
      if (busy || closed || !id) return;
      busy = true;
      try {
        const res = await fetch("/api/snesdbg/poll?id=" + encodeURIComponent(id), { headers: hdr });
        if (!res.ok) return;
        const { pending } = await res.json();
        for (const req of pending || []) {
          if (player.destroyed || player.core !== core) return;
          const cap = core.debugCapture();
          const files = Object.assign({}, cap.files);
          files["capture.json"] = enc.encode(JSON.stringify(cap.meta, null, 1));
          try { files["screen.png"] = await player.screenshotPNG(); } catch (e) { /* no picture */ }
          await post("/api/snesdbg/capture?id=" + encodeURIComponent(id) + "&req=" + encodeURIComponent(req), pack(files));
          if (opts.onCapture) opts.onCapture(req);
        }
      } catch (e) { /* RetroPixels.exe went away; try again next time */ } finally { busy = false; }
    }
    (async () => {
      try {
        const res = await post("/api/snesdbg/start?name=" + encodeURIComponent(opts.name || "game") + "&title=" + encodeURIComponent((core.cart && core.cart.title) || ""), opts.rom);
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || res.status);
        id = data.id;
        if (opts.onStatus) opts.onStatus(data);
        timer = setInterval(poll, 500);
      } catch (e) { if (opts.onStatus) opts.onStatus({ error: String(e.message || e) }); }
    })();
    return {
      close() {
        closed = true;
        if (timer) clearInterval(timer);
        if (id) post("/api/snesdbg/end?id=" + encodeURIComponent(id), new Uint8Array(0)).catch(() => {});
      },
    };
  }

  global.RPSNESDebug = { attach, pack };
})(typeof window !== "undefined" ? window : globalThis);
