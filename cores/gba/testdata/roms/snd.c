// Sound test ROM: the four PSG channels (sweep, envelopes, lengths, duty,
// wave RAM banks and 64-sample mode, both noise widths), the mixer and
// master switches, and the two DMA sound FIFOs fed by DMA 1/2 on timers 0/1
// and by direct CPU writes. The buttons pick what plays.
#include "common.h"
#define SND(o) REG16(0x04000060 + (o))
#define SOUNDCNT_L REG16(0x04000080)
#define SOUNDCNT_H REG16(0x04000082)
#define SOUNDCNT_X REG16(0x04000084)
#define SOUNDBIAS REG16(0x04000088)
#define WAVE ((volatile u16 *)0x04000090)
#define TM0D REG16(0x04000100)
#define TM0C REG16(0x04000102)
#define TM1D REG16(0x04000104)
#define TM1C REG16(0x04000106)
#define DMA1SAD REG32(0x040000bc)
#define DMA1DAD REG32(0x040000c0)
#define DMA1CNT REG32(0x040000c4)
#define DMA2SAD REG32(0x040000c8)
#define DMA2DAD REG32(0x040000cc)
#define DMA2CNT REG32(0x040000d0)

static s8 sampA[4096] __attribute__((section(".ewram"), aligned(4)));
static s8 sampB[4096] __attribute__((section(".ewram"), aligned(4)));

static void start_dma(int t0, int t1) {
  DMA1CNT = 0; DMA2CNT = 0;
  DMA1SAD = (u32)sampA; DMA1DAD = 0x040000a0; DMA1CNT = 0xb6400000 | 4; // repeat, 32-bit, fifo timing, src inc
  DMA2SAD = (u32)sampB; DMA2DAD = 0x040000a4; DMA2CNT = 0xb6400000 | 4;
  TM0C = 0; TM1C = 0;
  TM0D = 0x10000 - t0; TM1D = 0x10000 - t1;
  TM0C = 0x80; TM1C = 0x80;
}

int main(void) {
  irq_init(1 | 0x200 | 0x400); // vblank + DMA1/2 done
  DISPCNT = 3 | 0x400;
  for (int i = 0; i < 4096; i++) { sampA[i] = (s8)(i * 3); sampB[i] = (s8)(rnd() >> 3); }
  SOUNDCNT_X = 0x80;
  SOUNDCNT_L = 0xff77;
  SOUNDCNT_H = 0x0b0e | 0x3000 | 0x0400;
  for (int i = 0; i < 8; i++) WAVE[i] = 0x1032 * (i + 1) ^ 0x8ac1;
  SND(0x10) = 0x40; // wave: bank 1 selected for playing, write bank 0
  for (int i = 0; i < 8; i++) WAVE[i] = 0xfedc - i * 0x1111;
  SND(0x10) = 0x80;
  start_dma(380, 760);
  u32 f = 0, prev = 0;
  for (;;) {
    vblank_wait();
    u32 k = keys(), nk = k & ~prev;
    prev = k;
    rng += k;
    u32 r = rnd();
    if (nk & 1) { SND(0x00) = r & 0x7f; SND(0x02) = (r >> 8) & 0xffff; SND(0x04) = 0x8000 | ((r >> 4) & 0x47ff); }
    if (nk & 2) { SND(0x08) = (r >> 3) & 0xffff; SND(0x0c) = 0x8000 | ((r >> 9) & 0x47ff); }
    if (nk & 0x100) { // wave
      SND(0x10) = 0x80 | (r & 0x60); SND(0x12) = (r >> 8) & 0xe0ff; SND(0x14) = 0x8000 | ((r >> 16) & 0x47ff);
      for (int i = 0; i < 8; i++) WAVE[i] ^= r >> i;
    }
    if (nk & 0x200) { SND(0x18) = (r >> 2) & 0xff3f; SND(0x1c) = 0x8000 | ((r >> 12) & 0x40ff); }
    if (nk & 0x10) SOUNDCNT_L = r;
    if (nk & 0x20) SOUNDCNT_H = (r & 0x330f) | 0x0800 | ((r & 0x10) ? 0x8000 : 0);
    if ((nk & 0x40) && (r & 3) == 0) { SOUNDCNT_X = 0; SOUNDCNT_X = 0x80; } // master off and on
    if (nk & 0x80) start_dma(200 + (r & 1023), 300 + ((r >> 10) & 2047));
    if (k & 4) { REG32(0x040000a0) = r; REG16(0x040000a4) = r >> 16; REG8(0x040000a6) = r >> 3; } // CPU feeds the FIFOs
    if (nk & 8) { TM0C = 0x80 | (r & 3); TM1C = 0x80 | 4 * ((r >> 2) & 1) | ((r >> 4) & 1); }
    if ((f & 63) == 0) { SND(0x04) = 0xc000 | (f & 0x7ff); SND(0x18) = 0x0f00 | 0xc000; SND(0x1c) = 0xc0f3; }
    if ((f & 127) == 64) { SND(0x0c) = 0x8000 | (f * 7 & 0x7ff); SND(0x14) = 0x8000 | (f * 3 & 0x7ff); }
    bits_row(0, f % 160, SOUNDCNT_X | REG16(0x04000100) << 16, 0x7fff, 0);
    bits_row(40, f % 160, REG16(0x04000090 + (f & 14)) | SND(0x08) << 16, 0x7fff, 0);
    bits_row(80, f % 160, irq_flags, 0x7fff, 0);
    f++;
  }
}
