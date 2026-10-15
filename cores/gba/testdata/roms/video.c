// Video test ROM: modes 0-5, text and affine backgrounds (sizes, wrap, flips,
// 16/256 colours, mosaic), sprites (all shapes and sizes, affine and double
// size, semi-transparent, OBJ window, mosaic, 1D/2D mapping, bitmap-mode tile
// limit), windows, blending, forced blank, H-blank DMA scrolling and
// mid-frame writes from the V-count interrupt. The buttons and a random
// generator keep changing the set-up.
#include "common.h"
#define BGCNT(k) REG16(0x04000008 + 2 * (k))
#define BGHOFS(k) REG16(0x04000010 + 4 * (k))
#define BGVOFS(k) REG16(0x04000012 + 4 * (k))
#define DMA0SAD REG32(0x040000b0)
#define DMA0DAD REG32(0x040000b4)
#define DMA0CNT REG32(0x040000b8)
static u16 scroll[161] __attribute__((section(".ewram")));
static const s16 sinq[16] = {0, 25, 49, 71, 90, 106, 118, 125, 128, 125, 118, 106, 90, 71, 49, 25};
static s16 sn(int a) { a &= 31; return a < 16 ? sinq[a] : -sinq[a - 16]; }

static void vcount_hook(u32 f) {
  if (f & 4) { // V-count: move the affine reference point and colours mid-frame
    REG32(0x04000028) = rnd() & 0xfffff; REG16(0x04000020) = 0x100 + (rnd() & 0x3f);
    PAL[0] = rnd();
    DISPSTAT = (DISPSTAT & 0xff) | ((rnd() % 160) << 8);
  }
}

static void fill_tiles(void) {
  volatile u32 *v = (volatile u32 *)0x06000000;
  for (int i = 0; i < 0x18000 / 4; i++) { u32 x = i * 0x01010101u; v[i] = (x ^ (x >> 3) ^ (i >> 4)) * 0x9e3779b1u; }
  for (int i = 0; i < 512; i++) PAL[i] = rnd();
  for (int i = 0; i < 512; i++) OAM[i] = rnd();
}

static void randomize_sprites(int n) {
  for (int j = 0; j < n; j++) {
    int i = rnd() & 127;
    u32 r = rnd();
    OAM[i * 4] = (r & 0xff) | (r & 0xff00 & ~0x0000) ;
    OAM[i * 4 + 1] = (r >> 16);
    OAM[i * 4 + 2] = rnd();
  }
  for (int m = 0; m < 32; m++) {
    int a = rnd() & 31, s = 0x80 + (rnd() & 0x1ff);
    OAM[m * 16 + 3] = (sn(a + 8) * s) >> 7; OAM[m * 16 + 7] = (-sn(a) * s) >> 7;
    OAM[m * 16 + 11] = (sn(a) * s) >> 7; OAM[m * 16 + 15] = (sn(a + 8) * s) >> 7;
  }
}

int main(void) {
  fill_tiles();
  irq_hook = vcount_hook;
  irq_init(1 | 4);
  DISPSTAT |= 0x20 | (80 << 8);
  DISPCNT = 0x1f00;
  for (int i = 0; i < 161; i++) scroll[i] = i * 3;
  u32 f = 0;
  for (;;) {
    vblank_wait();
    u32 k = keys();
    rng += k;
    u32 r = rnd();
    // display set-up: every 16 frames, or on a new button press
    if ((f & 15) == 0 || (k & 0x100)) {
      u32 mode = (k & 0x200) ? (r & 7) % 6 : (f >> 4) % 6;
      DISPCNT = mode | (rnd() & ((r & 0x10000) ? 0xff70 : 0x1f70)) | 0x1000 | ((r & 0x3f00) ? 0 : 0x80 & rnd());
      for (int b = 0; b < 4; b++) BGCNT(b) = rnd();
      REG16(0x04000048) = rnd(); REG16(0x0400004a) = rnd();
      REG16(0x04000040) = rnd(); REG16(0x04000042) = rnd(); REG16(0x04000044) = rnd(); REG16(0x04000046) = rnd();
      REG16(0x0400004c) = (k & 2) ? rnd() : 0;
      REG16(0x04000050) = rnd(); REG16(0x04000052) = rnd() & 0x1f1f; REG16(0x04000054) = rnd() & 0x1f;
    }
    if (k & 1) randomize_sprites(8);
    if (k & 0x10) for (int i = 0; i < 16; i++) VRAM[rnd() % 0xc000] = rnd();
    if (k & 0x20) for (int i = 0; i < 16; i++) PAL[rnd() & 511] = rnd();
    // scrolling and the affine backgrounds
    for (int b = 0; b < 4; b++) { BGHOFS(b) = f * (b + 1); BGVOFS(b) = f >> b; }
    int a = f >> 2, s = 0x80 + ((f * 3) & 0xff);
    REG16(0x04000020) = (sn(a + 8) * s) >> 7; REG16(0x04000022) = (-sn(a) * s) >> 7;
    REG16(0x04000024) = (sn(a) * s) >> 7; REG16(0x04000026) = (sn(a + 8) * s) >> 7;
    REG32(0x04000028) = (f * 64) & 0xfffffff; REG32(0x0400002c) = -(s32)(f * 32);
    REG16(0x04000030) = 0x100; REG16(0x04000036) = 0x100 - (f & 0x7f);
    REG32(0x04000038) = (k & 4) ? 0x8000000 - f * 100 : f * 300; REG32(0x0400003c) = f * 50;
    // H-blank DMA scrolls BG0 line by line
    DMA0CNT = 0;
    if (k & 8) {
      for (int i = 0; i < 161; i++) scroll[i] = (sn(i + f) + 128) >> 2;
      DMA0SAD = (u32)scroll; DMA0DAD = 0x04000010; DMA0CNT = 0xa2400001; // H-blank, repeat, dest fixed (16-bit)
    }
    if (k & 0x40) DISPCNT ^= 0x10; // page flip
    if (k & 0x80) DISPCNT ^= 0x40; // 1D / 2D sprite mapping
    f++;
  }
}
