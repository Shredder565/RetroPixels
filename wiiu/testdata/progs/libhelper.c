/* helperlib.rpl: used by gamelib.rpl. Its entry point must run first. */
#include "wiiu.h"

int helper_ready;

int helper_mul(int a, int b) { return helper_ready ? a * b : -1000; }

int rpl_entry(void *handle, int reason) {
	if (reason == 1)
		helper_ready = 1;
	return 0;
}
