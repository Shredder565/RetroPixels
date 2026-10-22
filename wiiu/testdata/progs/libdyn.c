/* dynlib.rpl: titletest.c loads it with OSDynLoad_Acquire. */
#include "wiiu.h"

static int base;

int dyn_value(void) { return base + 7; }

int rpl_entry(void *handle, int reason) {
	if (reason == 1)
		base = 40;
	return 0;
}
