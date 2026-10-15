// Small helpers for RetroPixels' GBA test ROMs (original code).
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef signed char s8; typedef signed short s16; typedef signed int s32;
#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))
#define REG8(a) (*(volatile u8 *)(a))
#define DISPCNT REG16(0x04000000)
#define DISPSTAT REG16(0x04000004)
#define VCOUNT REG16(0x04000006)
#define KEYINPUT REG16(0x04000130)
#define KEYCNT REG16(0x04000132)
#define IE REG16(0x04000200)
#define IF REG16(0x04000202)
#define WAITCNT REG16(0x04000204)
#define IME REG16(0x04000208)
#define VRAM ((volatile u16 *)0x06000000)
#define PAL ((volatile u16 *)0x05000000)
#define OAM ((volatile u16 *)0x07000000)
#define BIOSFLAGS REG16(0x03007ff8)

extern volatile u32 irq_flags;
extern void (*volatile irq_hook)(u32);
volatile u32 irq_flags;
void (*volatile irq_hook)(u32);

static u32 rng = 12345;
static inline u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static inline u32 keys(void) { return ~KEYINPUT & 0x3ff; }

// SWI calls (Thumb: swi n)
#define SWI(n) asm volatile("swi " #n ::: "r0", "r1", "r2", "r3", "memory")
static inline void vblank_wait(void) { SWI(5); }
static inline void halt(void) { SWI(2); }

static void mem16(volatile u16 *d, u16 v, int n) { for (int i = 0; i < n; i++) d[i] = v; }

// Draw a 32-bit value as a row of 32 pixels (bit set = colour on) at (x, y) in mode 3.
static void bits_row(int x, int y, u32 v, u16 on, u16 off) {
  for (int i = 0; i < 32; i++) VRAM[y * 240 + x + i] = (v >> (31 - i)) & 1 ? on : off;
}

static void irq_init(u16 mask) {
  irq_flags = 0;
  IE = mask;
  if (mask & 1) DISPSTAT |= 8;
  if (mask & 2) DISPSTAT |= 0x10;
  IME = 1;
}
