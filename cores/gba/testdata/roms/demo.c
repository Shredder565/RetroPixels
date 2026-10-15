// A little game-like demo (original code): a tile-map world scrolling with
// parallax, a player sprite moved with the D-pad, A jumps (square-wave sweep),
// B fires (noise), L/R change the music speed, a software mixer streaming
// music through DMA sound A every frame (like real games' sound drivers), a
// bass line on the wave channel, and a high score kept in SRAM.
#include "common.h"
__attribute__((used)) const char save_tag[] = "SRAM_V113";
#define BGCNT(k) REG16(0x04000008 + 2 * (k))
#define BGHOFS(k) REG16(0x04000010 + 4 * (k))
#define BGVOFS(k) REG16(0x04000012 + 4 * (k))
#define SND(o) REG16(0x04000060 + (o))
#define DMA1SAD REG32(0x040000bc)
#define DMA1DAD REG32(0x040000c0)
#define DMA1CNT REG32(0x040000c4)
#define DMA0SAD REG32(0x040000b0)
#define DMA0DAD REG32(0x040000b4)
#define DMA0CNT REG32(0x040000b8)
#define TM0D REG16(0x04000100)
#define TM0C REG16(0x04000102)
#define RATE 13379
#define PERFRAME 224 // samples per frame at 13379 Hz (59.73 fps)
static s8 mixbuf[2][PERFRAME] __attribute__((section(".ewram"), aligned(4)));
static u16 hscroll[160] __attribute__((section(".ewram")));
static const u8 notes[32] = {0, 4, 7, 12, 7, 4, 0, 4, 5, 9, 12, 17, 12, 9, 5, 9, 7, 11, 14, 19, 14, 11, 7, 11, 0, 4, 7, 12, 16, 12, 7, 4};
static const u16 freq[24] = {262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494, 523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988};
static const u8 font_digits[10][5] = {{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
static u32 ph1, ph2, inc1, inc2, buf_i;
static int tempo = 8;

static void mix(u32 fr) {
  s8 *b = mixbuf[buf_i];
  int n = notes[(fr / tempo) & 31];
  inc1 = (freq[n] << 16) / RATE * 1;
  inc2 = (freq[(n + 7) % 24] << 15) / RATE;
  for (int i = 0; i < PERFRAME; i++) {
    ph1 += inc1; ph2 += inc2;
    int s = ((ph1 >> 15) & 1 ? 30 : -30) + (int)((ph2 >> 9) & 127) / 3 - 21;
    b[i] = s;
  }
  // restart the stream on the buffer just made
  DMA1CNT = 0;
  DMA1SAD = (u32)b; DMA1DAD = 0x040000a0; DMA1CNT = 0xb6400000 | 4;
  buf_i ^= 1;
}

static void draw_digit(int x, int y, int d) {
  for (int r = 0; r < 5; r++) for (int c = 0; c < 3; c++) {
    int tile = 64 + d; // digits live at tiles 64..73 (4bpp)
    (void)tile; (void)x; (void)y;
    if (font_digits[d][r] & (4 >> c)) {}
  }
}

int main(void) {
  // palettes
  for (int i = 0; i < 16; i++) { PAL[i] = (i * 2) | (i * 1) << 5 | (31 - i) << 10; PAL[16 + i] = (31 - i) | (i * 2) << 5 | 8 << 10; PAL[256 + i] = i ? (31 << 0) | (i * 2) << 5 : 0; }
  PAL[0] = 0x7ead;
  // tiles: 0 empty, 1-7 bricks/clouds patterns
  volatile u32 *ch = (volatile u32 *)0x06000000;
  for (int t = 1; t < 8; t++) for (int y = 0; y < 8; y++) {
    u32 row = 0;
    for (int x = 0; x < 8; x++) { int c = ((x + y * t) % 7 == 0) ? 15 : ((x ^ y) & t) + t; row |= (u32)(c & 15) << (x * 4); }
    ch[t * 8 + y] = row;
  }
  // digit tiles 64-73
  for (int d = 0; d < 10; d++) for (int y = 0; y < 8; y++) {
    u32 row = 0;
    if (y >= 1 && y <= 5) for (int x = 0; x < 3; x++) if (font_digits[d][y - 1] & (4 >> x)) { row |= 15u << ((x * 2 + 1) * 4); row |= 15u << ((x * 2 + 2) * 4); }
    ch[(64 + d) * 8 + y] = row;
  }
  // maps: BG0 ground at screen block 28, BG1 sky/clouds at 29, BG2 score at 30
  volatile u16 *m0 = (volatile u16 *)0x0600e000, *m1 = (volatile u16 *)0x0600e800, *m2 = (volatile u16 *)0x0600f000;
  for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
    m0[y * 32 + x] = y > 15 + ((x * 5) & 3) ? (1 + ((x + y) & 3)) | ((x & 4) ? 0x400 : 0) : 0;
    m1[y * 32 + x] = (y < 10 && ((x * 7 + y * 3) % 11) == 0) ? 5 | 0x1000 : 0;
    m2[y * 32 + x] = 0;
  }
  BGCNT(0) = 0 | 28 << 8 | 1; BGCNT(1) = 0 | 29 << 8 | 3; BGCNT(2) = 0 | 30 << 8 | 0;
  // player sprite: tiles at 0x10000 (tile 0..3, 16x16 1D)
  volatile u32 *oc = (volatile u32 *)0x06010000;
  for (int t = 0; t < 4; t++) for (int y = 0; y < 8; y++) {
    u32 row = 0;
    for (int x = 0; x < 8; x++) { int gx = (t & 1) * 8 + x - 8, gy = (t >> 1) * 8 + y - 8; int d = gx * gx + gy * gy; row |= (u32)(d < 40 ? 1 + (d >> 3) : 0) << (x * 4); }
    oc[t * 8 + y] = row;
  }
  // bullets: tile 4 (8x8)
  for (int y = 0; y < 8; y++) oc[4 * 8 + y] = (y > 2 && y < 5) ? 0x00ffff00 : 0;
  for (int i = 0; i < 128; i++) OAM[i * 4] = 0x200; // hidden
  DISPCNT = 0 | 0x40 | 0x100 | 0x200 | 0x400 | 0x1000;
  // window around the score, blending for the clouds
  REG16(0x04000050) = 0x0042 | 0x0100; REG16(0x04000052) = 0x0a06;
  // sound
  REG16(0x04000084) = 0x80;
  REG16(0x04000080) = 0xff77;
  REG16(0x04000082) = 0x0b0e; // DMA A full volume both sides, timer 0, PSG 100%
  TM0D = 0x10000 - 16777216 / RATE; TM0C = 0x80;
  SND(0x10) = 0x40; for (int i = 0; i < 8; i++) REG16(0x04000090 + i * 2) = 0x0123 * i ^ 0xcdef; SND(0x10) = 0x80;
  irq_init(1);
  // high score from SRAM
  u32 hi = REG8(0x0e000000) | REG8(0x0e000001) << 8;
  if (hi == 0xffff) hi = 0;
  int px = 60 << 4, py = 100 << 4, vy = 0, shots = 0, bx[4] = {0}, by[4] = {0}, score = 0;
  u32 f = 0, prev = 0;
  for (;;) {
    vblank_wait();
    mix(f);
    u32 k = keys(), nk = k & ~prev;
    prev = k;
    if (k & 0x10) px += 24;
    if (k & 0x20) px -= 24;
    if (nk & 1 && py >= (100 << 4)) { vy = -70; SND(0x00) = 0x17; SND(0x02) = 0xf780; SND(0x04) = 0x8000 | 1600; }
    vy += 4; py += vy; if (py > (100 << 4)) { py = 100 << 4; vy = 0; }
    if (nk & 2) { int i = shots++ & 3; bx[i] = px >> 4; by[i] = (py >> 4) + 4; SND(0x18) = 0xf13f; SND(0x1c) = 0x8000 | 0x40 | 0x22; score++; }
    if (nk & 0x200) tempo = tempo > 2 ? tempo - 1 : 2;
    if (nk & 0x100) tempo = tempo < 16 ? tempo + 1 : 16;
    if ((f % (tempo * 2)) == 0) { int n = notes[(f / tempo) & 31] % 12; SND(0x12) = 0x2000; SND(0x14) = 0x8000 | (2048 - 2097152 / (freq[n] / 2) / 32); }
    // player and bullets
    OAM[0] = ((py >> 4) & 0xff); OAM[1] = ((px >> 4) & 0x1ff) | 0x4000 | ((k & 0x20) ? 0x1000 : 0); OAM[2] = 0 | 0x800;
    for (int i = 0; i < 4; i++) { bx[i] += 4; OAM[(1 + i) * 4] = bx[i] > 0 && bx[i] < 240 ? (by[i] & 0xff) : 0x200; OAM[(1 + i) * 4 + 1] = bx[i] & 0x1ff; OAM[(1 + i) * 4 + 2] = 4; }
    // scrolling: ground with the camera, clouds slower, a wavy horizon by H-blank DMA
    BGHOFS(0) = f; BGHOFS(1) = f >> 2;
    for (int y = 0; y < 160; y++) hscroll[y] = (f >> 1) + (y > 120 ? ((y * 3 + f) & 7) : 0);
    DMA0CNT = 0; DMA0SAD = (u32)hscroll; DMA0DAD = 0x04000010; DMA0CNT = 0xa2400001;
    // score: 4 digits on BG2 (top left), high score top right
    if (score > (int)hi) { hi = score; REG8(0x0e000000) = hi; REG8(0x0e000001) = hi >> 8; }
    int s = score, h = hi;
    for (int d = 3; d >= 0; d--) { m2[1 * 32 + 1 + d] = 64 + s % 10; s /= 10; m2[1 * 32 + 25 + d] = 64 + h % 10; h /= 10; }
    draw_digit(0, 0, 0);
    f++;
  }
}
