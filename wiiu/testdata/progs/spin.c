/* A busy program for measuring the CPU's speed. */
#include "wiiu.h"

volatile uint32_t sink;

int main(int argc, char **argv) {
	uint32_t a = 1, b = 2;
	for (;;) {
		for (int i = 0; i < 1000; i++) {
			a = a * 1664525 + 1013904223;
			b ^= a >> 3;
			if (b & 1)
				b += i;
		}
		sink = a + b;
	}
}
