// The web 68000 (web/emu/m68k.js) runs an A-line opcode holding a backward
// short branch as that branch only when branchHooks is on (see branchhook_test.go).
globalThis.window = globalThis; require(process.argv[2]);
const C = globalThis.RPM68K;
function run(hooks) {
  const m = new Uint8Array(0x10000);
  const w16 = (a, v) => { m[a] = v >> 8; m[a + 1] = v & 255; }, w32 = (a, v) => { w16(a, v >>> 16); w16(a + 2, v & 0xffff); };
  w32(0, 0x8000); w32(4, 0x100); w32(0x28, 0x200); w32(0x2c, 0x200);
  w16(0x100, 0x4e71); w16(0x102, 0xa0fc); w16(0x200, 0x4e71);
  const c = new C({ read8: a => m[a & 0xffff], read16: a => (m[a & 0xffff] << 8) | m[(a + 1) & 0xffff],
    write8: (a, v) => { m[a & 0xffff] = v; }, write16: (a, v) => w16(a & 0xffff, v), ack: () => 0 });
  c.branchHooks = hooks; c.reset();
  for (let i = 0; i < 3; i++) c.step();
  return c.pc.toString(16);
}
console.log("hooks:", run(true), "no hooks:", run(false));
