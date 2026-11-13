// PixelDS self-test, ARM7 side: answers the ARM9's IPC checks, then checks
// the clock, touch screen, firmware and sound, and reports by FIFO.
#include "ds.h"
static uint8_t spi(int dev, uint8_t v, int hold) {
    while (SPICNT & 0x80);
    SPICNT = 0x8000 | (dev << 8) | (hold ? 0x800 : 0);
    SPIDATA = v;
    while (SPICNT & 0x80);
    return SPIDATA;
}
static void rtcBit(int v) { RTC_REG = 0x10 | 0x20 | 0x40 | 4 | v; RTC_REG = 0x10 | 0x20 | 0x40 | 4 | 2 | v; }
static int rtcIn(void) { RTC_REG = 0x20 | 0x40 | 4; RTC_REG = 0x20 | 0x40 | 4 | 2; return RTC_REG & 1; }
static uint8_t rtcYear(void) {
    RTC_REG = 0x10 | 0x20 | 0x40; RTC_REG = 0x10 | 0x20 | 0x40 | 4;
    uint8_t cmd = 0x65; // date and time, read
    for (int i = 7; i >= 0; i--) rtcBit(cmd >> i & 1);
    uint8_t y = 0;
    for (int i = 0; i < 8; i++) y |= rtcIn() << i;
    RTC_REG = 0x20 | 0x40;
    return y;
}
static int16_t tone[32];
int main(void) {
    // IPC sync answer
    for (int i = 0; i < 1000000; i++) if ((IPCSYNC & 15) == 5) { IPCSYNC = 6 << 8; break; }
    // FIFO echo
    IPCFIFOCNT = 0x8000 | 8;
    for (int i = 0; i < 1000000; i++) if (!(IPCFIFOCNT & 0x100)) { uint32_t v = IPCFIFORECV; IPCFIFOSEND = v + 1; break; }
    uint32_t rep = 0xC0DE0000;
    // RTC: the year should look like BCD 20-99
    uint8_t y = rtcYear();
    if ((y >> 4) >= 2 && (y >> 4) <= 9 && (y & 15) <= 9) rep |= 1;
    // touch: pen up -> X reads 0
    spi(2, 0xD4, 1); uint16_t hi = spi(2, 0, 1); uint16_t lo = spi(2, 0, 0);
    if ((((hi << 8) | lo) >> 3) == 0) rep |= 2;
    // firmware: user settings version at 0x3FE00 is 5
    spi(1, 0x03, 1); spi(1, 0x03, 1); spi(1, 0xFE, 1); spi(1, 0x00, 1);
    uint8_t ver = spi(1, 0, 0);
    if (ver == 5) rep |= 4;
    // sound: a looping 16-bit square tone on channel 0
    for (int i = 0; i < 32; i++) tone[i] = i < 16 ? 12000 : -12000;
    SOUNDCNT = 0x8000 | 0x7F;
    SCHAN_SAD(0) = (uint32_t)tone; SCHAN_TMR(0) = 0x10000 - 512; SCHAN_PNT(0) = 0; SCHAN_LEN(0) = 16;
    SCHAN_CNT(0) = 0x7F | (64 << 16) | (1 << 27) | (1 << 29) | (1u << 31);
    if (SCHAN_CNT(0) & (1u << 31)) rep |= 8;
    IPCFIFOSEND = rep;
    for (;;) HALTCNT = 0x80;
}
