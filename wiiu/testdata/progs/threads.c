#include "wiiu.h"
static OSMutex mtx;
static OSCondition cond;
static OSEvent ev;
static OSMessageQueue q;
static OSMessage qbuf[4];
static volatile int counter, ready, produced, alarms;
static uint8_t stacks[4][0x4000] __attribute__((aligned(16)));
static OSThread th[4];

static int worker(int argc, const char **argv) {
	for (int i = 0; i < 1000; i++) {
		OSLockMutex(&mtx);
		int v = counter;
		if (i % 100 == 0) OSYieldThread();
		counter = v + 1;
		OSUnlockMutex(&mtx);
	}
	return argc * 10 + (int)OSGetCoreId();
}

static int consumer(int argc, const char **argv) {
	int sum = 0;
	for (int i = 0; i < 20; i++) {
		OSMessage m;
		OSReceiveMessage(&q, &m, 1);
		sum += (int)m.args[0];
	}
	OSLockMutex(&mtx);
	ready = 1;
	OSSignalCond(&cond);
	OSUnlockMutex(&mtx);
	OSWaitEvent(&ev);
	return sum;
}

static void tick(OSAlarm *a, void *ctx) { alarms++; }

int main(int argc, char **argv) {
	OSInitMutex(&mtx);
	OSInitCond(&cond);
	OSInitEvent(&ev, 0, 1);
	OSInitMessageQueue(&q, qbuf, 4);
	for (int i = 0; i < 3; i++) {
		OSCreateThread(&th[i], worker, i + 1, 0, stacks[i] + sizeof stacks[i], sizeof stacks[i], 16, 1 << i);
		OSResumeThread(&th[i]);
	}
	int res[3];
	for (int i = 0; i < 3; i++) OSJoinThread(&th[i], &res[i]);
	OSReport("counter %d results %d %d %d\n", counter, res[0], res[1], res[2]);

	OSCreateThread(&th[3], consumer, 0, 0, stacks[3] + sizeof stacks[3], sizeof stacks[3], 15, 7);
	OSResumeThread(&th[3]);
	for (int i = 1; i <= 20; i++) {
		OSMessage m = {0, {i, 0, 0}};
		OSSendMessage(&q, &m, 1);
	}
	OSLockMutex(&mtx);
	while (!ready) OSWaitCond(&cond, &mtx);
	OSUnlockMutex(&mtx);
	OSSignalEvent(&ev);
	int sum;
	OSJoinThread(&th[3], &sum);
	OSReport("queue sum %d\n", sum);

	OSAlarm a;
	OSCreateAlarm(&a);
	OSTime one_ms = 248625000 / 4 / 1000;
	OSSetPeriodicAlarm(&a, OSGetTime(), one_ms * 5, tick);
	OSTime start = OSGetSystemTime();
	OSSleepTicks(one_ms * 100);
	OSCancelAlarm(&a);
	OSTime took = OSGetSystemTime() - start;
	OSReport("slept %d ms, alarms %s\n", (int)(took / one_ms), alarms >= 18 && alarms <= 22 ? "ok" : "wrong");

	void *heap = MEMGetBaseHeapHandle(1);
	void *p1 = MEMAllocFromExpHeapEx(heap, 1000, 64);
	void *p2 = MEMAllocFromDefaultHeapEx(5000, 0x100);
	void *p3 = MEMAllocFromExpHeapEx(heap, 100, -4);
	OSReport("heap %d %d %d\n", ((uint32_t)p1 & 63) == 0, ((uint32_t)p2 & 0xff) == 0, p3 > p2);
	MEMFreeToExpHeap(heap, p1);
	MEMFreeToDefaultHeap(p2);
	void *p4 = MEMAllocFromExpHeapEx(heap, 1000, 64);
	OSReport("reuse %d\n", p4 == p1);
	return 0;
}
