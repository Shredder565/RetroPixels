#include "wiiu.h"
int main(int argc, char **argv) {
	OSReport("hello from %s: %d %u %x %s %.3f %lld\n", argv[0], -42, 42u, 0xbeef, "wii u", 3.25, 1234567890123LL);
	char buf[64];
	int n = __os_snprintf(buf, sizeof buf, "%05d|%-4s|%c", 7, "ab", 'Z');
	OSReport("snprintf: %s (%d)\n", buf, n);
	return 3;
}
