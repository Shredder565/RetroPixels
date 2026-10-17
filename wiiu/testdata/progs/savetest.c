/* RetroPixels' save test: reads a file of the title's content folder, then
   counts its runs in a save file (nn_save), so a second run reads 2. */
#include "wiiu.h"

typedef struct { uint8_t data[0x1700]; } __attribute__((aligned(64))) FSClient;
typedef struct { uint8_t data[0xa80]; } __attribute__((aligned(64))) FSCmdBlock;
void FSInit(void);
int FSAddClient(FSClient *c, uint32_t errMask);
void FSInitCmdBlock(FSCmdBlock *b);
int FSOpenFile(FSClient *c, FSCmdBlock *b, const char *path, const char *mode, int *h, uint32_t errMask);
int FSReadFile(FSClient *c, FSCmdBlock *b, void *buf, uint32_t size, uint32_t count, int h, int flag, uint32_t errMask);
int FSWriteFile(FSClient *c, FSCmdBlock *b, const void *buf, uint32_t size, uint32_t count, int h, int flag, uint32_t errMask);
int FSCloseFile(FSClient *c, FSCmdBlock *b, int h, uint32_t errMask);
void SAVEInit(void);
int SAVEInitSaveDir(uint8_t account);
int SAVEOpenFile(FSClient *c, FSCmdBlock *b, uint8_t account, const char *path, const char *mode, int *h, uint32_t errMask);
uint8_t GetSlotNo__Q2_2nn3actFv(void);
unsigned long long OSGetTitleID(void);

static FSClient client;
static FSCmdBlock block;

int main(int argc, char **argv) {
	FSInit();
	FSAddClient(&client, 0xffffffff);
	FSInitCmdBlock(&block);
	unsigned long long id = OSGetTitleID();
	OSReport("title %08x%08x\n", (uint32_t)(id >> 32), (uint32_t)id);

	char text[32] = {0};
	int h;
	if (FSOpenFile(&client, &block, "/vol/content/hello.txt", "r", &h, 0xffffffff) == 0) {
		FSReadFile(&client, &block, text, 1, sizeof text - 1, h, 0, 0xffffffff);
		FSCloseFile(&client, &block, h, 0xffffffff);
	}
	OSReport("content %s\n", text);

	SAVEInit();
	uint8_t slot = GetSlotNo__Q2_2nn3actFv();
	SAVEInitSaveDir(slot);
	uint32_t runs = 0;
	if (SAVEOpenFile(&client, &block, slot, "runs.bin", "r", &h, 0xffffffff) == 0) {
		FSReadFile(&client, &block, &runs, 4, 1, h, 0, 0xffffffff);
		FSCloseFile(&client, &block, h, 0xffffffff);
	}
	runs++;
	if (SAVEOpenFile(&client, &block, slot, "runs.bin", "w", &h, 0xffffffff) == 0) {
		FSWriteFile(&client, &block, &runs, 4, 1, h, 0, 0xffffffff);
		FSCloseFile(&client, &block, h, 0xffffffff);
	}
	OSReport("runs %d\n", runs);
	OSReport("save test done\n");
	return 0;
}
