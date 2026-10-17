/* A title library (gamelib.rpl) for titletest.c: its entry point sets
   lib_counter, and lib_add uses another title library (helperlib.rpl). */
#include "wiiu.h"

int lib_counter;
int helper_mul(int a, int b);

int lib_add(int a, int b) { return helper_mul(a + b, 2) + lib_counter; }

int rpl_entry(void *handle, int reason) {
	if (reason == 1)
		lib_counter += 100;
	return 0;
}
