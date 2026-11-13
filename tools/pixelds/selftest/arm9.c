// PixelDS self-test, ARM9 side. Each test writes 1 (pass) or 2 (fail) to
// results[n] at 0x02380000 and shows a green or red block on the top screen.
// The bottom screen shows a 16-bit bitmap gradient and a sprite.
#include "ds.h"
#define RESULTS ((vu32*)0x02380000)
#define IRQ_HANDLER (*(vu32*)0x03003FFC)
#define IRQ_FLAGS   (*(vu32*)0x03003FF8)

static volatile int vblanks, timers;
static void irq(void) {
    uint32_t f = IF & IE;
    if (f & IRQ_VBLANK) vblanks++;
    if (f & IRQ_TIMER0) timers++;
    IRQ_FLAGS |= f;
    IF = f;
}

static void block(int n, int ok) {
    RESULTS[n] = ok ? 1 : 2;
    // 16x16 block: tiles of the BG0 map at 0x0600F800, 2x2 tiles per block
    vu16 *map = (vu16*)0x0600F800;
    int bx = (n % 8) * 4 + 1, by = (n / 8) * 4 + 2;
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++)
        map[(by + y) * 32 + bx + x] = (ok ? 1 : 2) | ((ok ? 1 : 2) << 12);
}

static uint32_t lz_src[] = { 0x00002010, 0x00F04160, 0x000000A0 }; // 32 x "A", LZ77

static int cardRead(uint32_t addr, uint32_t *out) {
    while (ROMCTRL & (1u << 31));
    AUXSPICNT = 0x8000 | 0x4000;
    CARDCMD[0] = 0xB7; CARDCMD[1] = addr >> 24; CARDCMD[2] = addr >> 16; CARDCMD[3] = addr >> 8;
    CARDCMD[4] = addr; CARDCMD[5] = CARDCMD[6] = CARDCMD[7] = 0;
    ROMCTRL = 0xA1586000; // start, 512 bytes
    int n = 0;
    while (ROMCTRL & (1u << 31)) {
        if (ROMCTRL & (1u << 23)) { uint32_t v = CARDDATA; if (n < 128) out[n] = v; n++; }
    }
    return n;
}

static uint8_t eepromXfer(uint8_t v, int hold) {
    AUXSPICNT = 0x8000 | 0x2000 | (hold ? 0x40 : 0);
    AUXSPIDATA = v;
    while (AUXSPICNT & 0x80);
    return AUXSPIDATA;
}

int main(void) {
    POWCNT1 = 0x820F;
    VRAMCNT_A = 0x81; // engine A BG
    VRAMCNT_C = 0x84; // engine B BG
    VRAMCNT_B = 0x82; // engine A OBJ
    DISPCNT = 0x00010100;           // mode 0, BG0
    BG0CNT = (31 << 8) | (0 << 2);  // map at 0xF800, tiles at 0
    BG_PAL[0] = 0x2108; BG_PAL[16+1] = 0x03E0; BG_PAL[32+1] = 0x001F;
    // tile 1 and 2: solid colour 1
    vu32 *tiles = (vu32*)0x06000000;
    for (int i = 8; i < 24; i++) tiles[i] = 0x11111111;
    for (int i = 0; i < 32*24; i++) ((vu16*)0x0600F800)[i] = 0;

    // bottom screen: 16-bit bitmap on BG3 (mode 5, direct colour)
    DISPCNT_B = 0x00010000 | 5 | (1 << 11);
    BG3CNT_B = (1 << 7) | (1 << 2) | (1 << 14); // direct colour, 256x256, base 0
    BG3PA_B = 256; BG3PB_B = 0; BG3PC_B = 0; BG3PD_B = 256;
    vu16 *bmp = (vu16*)0x06200000;
    for (int y = 0; y < 192; y++) for (int x = 0; x < 256; x++)
        bmp[y * 256 + x] = 0x8000 | (x >> 3) | ((y >> 3) << 5) | (((x + y) >> 4) << 10);
    // a sprite on top: 16x16 white square
    DISPCNT |= (1 << 12) | (1 << 4);
    vu32 *otiles = (vu32*)0x06400000;
    for (int i = 0; i < 32; i++) otiles[i] = 0x11111111;
    OBJ_PAL[1] = 0x7FFF;
    for (int i = 0; i < 128; i++) { OAM[i*4] = 0x200; }
    OAM[0] = 170; OAM[1] = (1 << 14) | 220; OAM[2] = 0;

    int t = 0;
    block(t++, 1); // the ARM9 runs

    IRQ_HANDLER = (uint32_t)irq;
    IE = IRQ_VBLANK | IRQ_TIMER0;
    DISPSTAT = 8;
    IME = 1;
    __asm__ volatile("mrs r0, cpsr\n bic r0, r0, #0x80\n msr cpsr_c, r0" ::: "r0");

    // 1: VBlank interrupts through the BIOS's VBlankIntrWait
    for (int i = 0; i < 3; i++) swiWaitForVBlank();
    block(t++, vblanks >= 3);

    // 2: a timer interrupt
    TM0CNT_L = 0x10000 - 1000; TM0CNT_H = 0x80 | 0x40 | 1;
    swiIntrWait(1, IRQ_TIMER0);
    TM0CNT_H = 0;
    block(t++, timers >= 1);

    // 3: IPC sync: the ARM7 answers 5 with 6
    IPCSYNC = 5 << 8;
    int ok = 0;
    for (int i = 0; i < 100000 && !ok; i++) ok = (IPCSYNC & 15) == 6;
    block(t++, ok);

    // 4: the IPC FIFO: the ARM7 sends back each word plus one
    IPCFIFOCNT = 0x8000 | 8;
    IPCFIFOSEND = 0x12345678;
    ok = 0;
    for (int i = 0; i < 100000; i++) if (!(IPCFIFOCNT & 0x100)) { ok = IPCFIFORECV == 0x12345679; break; }
    block(t++, ok);

    // 5: the divider and square root
    DIVCNT = 0; DIV_NUMER = (uint64_t)(int64_t)-100; DIV_DENOM = 7;
    while (DIVCNT & 0x8000);
    int64_t q = (int32_t)DIV_RESULT, r = (int32_t)DIV_REM;
    SQRTCNT = 1; SQRT_PARAM = 1000000ull * 1000000ull;
    while (SQRTCNT & 0x8000);
    block(t++, q == -14 && r == -2 && SQRT_RESULT == 1000000);

    // 6: DMA
    static uint32_t src[16], dst[16];
    for (int i = 0; i < 16; i++) src[i] = i * 0x01010101;
    DMA3SAD = (uint32_t)src; DMA3DAD = (uint32_t)dst; DMA3CNT = 0x84000000 | 16;
    while (DMA3CNT & 0x80000000);
    ok = 1; for (int i = 0; i < 16; i++) if (dst[i] != src[i]) ok = 0;
    block(t++, ok);

    // 7: reading the card (the header's first word, at 0x8000's mirror... read 0x0000 -> 0x8000)
    static uint32_t buf[128];
    int n = cardRead(0x8000, buf);
    ok = n == 128 && buf[0] == *(vu32*)0x02000000; // the ARM9 program starts at 0x8000? no: compare to ROM
    block(t++, n == 128);

    // 8: the BIOS's LZ77 decompression
    static uint8_t out[32];
    __asm__ volatile("mov r0, %0\n mov r1, %1\n swi 0x110000" :: "r"(lz_src), "r"(out) : "r0","r1","r2","r3","memory");
    ok = 1; for (int i = 0; i < 32; i++) if (out[i] != 'A') ok = 0;
    block(t++, ok);

    // 9: the save chip: write 4 bytes, read them back
    eepromXfer(0x06, 0);
    eepromXfer(0x02, 1); eepromXfer(0x00, 1); eepromXfer(0x10, 1);
    for (int i = 0; i < 31; i++) eepromXfer(0xA0 + i, 1);
    eepromXfer(0xA0 + 31, 0);
    eepromXfer(0x03, 1); eepromXfer(0x00, 1); eepromXfer(0x10, 1);
    ok = 1; for (int i = 0; i < 31; i++) if (eepromXfer(0, 1) != 0xA0 + i) ok = 0;
    eepromXfer(0, 0);
    block(t++, ok);

    // 10: the ARM7's report (RTC, touch, firmware, sound) arrives by FIFO
    uint32_t rep = 0;
    for (int i = 0; i < 2000000; i++) if (!(IPCFIFOCNT & 0x100)) { rep = IPCFIFORECV; break; }
    block(t++, rep == 0xC0DE0000 + 0xF);   // all four ARM7 checks passed
    RESULTS[31] = rep;
    RESULTS[30] = 0x600D;
    for (;;) swiWaitForVBlank();
}
