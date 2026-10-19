// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/MMU/MMU.h"

// Audio interface registers. Audio output is handled at the snd_core level,
// so no AI register is backed by hardware emulation; unhandled MMIO accesses
// read as zero and writes are dropped by the MMU.
