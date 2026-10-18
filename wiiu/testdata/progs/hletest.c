/* RetroPixels' library structs test: calls that fill in structs the program
   gives them, each followed by a guard word that must stay untouched, and
   the values Cemu puts in them. */
#include "wiiu.h"

typedef struct { uint8_t data[0x1700]; } __attribute__((aligned(64))) FSClient;
typedef struct { uint8_t data[0xa80]; } __attribute__((aligned(64))) FSCmdBlock;
typedef struct { uint32_t flag, permission, owner, group, size; uint8_t rest[0x64 - 20]; } FSStat;
typedef struct { FSStat stat; char name[256]; } FSDirEntry; /* 0x64 + 256: more than the 0xe4 that's written */
typedef struct { void (*callback)(FSClient *, FSCmdBlock *, int, void *); void *context; OSMessageQueue *queue; } FSAsyncParams;
typedef struct { FSAsyncParams params; OSMessage msg; FSClient *client; FSCmdBlock *block; int status; } FSAsyncResult;
void FSInit(void);
int FSAddClient(FSClient *c, uint32_t errMask);
void FSInitCmdBlock(FSCmdBlock *b);
int FSOpenDir(FSClient *c, FSCmdBlock *b, const char *path, int *h, uint32_t errMask);
int FSReadDir(FSClient *c, FSCmdBlock *b, int h, FSDirEntry *e, uint32_t errMask);
int FSReadDirAsync(FSClient *c, FSCmdBlock *b, int h, FSDirEntry *e, uint32_t errMask, FSAsyncParams *p);
int FSRewindDir(FSClient *c, FSCmdBlock *b, int h, uint32_t errMask);
int FSCloseDir(FSClient *c, FSCmdBlock *b, int h, uint32_t errMask);
int FSGetStat(FSClient *c, FSCmdBlock *b, const char *path, FSStat *s, uint32_t errMask);
int FSGetCwd(FSClient *c, FSCmdBlock *b, char *out, uint32_t size, uint32_t errMask);
int FSAAddClient(void *attach);
int FSAGetCwd(int client, char *out, uint32_t size);
void *AXAcquireVoiceEx(uint32_t prio, void *cb, void *user);
void AXInit(void);
uint32_t GX2RGetBufferAlignment(uint32_t flags);
uint32_t GX2RGetBufferAllocationSize(void *buffer);

static FSClient client;
static FSCmdBlock block;
static OSMessage msgs[4];
static OSMessageQueue queue;

#define GUARD 0x600dc0de

int main(int argc, char **argv) {
	FSInit();
	FSAddClient(&client, 0xffffffff);
	FSInitCmdBlock(&block);

	/* a directory entry holding a long name, and a guard word after it */
	static struct { FSDirEntry e; uint32_t guard; } d;
	uint32_t *after = (uint32_t *)((uint8_t *)&d.e + 0xe4);
	for (int i = 0; i < (int)sizeof d.e / 4; i++)
		((uint32_t *)&d.e)[i] = GUARD;
	int h = -1, st = FSOpenDir(&client, &block, "/vol/content/long", &h, 0xffffffff);
	int r = FSReadDir(&client, &block, h, &d.e, 0xffffffff);
	int len = 0;
	while (len < 256 && d.e.name[len])
		len++;
	OSReport("dir: open %d read %d flag %x len %d guard %x\n", st, r, d.e.stat.flag, len, *after == GUARD);

	/* the same, finished by a message */
	OSInitMessageQueue(&queue, msgs, 4);
	FSAsyncParams p = {0, (void *)0x1234, &queue};
	FSRewindDir(&client, &block, h, 0xffffffff);
	r = FSReadDirAsync(&client, &block, h, &d.e, 0xffffffff, &p);
	OSMessage m;
	OSReceiveMessage(&queue, &m, 1);
	FSAsyncResult *res = m.message;
	uint32_t at = (uint32_t)res - (uint32_t)&block;
	OSReport("async: %d type %d in block %d status %d block %d ctx %x\n", r, m.args[2], at < sizeof block,
		res->status, res->block == &block, (uint32_t)res->params.context);
	FSCloseDir(&client, &block, h, 0xffffffff);

	/* a folder: no size */
	FSStat s;
	for (int i = 0; i < (int)sizeof s / 4; i++)
		((uint32_t *)&s)[i] = GUARD;
	st = FSGetStat(&client, &block, "/vol/content/long", &s, 0xffffffff);
	OSReport("stat: %d flag %x size %d perm %x\n", st, s.flag, s.size, s.permission);

	/* a working folder cut short to fit */
	static struct { char buf[4]; uint32_t guard; } cwd = {{0}, GUARD};
	int fsa = FSAAddClient(0);
	st = FSAGetCwd(fsa, cwd.buf, sizeof cwd.buf);
	OSReport("cwd: %d %d guard %x\n", st, cwd.buf[3] == 0, cwd.guard == GUARD);

	/* a GamePad sample, no touch */
	static struct { VPADStatus v; uint32_t guard; } pad;
	pad.guard = GUARD;
	int32_t err = 5;
	int n = VPADRead(0, &pad.v, 1, &err);
	uint8_t *raw = (uint8_t *)&pad.v;
	uint16_t valid = raw[0x52 + 6] << 8 | raw[0x52 + 7];
	float accX = *(float *)(raw + 0x30);
	OSReport("vpad: %d err %d valid %d battery %x accx %d guard %x\n", n, err, valid, raw[0xa1], (int)accX,
		pad.guard == GUARD);
	err = 5;
	n = VPADRead(1, &pad.v, 1, &err);
	OSReport("vpad1: %d err %d guard %x\n", n, err, pad.guard == GUARD);

	/* sound: AXAcquireVoiceEx's callback is at 0x48 */
	AXInit();
	uint32_t *voice = AXAcquireVoiceEx(10, (void *)0x5678, (void *)0x9abc);
	OSReport("ax: cb %x ex %x user %x\n", voice[0x20 / 4], voice[0x48 / 4], voice[0x24 / 4]);

	/* GX2R: alignment by use, sizes padded to 64 bytes */
	uint32_t buf[4] = {1 << 4, 3, 7, 0};
	OSReport("gx2r: %x %x %x\n", GX2RGetBufferAlignment(1 << 4), GX2RGetBufferAlignment(1 << 6),
		GX2RGetBufferAllocationSize(buf));
	OSReport("hle test done\n");
	return 0;
}
