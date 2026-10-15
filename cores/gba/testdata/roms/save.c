// Save-memory test ROM: EEPROM (through DMA 3 and the CPU), Flash (ID, erase,
// program, banks) and SRAM, driven by the buttons; what it reads back is drawn
// on the screen so a wrong read shows in the picture.
#include "common.h"
#ifndef TAG
#define TAG "SRAM_V113"
#endif
__attribute__((used)) const char save_tag[] = TAG;

#define DMA3SAD REG32(0x040000d4)
#define DMA3DAD REG32(0x040000d8)
#define DMA3CNT REG32(0x040000dc)
static u16 ebuf[100] __attribute__((section(".ewram")));
static int line = 0;
static void show(u32 v) { bits_row((line / 160) * 34, line % 160, v, 0x7fff, 0x0010); line = (line + 1) % 320; }

#if defined(EEPROM)
#define ABITS (EEPROM == 512 ? 6 : 14)
static void ee_dma(int n, int to_ee) {
  if (to_ee) { DMA3SAD = (u32)ebuf; DMA3DAD = 0x0d000000; }
  else { DMA3SAD = 0x0d000000; DMA3DAD = (u32)ebuf; }
  DMA3CNT = n | 0x80000000;
}
static void ee_write(int addr, u32 lo, u32 hi) {
  int k = 0;
  ebuf[k++] = 1; ebuf[k++] = 0;
  for (int i = ABITS - 1; i >= 0; i--) ebuf[k++] = (addr >> i) & 1;
  for (int i = 31; i >= 0; i--) ebuf[k++] = (hi >> i) & 1;
  for (int i = 31; i >= 0; i--) ebuf[k++] = (lo >> i) & 1;
  ebuf[k++] = 0;
  ee_dma(k, 1);
  // wait until ready (reads 1)
  for (int i = 0; i < 10 && !(REG16(0x0d000000) & 1); i++) ;
}
static void ee_read(int addr, u32 *lo, u32 *hi) {
  int k = 0;
  ebuf[k++] = 1; ebuf[k++] = 1;
  for (int i = ABITS - 1; i >= 0; i--) ebuf[k++] = (addr >> i) & 1;
  ebuf[k++] = 0;
  ee_dma(k, 1);
  ee_dma(68, 0);
  u32 h = 0, l = 0;
  for (int i = 0; i < 32; i++) h = (h << 1) | (ebuf[4 + i] & 1);
  for (int i = 0; i < 32; i++) l = (l << 1) | (ebuf[36 + i] & 1);
  *lo = l; *hi = h;
}
// a read request sent with CPU stores, bits read with CPU loads
static void ee_read_cpu(int addr, u32 *lo) {
  volatile u16 *e = (volatile u16 *)0x0d000000;
  *e = 1; *e = 1;
  for (int i = ABITS - 1; i >= 0; i--) *e = (addr >> i) & 1;
  *e = 0;
  u32 l = 0;
  for (int i = 0; i < 68; i++) l = (l << 1) | (*e & 1);
  *lo = l;
}
static void step(u32 k, u32 f) {
  int addr = rnd() & (EEPROM == 512 ? 0x3f : 0x3ff);
  if (f < 3) addr = f;
  if ((k & 1) || f < 3) { u32 a = rnd(), b = rnd(); ee_write(addr, a, b); show(a); }
  if (k & 2) { u32 lo, hi; ee_read(addr, &lo, &hi); show(lo); show(hi); }
  if (k & 0x100) { u32 lo; ee_read_cpu(addr, &lo); show(lo); }
  if ((k & 0x300) == 0x300) { // odd lengths and junk bits
    ebuf[0] = 0; ebuf[1] = 1; ebuf[2] = 1; ee_dma(3, 1);
    ee_dma(5, 0); show(ebuf[0] | ebuf[1] << 1 | ebuf[4] << 4);
  }
}
#elif defined(FLASH)
#define F8(a) REG8(0x0e000000 + (a))
static void cmd(u8 c) { F8(0x5555) = 0xaa; F8(0x2aaa) = 0x55; F8(0x5555) = c; }
static void step(u32 k, u32 f) {
  if (f == 1 || (k & 0x30) == 0x30) { cmd(0x90); show(F8(0) | F8(1) << 8 | F8(2) << 16); cmd(0xf0); }
#if FLASH == 128
  if (k & 0x80) { cmd(0xb0); F8(0) = (rnd() >> 7) & 1; }
#endif
  if (k & 1) { int a = rnd() & 0xffff; u8 v = rnd(); cmd(0xa0); F8(a) = v; show(F8(a) | a << 8); }
  if (k & 2) { int a = rnd() & 0xf000; cmd(0x80); cmd(0x30 & 0); F8(0x5555) = 0xaa; F8(0x2aaa) = 0x55; F8(a) = 0x30; show(F8(a + 5)); }
  if ((k & 0x300) == 0x300 && (rnd() & 7) == 0) { cmd(0x80); cmd(0x10); show(F8(0x1234)); }
  if (k & 0x40) { F8(0x5555) = 0xaa; F8(0x1234) = 0x55; F8(0x10) = 0x77; } // broken sequence
  if (k & 0x10) { u32 s = 0; for (int i = 0; i < 64; i++) s = s * 31 + F8((rnd() & 0xffff)); show(s); }
  if (k & 0x100) { u16 h = REG16(0x0e000000 + (rnd() & 0xfffe)); u32 w = REG32(0x0e000000 + (rnd() & 0xfffc)); show(h ^ w); }
}
#else
static void step(u32 k, u32 f) {
  if (k & 1) { for (int i = 0; i < 16; i++) { int a = rnd() & 0x7fff; REG8(0x0e000000 + a) = rnd(); } }
  if (k & 2) { u32 s = 0; for (int i = 0; i < 64; i++) s = s * 31 + REG8(0x0e000000 + (rnd() & 0xffff)); show(s); }
  if (k & 0x100) { REG16(0x0e000000 + (rnd() & 0xfffe)) = rnd(); REG32(0x0e000000 + (rnd() & 0xfffc)) = rnd(); }
  if (k & 0x200) { show(REG16(0x0e000000 + (rnd() & 0x7ffe)) | REG32(0x0e000000 + (rnd() & 0x7ffc)) << 16); }
}
#endif

int main(void) {
  DISPCNT = 3 | 0x400;
  irq_init(1);
  WAITCNT = 0x4317;
  u32 f = 0;
  for (;;) {
    vblank_wait();
    u32 k = keys();
    rng += k;
    step(k, f);
    show(f);
    f++;
  }
}
