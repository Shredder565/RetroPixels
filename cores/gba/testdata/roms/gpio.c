// Cartridge GPIO test ROM: real-time clock (both bit orders, status writes,
// reset), sunlight sensor, gyro, rumble motor and the tilt sensor in the save
// area. Every routine runs whatever the cart is (the game code picks the
// chips); the values read are drawn on the screen.
#include "common.h"
#ifdef TAG
__attribute__((used)) const char save_tag[] = TAG;
#endif
#define GP(o) REG16(0x080000c4 + (o))
static int line;
static void show(u32 v) { bits_row((line / 160) * 34, line % 160, v, 0x7fff, 0x2000); line = (line + 1) % 320; }

static void rtc_cmd(u32 c, int lsb) {
  GP(4) = 1; GP(2) = 7; GP(0) = 1; GP(0) = 5;
  for (int i = 0; i < 8; i++) { int b = lsb ? (c >> i) & 1 : (c >> (7 - i)) & 1; GP(0) = 4 | b << 1; GP(0) = 5 | b << 1; }
}
static u32 rtc_read(int n) {
  GP(2) = 5;
  u32 v = 0;
  for (int i = 0; i < n * 8; i++) { GP(0) = 4; GP(0) = 5; v |= ((GP(0) >> 1) & 1) << (i & 31); if ((i & 31) == 31) { show(v); v = 0; } }
  GP(0) = 1; GP(2) = 7;
  return v;
}
static void rtc_write(u32 b) {
  for (int i = 0; i < 8; i++) { int x = (b >> i) & 1; GP(0) = 4 | x << 1; GP(0) = 5 | x << 1; }
  GP(0) = 1;
}
static int sun(void) {
  GP(4) = 1; GP(2) = 7; GP(0) = 2; GP(0) = 0;
  for (int n = 0; n < 256; n++) { if (GP(0) & 8) return n; GP(0) = 1; GP(0) = 0; }
  return 256;
}
static u32 gyro(int motor) {
  GP(4) = 1; GP(2) = 0xb;
  int m = motor ? 8 : 0;
  GP(0) = m | 1; GP(0) = m | 2; GP(0) = m;
  u32 v = 0;
  for (int i = 0; i < 16; i++) { GP(0) = m | 2; GP(0) = m; v = v << 1 | ((GP(0) >> 2) & 1); }
  return v;
}
static u32 tilt(void) {
  REG8(0x0e008000) = 0x55; REG8(0x0e008100) = 0xaa;
  return REG8(0x0e008200) | REG8(0x0e008300) << 8 | REG8(0x0e008400) << 16 | REG8(0x0e008500) << 24;
}

int main(void) {
  DISPCNT = 3 | 0x400;
  irq_init(1);
  u32 f = 0;
  for (;;) {
    vblank_wait();
    u32 k = keys();
    rng += k;
    u32 r = rnd();
    // RTC: status, date + time, time, both bit orders; sometimes a status write or reset
    rtc_cmd(0x63, r & 1); show(rtc_read(1));
    rtc_cmd(0x65, (r >> 1) & 1); rtc_read(7);
    if (k & 1) { rtc_cmd(0x67, 0); rtc_read(3); }
    if ((k & 0x30) == 0x30) { rtc_cmd(0x62, 0); rtc_write(r & 0x40 ? 0x40 : 0x00); }
    if ((k & 0x300) == 0x300 && (r & 15) == 0) rtc_cmd(0x60, 0);
    if (k & 0x80) { GP(4) = 0; show(GP(0) | GP(2) << 4 | GP(4) << 8); } // write-only port reads the ROM
    show(sun());
    show(gyro(k & 2));
    // rumble: motor on for part of the frame
    GP(2) = 8; GP(0) = 8;
    while (VCOUNT != (r & 127)) ;
    GP(0) = 0;
    if (k & 4) { GP(0) = 8; }
    show(tilt());
    show(REG32(0x080000c4) ^ REG16(0x080000c6));
    f++;
  }
}
