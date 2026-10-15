// BIOS-call test ROM: every call RetroPixels' BIOS handles, with random (and
// awkward) arguments picked by the buttons; results are drawn on the screen and
// decompression also goes straight into the visible picture.
#include "common.h"
static u32 o0, o1, o3;
#define CALL(n, a, b, c, d) do { register u32 r0 asm("r0") = (a); register u32 r1 asm("r1") = (b); register u32 r2 asm("r2") = (c); register u32 r3 asm("r3") = (d); \
  asm volatile("swi " #n : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) :: "memory", "r12", "lr"); o0 = r0; o1 = r1; o3 = r3; } while (0)
static int line;
static void show(u32 v) { bits_row(160 + (line / 160) * 40, line % 160, v, 0x7fff, 0x0200); line = (line + 1) % 320; }
static u8 src[0x2400] __attribute__((section(".ewram"), aligned(4)));
static u8 dst[0x4000] __attribute__((section(".ewram"), aligned(4)));
static u32 sum(const u8 *p, int n) { u32 s = 0; for (int i = 0; i < n; i++) s = s * 33 + p[i]; return s; }

__attribute__((target("arm"), noinline)) static u32 arm_div(u32 a, u32 b) {
  register u32 r0 asm("r0") = a; register u32 r1 asm("r1") = b;
  asm volatile("swi 0x060000" : "+r"(r0), "+r"(r1) :: "r2", "r3", "r12", "lr", "memory");
  return r0 ^ r1;
}

static u32 pick(void) {
  u32 r = rnd();
  switch (r & 7) { case 0: return 0; case 1: return 0xffffffff; case 2: return 0x80000000; case 3: return r >> 20; default: return rnd(); }
}

int main(void) {
  DISPCNT = 3 | 0x400;
  irq_init(1 | 0x1000);
  KEYCNT = 0x4000 | 0x3ff;
  u32 f = 0;
  for (;;) {
    if (f & 1) vblank_wait(); else { CALL(4, f & 2 ? 1 : 0, 1, 0, 0); }
    u32 k = keys();
    rng += k * 0x9e3779b9 + f;
    u32 a = pick(), b = pick();
    CALL(6, a, b, 0, 0); show(o0); show(o1 ^ o3);
    CALL(7, a, b, 0, 0); show(o0 ^ o1 ^ o3);
    show(arm_div(a, b));
    CALL(8, a, 0, 0, 0); show(o0);
    CALL(9, a, 0, 0, 0); show(o0);
    CALL(10, a, b, 0, 0); show(o0);
    CALL(13, 0, 0, 0, 0); show(o0);
    // affine set-up
    for (int i = 0; i < 40; i++) src[i] = rnd();
    CALL(14, (u32)src, (u32)dst, 2, 0); show(sum(dst, 32));
    CALL(15, (u32)src, (u32)dst, 3, (k & 1) ? 2 : 8); show(sum(dst, 48));
    CALL(15, (u32)src, 0x07000006, 4, 8); show(OAM[3] | OAM[7] << 16);
    // copies
    for (int i = 0; i < 256; i++) src[i] = rnd();
    u32 n = rnd() & 0x3f;
    CALL(11, (u32)src + (rnd() & 7), (u32)dst + (rnd() & 7), n | (rnd() & 0x5000000), 0); show(sum(dst, 300));
    CALL(12, (u32)src + (rnd() & 7), (u32)dst, (rnd() & 0x2f) | (rnd() & 0x1000000), 0); show(sum(dst, 300));
    CALL(11, 0x00000100, (u32)dst, 8, 0); show(sum(dst, 16)); // from the BIOS: refused
    // bit unpack
    {
      u8 *info = src + 0x300; u32 sw = 1 << (rnd() & 3), dw = 1 << (rnd() % 6);
      if (dw < sw) dw = sw;
      info[0] = 16; info[1] = 0; info[2] = sw; info[3] = dw; *(u32 *)(info + 4) = (rnd() & 0x8000000f);
      CALL(16, (u32)src, (u32)dst, (u32)info, 0); show(sum(dst, 16 * 8 * dw / sw / 8 + 4));
    }
    // decompression: random streams with sane headers, into memory and into the picture
    {
      u32 size = 64 + (rnd() & 0x3ff);
      for (int i = 4; i < 0x2400; i++) src[i] = rnd();
      int kind = (k >> 4) & 7 ? (rnd() & 7) : 7;
      u32 *h = (u32 *)src;
      u16 *vr = (u16 *)(0x06000000 + ((f * 640) % 0x12000));
      switch (kind) {
        case 0: *h = 0x10 | size << 8; CALL(17, (u32)src, (u32)dst, 0, 0); show(sum(dst, size)); break;
        case 1: *h = 0x10 | size << 8; CALL(18, (u32)src, (u32)vr, 0, 0); break;
        case 2: *h = 0x30 | size << 8; CALL(20, (u32)src, (u32)dst, 0, 0); show(sum(dst, size)); break;
        case 3: *h = 0x30 | size << 8; CALL(21, (u32)src, (u32)vr, 0, 0); break;
        case 4: *h = 0x20 | ((rnd() & 1) ? 4 : 8) | (size & ~3) << 8; src[4] = rnd() & 0x3f; CALL(19, (u32)src, (u32)dst, 0, 0); show(sum(dst, size)); break;
        case 5: *h = 0x81 | size << 8; CALL(22, (u32)src, (u32)dst, 0, 0); show(sum(dst, size)); break;
        case 6: *h = 0x81 | size << 8; CALL(23, (u32)src, (u32)vr, 0, 0); break;
        default: *h = 0x82 | size << 8; CALL(24, (u32)src, (u32)vr, 0, 0); break;
      }
    }
    // sound bias, MIDI key, multiboot, unknown calls
    CALL(25, rnd() & 1, 0, 0, 0); show(REG16(0x04000088));
    *(u32 *)(src + 4) = rnd() >> (rnd() & 15);
    CALL(31, (u32)src, rnd() & 0xff, rnd() & 0xff, 0); show(o0);
    CALL(37, 0, 0, 0, 0); show(o0);
    CALL(26, 0, 0, 0, 0); CALL(40, 0, 0, 0, 0); CALL(42, 0, 0, 0, 0);
    // sleeping
    if (k & 0x100) { CALL(2, 0, 0, 0, 0); }
    if ((k & 0x300) == 0x300) { CALL(39, 0, 0, rnd() & 0x80 ? 0 : 0x80, 0); }
    if ((k & 0x30) == 0x30 && (rnd() & 7) == 0) { CALL(3, 0, 0, 0, 0); }
    // RAM reset of some areas (never the work RAM this program uses)
    if ((k & 0xc0) == 0xc0) { CALL(1, (rnd() & 0xf4) | 0x80, 0, 0, 0); DISPCNT = 3 | 0x400; irq_init(1 | 0x1000); KEYCNT = 0x4000 | 0x3ff; }
    if ((k & 0x3c0) == 0x3c0 && (rnd() & 3) == 0) { REG8(0x03007ffa) = 0; CALL(0, 0, 0, 0, 0); }
    if ((k & 0x3c3) == 0x3c3) { CALL(38, 0, 0, 0, 0); }
    show(irq_flags ^ f << 16);
    f++;
  }
}
