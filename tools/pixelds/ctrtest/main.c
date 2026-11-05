// PixelDS 3DS test program: checks the kernel and the services through
// raw system calls and IPC, the way 3DS programs use them, and writes the
// results at 0x00100100 for the Go test (cores/pixelds/ctr/ctr_test.go).
#include "svc.h"

volatile u32 results[32] __attribute__((section(".results")));

static u32 srv, gsp, hid, gspEvent;
static volatile u32 *gspShm = (u32 *)0x10002000;
static volatile u32 *hidShm = (u32 *)0x10000000;
static volatile u32 counter;
static volatile s32 arbValue;
static u32 arbiter, threadEvent;

static void thread1(u32 arg) {
	counter += arg;
	svc_SignalEvent(threadEvent, 0, 0, 0, 0, 0);
	svc_ExitThread(0, 0, 0, 0, 0, 0);
}

static void thread2(u32 arg) {
	// wait while arbValue < 1
	svc_ArbitrateAddress(arbiter, (u32)&arbValue, 1, 1, 0, 0);
	counter += 100;
	svc_ExitThread(0, 0, 0, 0, 0, 0);
}

static u8 stack1[0x1000] __attribute__((aligned(8)));
static u8 stack2[0x1000] __attribute__((aligned(8)));

static void waitVBlank(void) {
	svc_WaitSynchronization1(gspEvent, 0xffffffff, 0xffffffff, 0x7fffffff, 0, 0);
	// pop the queue
	u32 hdr = gspShm[0];
	u32 count = hdr >> 8 & 0xff;
	gspShm[0] = (hdr & 0xff) | 0;
	(void)count;
}

static void present(u32 screen, u32 fb, u32 stride, u32 format) {
	volatile u32 *slot = gspShm + (0x200 + screen * 0x40) / 4;
	slot[1] = 0;      // active framebuffer
	slot[2] = fb;     // left
	slot[3] = fb;     // right
	slot[4] = stride;
	slot[5] = format;
	slot[6] = 0;
	slot[7] = 0;
	slot[0] = 0 | 1 << 8; // index 0, update
}

float fsq(float x);

int main(void) {
	u32 *c;
	struct regs r;

	// srv
	r = svc_ConnectToPort(0, (u32) "srv:", 0, 0, 0, 0);
	srv = r.r1;
	c = cmdbuf();
	c[0] = 0x00010002;
	c[1] = 0x20;
	svc_SendSyncRequest(srv, 0, 0, 0, 0, 0);
	results[0] = r.r0 == 0 && c[1] == 0;

	// GSP: interrupt queue and shared memory
	if (getService(srv, "gsp::Gpu", &gsp) == 0) {
		r = svc_CreateEvent(0, 0, 0, 0, 0, 0);
		gspEvent = r.r1;
		c = cmdbuf();
		c[0] = 0x00130042;
		c[1] = 1;
		c[2] = 0;
		c[3] = gspEvent;
		svc_SendSyncRequest(gsp, 0, 0, 0, 0, 0);
		u32 res = c[1], shm = c[4];
		r = svc_MapMemoryBlock(shm, 0x10002000, 3, 0x10000000, 0, 0);
		c = cmdbuf();
		c[0] = 0x00160042;
		c[1] = 0;
		c[2] = 0;
		c[3] = 0xffff8001;
		svc_SendSyncRequest(gsp, 0, 0, 0, 0, 0);
		results[1] = res == 0x2a07 && r.r0 == 0 && c[1] == 0;
	}

	// HID
	if (getService(srv, "hid:USER", &hid) == 0) {
		c = cmdbuf();
		c[0] = 0x000a0000;
		svc_SendSyncRequest(hid, 0, 0, 0, 0, 0);
		u32 shm = c[3];
		r = svc_MapMemoryBlock(shm, 0x10000000, 1, 0x10000000, 0, 0);
		results[2] = c[1] == 0 && r.r0 == 0 && c[2] == 0x14000000;
	}

	// a framebuffer in the linear heap: a red-green gradient on the top
	// screen, blue on the bottom
	r = svc_ControlMemory(0x10003, 0, 0, 0x100000, 3, 0);
	u8 *top = (u8 *)r.r1;
	u8 *bot = top + 400 * 240 * 3;
	results[3] = r.r0 == 0 && r.r1 >= 0x14000000 && r.r1 < 0x18000000;
	for (u32 x = 0; x < 400; x++)
		for (u32 y = 0; y < 240; y++) {
			u8 *p = top + (x * 240 + (239 - y)) * 3;
			p[0] = 0;               // blue
			p[1] = y * 255 / 239;   // green
			p[2] = x * 255 / 399;   // red
		}
	for (u32 i = 0; i < 320 * 240; i++) {
		bot[i * 3] = 0xff;
		bot[i * 3 + 1] = 0x40;
		bot[i * 3 + 2] = 0x20;
	}
	present(0, (u32)top, 240 * 3, 1);
	present(1, (u32)bot, 240 * 3, 1);

	// VBlanks
	for (int i = 0; i < 3; i++) waitVBlank();
	results[4] = 3;

	// a thread of higher priority runs at once and signals an event
	r = svc_CreateEvent(0, 0, 0, 0, 0, 0);
	threadEvent = r.r1;
	r = svc_CreateThread(0x2f, (u32)thread1, 7, (u32)(stack1 + sizeof stack1), 0, 0);
	svc_WaitSynchronization1(threadEvent, 0xffffffff, 0xffffffff, 0x7fffffff, 0, 0);
	results[5] = r.r0 == 0 && counter == 7;

	// the address arbiter: a thread waits until main signals
	r = svc_CreateAddressArbiter(0, 0, 0, 0, 0, 0);
	arbiter = r.r1;
	arbValue = 0;
	r = svc_CreateThread(0x2f, (u32)thread2, 0, (u32)(stack2 + sizeof stack2), 0, 0);
	u32 before = counter;
	arbValue = 1;
	svc_ArbitrateAddress(arbiter, (u32)&arbValue, 0, 0xffffffff, 0, 0);
	svc_SleepThread(1000000, 0, 0, 0, 0, 0);
	results[6] = before == 7 && counter == 107;

	// a timer
	r = svc_CreateTimer(0, 0, 0, 0, 0, 0);
	u32 tm = r.r1;
	u64 t0 = 0;
	r = svc_GetSystemTick(0, 0, 0, 0, 0, 0);
	t0 = r.r0 | (u64)r.r1 << 32;
	svc_SetTimer(tm, 0, 2000000, 0, 0, 0); // 2 ms, once
	r = svc_WaitSynchronization1(tm, 0xffffffff, 0xffffffff, 0x7fffffff, 0, 0);
	struct regs t = svc_GetSystemTick(0, 0, 0, 0, 0, 0);
	u64 dt = (t.r0 | (u64)t.r1 << 32) - t0;
	results[7] = r.r0 == 0 && dt >= 500000 && dt < 1000000;

	// a wait that times out
	r = svc_CreateEvent(0, 0, 0, 0, 0, 0);
	r = svc_WaitSynchronization1(r.r1, 0, 1000000, 0, 0, 0);
	results[8] = r.r0 == 0x09401bfe;

	// floating point
	results[9] = (u32)(fsq(2.0f) * 1000.0f);

	// mutex and WaitSynchronizationN
	r = svc_CreateMutex(0, 1, 0, 0, 0, 0);
	u32 handles[2] = {threadEvent, r.r1};
	r = svc_WaitSynchronizationN(0, (u32)handles, 2, 0, 0xffffffff, 0);
	results[10] = r.r0 == 0 && r.r1 == 1;

	results[31] = 0xd04e;
	for (;;) {
		waitVBlank();
		results[11] = hidShm[(0x28 + hidShm[4] * 0x10) / 4];
	}
}
