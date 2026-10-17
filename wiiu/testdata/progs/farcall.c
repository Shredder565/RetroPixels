#include "wiiu.h"
// 36 MB of code before main, so main is further from the system's entry
// points than a branch reaches: its calls need trampolines.
__asm__(".section .text\n.globl far_pad\nfar_pad:\n.space 0x2400000\n.previous\n");
int main(int argc, char **argv) {
	OSReport("far call works\n");
	return 5;
}
