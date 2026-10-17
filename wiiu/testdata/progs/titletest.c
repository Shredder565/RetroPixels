/* RetroPixels' title test: a program that uses libraries from its own code
   folder, linked (gamelib.rpl, which uses helperlib.rpl) and loaded while
   it runs (dynlib.rpl). */
#include "wiiu.h"

int lib_add(int a, int b);
extern int lib_counter;
int OSDynLoad_Acquire(const char *name, uint32_t *handle);
int OSDynLoad_FindExport(uint32_t handle, int isData, const char *name, void *out);

int main(int argc, char **argv) {
	OSReport("lib_add %d\n", lib_add(1, 2));
	OSReport("lib_counter %d\n", lib_counter);
	uint32_t h = 0;
	int (*fn)(void) = 0;
	int err = OSDynLoad_Acquire("dynlib", &h);
	if (err == 0)
		err = OSDynLoad_FindExport(h, 0, "dyn_value", &fn);
	OSReport("dyn %d %d\n", err, fn ? fn() : -1);
	OSReport("title test done\n");
	return 0;
}
