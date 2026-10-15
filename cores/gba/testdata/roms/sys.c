// System test ROM: timers (prescalers, cascade, interrupts, reading them
// while they run), DMA (all start timings, repeat, word size, address
// control, reload, BIOS-area sources, interrupts), serial port with nothing
// attached, keypad interrupts, HALTCNT, wait states and prefetch, open bus,
// code running from work RAM, odd-sized writes to video memory, reads past the
// end of the ROM and of unmapped areas. Everything read is drawn on screen.
#include "common.h"
#define TMD(i) REG16(0x04000100 + 4 * (i))
#define TMC(i) REG16(0x04000102 + 4 * (i))
#define DSAD(i) REG32(0x040000b0 + 12 * (i))
#define DDAD(i) REG32(0x040000b4 + 12 * (i))
#define DCNT(i) REG32(0x040000b8 + 12 * (i))
#define SIOCNT REG16(0x04000128)
#define RCNT REG16(0x04000134)
static int line;
static void show(u32 v) { bits_row(120 + (line / 160) * 40, line % 160, v, 0x7fff, 0x4010); line = (line + 1) % 320; }
static u32 buf[256] __attribute__((section(".ewram")));
static u32 buf2[256] __attribute__((section(".ewram")));

// small routines copied to work RAM and run there
__attribute__((target("arm"), noinline)) static u32 arm_sum(u32 *p, int n) { u32 s = 0; for (int i = 0; i < n; i++) s += p[i] ^ (s << 3); return s; }
__attribute__((noinline)) static u32 th_sum(u32 *p, int n) { u32 s = 7; for (int i = 0; i < n; i++) s = s * 5 + p[i]; return s; }
static u16 ramcode[512] __attribute__((section(".ewram"), aligned(4)));
static u16 iramcode[256] __attribute__((aligned(4)));

static u32 vcount_irqs;
static void hook(u32 f) { if (f & 0x78) vcount_irqs += f; if (f & 0x80) SIOCNT = 0; }

int main(void) {
  DISPCNT = 3 | 0x400;
  irq_hook = hook;
  irq_init(1 | 0x78 | 0xf00 | 0x80 | 0x1000);
  // copy the routines (both are position-independent leaf code)
  { u16 *s = (u16 *)((u32)arm_sum & ~1); for (int i = 0; i < 512; i++) ramcode[i] = s[i]; }
  { u16 *s = (u16 *)((u32)th_sum & ~1); for (int i = 0; i < 256; i++) iramcode[i] = s[i]; }
  u32 (*ram_arm)(u32 *, int) = (void *)(u32)ramcode;
  u32 (*ram_th)(u32 *, int) = (void *)((u32)iramcode | 1);
  u32 f = 0, prev = 0;
  for (;;) {
    vblank_wait();
    u32 k = keys(), nk = k & ~prev;
    prev = k;
    rng += k;
    u32 r = rnd();
    // timers
    if ((nk & 1) || (f & 31) == 0) for (int i = 0; i < 4; i++) { TMC(i) = 0; TMD(i) = rnd(); TMC(i) = 0x80 | (rnd() & 0x47); }
    if (nk & 2) { int i = r & 3; TMC(i) = 0; } // stop one
    if (k & 0x100) { int i = r & 3; TMC(i) = TMC(i) ^ (r & 0x47); } // change a running one
    show(TMD(0) | TMD(1) << 16); show(TMD(2) | TMD(3) << 16);
    for (int i = 0; i < 4; i++) { u16 a = TMD(i); for (volatile int w = 0; w < (r & 15); w++) ; show(a | TMD(i) << 16); }
    // DMA
    for (int i = 0; i < 256; i++) buf[i] = rnd();
    {
      int ch = (r >> 4) & 3;
      u32 ctl = 0x8000 | (rnd() & 0x77e0);       // enable + random timing / size / control / irq / repeat
      if ((ctl & 0x3000) == 0x3000) ctl &= ~0x3000;
      u32 srcs[4] = {(u32)buf, 0x00000100, 0x08000000 + (r & 0xfff0), 0x06000000 + (r & 0x3ff0)};
      u32 dsts[4] = {(u32)buf2, 0x06004000 + (r & 0x3f0), 0x0e000010, 0x07000000};
      DCNT(ch) = 0;
      DSAD(ch) = srcs[(r >> 8) & 3] + 0x80; DDAD(ch) = dsts[(r >> 10) & 3] + 0x80;
      DCNT(ch) = ((r >> 12) & 0x3f) | ctl << 16;
      show(DCNT(ch)); show(buf2[r & 255]);
      if (k & 0x200) { DCNT(ch) = 0; }
    }
    // serial port with no cable
    if (k & 4) { RCNT = (r & 1) ? 0x8000 : 0; SIOCNT = (r >> 1) & 0x7fff; SIOCNT |= 0x80; show(SIOCNT | REG16(0x04000120) << 16); show(REG32(0x04000124)); }
    // keypad interrupt conditions
    KEYCNT = (r & 0xc3ff) | 0x4000;
    // wait states and prefetch
    if (nk & 8) WAITCNT = rnd() & 0x5fff;
    show(WAITCNT);
    // open bus and unmapped areas
    show(REG32(0x00000000 + (r & 0x3ffc))); show(REG32(0x00004000 + (r & 0xff0))); show(REG32(0x01000000)); show(REG32(0x10000000 + (r & 0xff0)));
    show(REG16(0x08000000 + 0x1000000 - 2 + (r & 0x10))); show(REG32(0x09ff0000 + (r & 0xfffc)));
    show(REG32(0x04000400 + (r & 0xfc))); show(REG16(0x0600ff00 + 0x10000 * ((r >> 3) & 1)) | REG16(0x06018000 + (r & 0x3ffe)) << 16);
    show(REG8(0x04000000 + (r & 0x3ff)) | REG16(0x04000000 + (r & 0x3fe)) << 8);
    // byte writes to video memory, OAM, palette
    REG8(0x06000000 + (r & 0x17fff)) = r >> 24; REG8(0x07000000 + (r & 0x3ff)) = r; REG8(0x05000000 + (r & 0x3ff)) = r >> 8;
    REG16(0x06018000 + (r & 0x7ffe)) = r;
    show(REG32(0x05000000 + (r & 0x3fc)) ^ REG32(0x07000000 + (r & 0x3fc)));
    // code in work RAM
    show(ram_arm(buf, 64)); show(ram_th(buf, 64));
    // HALTCNT
    if (k & 0x10) { REG8(0x04000301) = 0; }
    if ((k & 0x30) == 0x30 && (r & 7) == 0) { REG8(0x04000301) = 0x80; }
    show(irq_flags); show(vcount_irqs);
    f++;
  }
}
