#include "wiiu.h"
// Threads waiting on each other: suspend counts, joins, events, semaphores,
// mutex hand-over, message queues, priorities, spin locks, thread queues,
// OSRunThread, alarms, rendezvous and OSGetSystemInfo.
typedef struct OSSemaphore { uint8_t data[0x20]; } OSSemaphore;
typedef struct OSSpinLock { uint8_t data[0x10]; } OSSpinLock;
typedef struct OSThreadQueue { uint8_t data[0x10]; } OSThreadQueue;
typedef struct OSAlarmQueue { uint8_t data[0x24]; } OSAlarmQueue;
typedef struct { uint32_t busClock, coreClock; uint64_t baseTime; uint32_t l2[3], ratio; } OSSystemInfo;
typedef void (*OSThreadCallback)(OSThread *t, void *stack);
int32_t OSSuspendThread(OSThread *t);
BOOL OSIsThreadSuspended(OSThread *t);
BOOL OSIsThreadTerminated(OSThread *t);
void OSDetachThread(OSThread *t);
OSThreadCallback OSSetThreadDeallocator(OSThread *t, OSThreadCallback f);
OSThreadCallback OSSetThreadCleanupCallback(OSThread *t, OSThreadCallback f);
BOOL OSCreateThreadType(OSThread *t, OSThreadEntry entry, int argc, char *argv, void *stackTop, uint32_t stackSize,
	int32_t prio, uint8_t attr, int type);
int32_t OSGetThreadPriority(OSThread *t);
void OSExitThread(int v);
BOOL OSDisableInterrupts(void);
BOOL OSRestoreInterrupts(BOOL on);
void OSInitSemaphore(OSSemaphore *s, int32_t count);
int32_t OSWaitSemaphore(OSSemaphore *s);
int32_t OSSignalSemaphore(OSSemaphore *s);
int32_t OSGetSemaphoreCount(OSSemaphore *s);
void OSResetEvent(OSEvent *e);
void OSSignalEventAll(OSEvent *e);
BOOL OSWaitEventWithTimeout(OSEvent *e, OSTime ns);
BOOL OSJamMessage(OSMessageQueue *q, OSMessage *m, uint32_t flags);
BOOL OSPeekMessage(OSMessageQueue *q, OSMessage *m);
void OSInitSpinLock(OSSpinLock *l);
BOOL OSAcquireSpinLock(OSSpinLock *l);
BOOL OSReleaseSpinLock(OSSpinLock *l);
void OSInitThreadQueue(OSThreadQueue *q);
void OSSleepThread(OSThreadQueue *q);
void OSWakeupThread(OSThreadQueue *q);
BOOL OSSetAlarmEx(OSAlarm *a, OSTime delay, OSAlarmCallback cb, OSAlarmQueue *q);
void OSInitAlarmQueue(OSAlarmQueue *q);
BOOL OSGetAlarmFromQueue(OSAlarmQueue *q, OSAlarm **a, void **ctx, BOOL noWait);
OSSystemInfo *OSGetSystemInfo(void);
typedef struct OSRendezvous { uint32_t data[4]; } OSRendezvous;
void OSInitRendezvous(OSRendezvous *r);
BOOL OSWaitRendezvous(OSRendezvous *r, uint32_t coreMask);

#define CORE0 1
#define CORE1 2
#define ANY 7
static uint8_t stacks[16][0x4000] __attribute__((aligned(16)));
static OSThread th[16];
#define STACK(i) stacks[i] + sizeof stacks[i], sizeof stacks[i]
static const OSTime ms = 248625000 / 4 / 1000;

static volatile uint32_t passed;
static volatile int ran;
static int setRan(int argc, const char **argv) { ran = argc; return argc + 1; }

static void testSuspend(void) {
	ran = 0;
	OSCreateThread(&th[0], setRan, 7, 0, STACK(0), 16, CORE1);
	int s0 = OSIsThreadSuspended(&th[0]);
	OSSleepTicks(ms);
	int r0 = ran;
	int a = OSSuspendThread(&th[0]);
	int b = OSResumeThread(&th[0]);
	OSSleepTicks(ms);
	int r1 = ran;
	int c = OSResumeThread(&th[0]);
	int res = 0;
	int j = OSJoinThread(&th[0], &res);
	int d = OSResumeThread(&th[0]);
	OSReport("suspend %d %d %d %d %d %d %d %d %d %d\n", s0, r0, a, b, r1, c, j, res, ran, d);
}

static volatile int cleaned, freed;
static void cleanup(OSThread *t, void *stack) { cleaned++; }
static void dealloc(OSThread *t, void *stack) { freed++; }
static int quick(int argc, const char **argv) { return 5; }
static int exiter(int argc, const char **argv) {
	OSExitThread(6);
	return 0;
}

static void testJoin(void) {
	OSCreateThread(&th[1], quick, 0, 0, STACK(1), 16, CORE0);
	OSSetThreadCleanupCallback(&th[1], cleanup);
	OSSetThreadDeallocator(&th[1], dealloc);
	OSResumeThread(&th[1]);
	OSSleepTicks(ms);
	int term = OSIsThreadTerminated(&th[1]);
	int freedBefore = freed;
	int r = 0;
	int j = OSJoinThread(&th[1], &r);
	int freedAfter = freed;
	OSCreateThread(&th[2], exiter, 0, 0, STACK(2), 16, ANY);
	OSResumeThread(&th[2]);
	int r2 = 0;
	int j2 = OSJoinThread(&th[2], &r2);
	OSCreateThread(&th[3], quick, 0, 0, STACK(3), 16, CORE0);
	OSDetachThread(&th[3]);
	OSSetThreadDeallocator(&th[3], dealloc);
	OSResumeThread(&th[3]);
	OSSleepTicks(ms);
	int j3 = OSJoinThread(&th[3], 0);
	OSReport("join %d %d %d %d %d %d %d %d %d %d\n", term, cleaned, freedBefore, j, r, freedAfter, j2, r2, j3, freed);
}

static OSEvent evm, eva;
static int evWaiter(int argc, const char **argv) {
	OSWaitEvent(argc ? &eva : &evm);
	OSAddAtomic(&passed, 1);
	return 0;
}

static void testEvents(void) {
	passed = 0;
	OSInitEvent(&evm, 0, 0);
	for (int i = 0; i < 2; i++) {
		OSCreateThread(&th[4 + i], evWaiter, 0, 0, STACK(4 + i), 16, CORE0);
		OSResumeThread(&th[4 + i]);
	}
	OSSleepTicks(ms);
	// a pulse of a manual-reset event lets every waiter through
	OSSignalEvent(&evm);
	OSResetEvent(&evm);
	for (int i = 0; i < 2; i++) OSJoinThread(&th[4 + i], 0);
	int manual = passed;

	passed = 0;
	OSInitEvent(&eva, 0, 1);
	for (int i = 0; i < 3; i++) {
		OSCreateThread(&th[4 + i], evWaiter, 1, 0, STACK(4 + i), 16, ANY);
		OSResumeThread(&th[4 + i]);
	}
	OSSleepTicks(ms);
	OSSignalEvent(&eva);
	OSSleepTicks(ms);
	int one = passed;
	OSSignalEventAll(&eva);
	OSSleepTicks(ms);
	int all = passed;
	for (int i = 0; i < 3; i++) OSJoinThread(&th[4 + i], 0);
	int none = OSWaitEventWithTimeout(&eva, 0);
	OSSignalEvent(&eva);
	int sig = OSWaitEventWithTimeout(&eva, 0);
	OSTime start = OSGetSystemTime();
	int late = OSWaitEventWithTimeout(&eva, 2000000);
	int waited = OSGetSystemTime() - start >= ms;
	OSReport("events %d %d %d %d %d %d %d\n", manual, one, all, none, sig, late, waited);
}

static OSSemaphore sem;
static int semWaiter(int argc, const char **argv) {
	OSWaitSemaphore(&sem);
	OSAddAtomic(&passed, 1);
	return 0;
}

static void testSemaphores(void) {
	passed = 0;
	OSInitSemaphore(&sem, 0);
	for (int i = 0; i < 3; i++) {
		OSCreateThread(&th[4 + i], semWaiter, 0, 0, STACK(4 + i), 16, ANY);
		OSResumeThread(&th[4 + i]);
	}
	OSSleepTicks(ms);
	int prev = OSSignalSemaphore(&sem);
	OSSleepTicks(ms);
	int p1 = passed;
	OSSignalSemaphore(&sem);
	OSSignalSemaphore(&sem);
	for (int i = 0; i < 3; i++) OSJoinThread(&th[4 + i], 0);
	OSReport("sem %d %d %d %d\n", prev, p1, passed, OSGetSemaphoreCount(&sem));
}

// a thread that takes a mutex again at once doesn't keep it from a waiter
static OSMutex hogMutex;
static volatile int stop;
static int hog(int argc, const char **argv) {
	while (!stop) {
		OSLockMutex(&hogMutex);
		for (volatile int i = 0; i < 50; i++)
			;
		OSUnlockMutex(&hogMutex);
	}
	return 0;
}

static void testMutex(void) {
	OSInitMutex(&hogMutex);
	OSCreateThread(&th[7], hog, 0, 0, STACK(7), 16, CORE0);
	OSCreateThread(&th[8], hog, 0, 0, STACK(8), 16, CORE1);
	OSResumeThread(&th[7]);
	OSResumeThread(&th[8]);
	OSSleepTicks(ms);
	int got = 0;
	for (int i = 0; i < 20; i++) {
		OSLockMutex(&hogMutex);
		got++;
		OSUnlockMutex(&hogMutex);
	}
	stop = 1;
	OSJoinThread(&th[7], 0);
	OSJoinThread(&th[8], 0);
	OSReport("mutex %d\n", got);
}

static OSMessageQueue mq;
static OSMessage mqbuf[2];
static int sender(int argc, const char **argv) {
	int ok = 0;
	for (int i = 1; i <= 4; i++) {
		OSMessage m = {0, {i, 0, 0}};
		ok += OSSendMessage(&mq, &m, 1);
	}
	return ok;
}

static void testMessages(void) {
	OSInitMessageQueue(&mq, mqbuf, 2);
	OSCreateThread(&th[9], sender, 0, 0, STACK(9), 16, CORE0);
	OSResumeThread(&th[9]);
	OSSleepTicks(ms);
	int seq = 0;
	for (int i = 0; i < 4; i++) {
		OSMessage m;
		OSReceiveMessage(&mq, &m, 1);
		seq = seq * 10 + (int)m.args[0];
	}
	int sent = 0;
	OSJoinThread(&th[9], &sent);
	OSMessage m1 = {0, {1, 0, 0}}, m2 = {0, {2, 0, 0}}, m9 = {0, {9, 0, 0}}, got;
	OSSendMessage(&mq, &m1, 0);
	OSSendMessage(&mq, &m2, 0);
	int full = OSSendMessage(&mq, &m1, 0);
	int peek = OSPeekMessage(&mq, 0);
	OSReceiveMessage(&mq, &got, 0);
	int first = got.args[0];
	OSJamMessage(&mq, &m9, 0);
	OSReceiveMessage(&mq, &got, 0);
	int jammed = got.args[0];
	OSReceiveMessage(&mq, &got, 0);
	int last = got.args[0];
	int empty = OSReceiveMessage(&mq, &got, 0);
	OSReport("mq %d %d %d %d %d %d %d %d\n", seq, sent, full, peek, first, jammed, last, empty);
}

static OSEvent pev;
static volatile int after, seen, on;
static char order[4];
static int high(int argc, const char **argv) {
	OSWaitEvent(&pev);
	seen = after;
	return 0;
}
static int mark(int argc, const char **argv) {
	order[on++] = (char)argc;
	return 0;
}

static void testPriorities(void) {
	// a higher-priority thread on this core runs as soon as it's woken
	after = 0;
	OSInitEvent(&pev, 0, 1);
	OSCreateThread(&th[10], high, 0, 0, STACK(10), 10, CORE1);
	OSResumeThread(&th[10]);
	OSSignalEvent(&pev);
	after = 1;
	OSJoinThread(&th[10], 0);
	// or resumed
	ran = 0;
	OSCreateThread(&th[10], setRan, 3, 0, STACK(10), 10, CORE1);
	OSResumeThread(&th[10]);
	int now = ran;
	OSJoinThread(&th[10], 0);
	// I/O threads go before app threads; with interrupts off nothing takes the core
	BOOL ints = OSDisableInterrupts();
	OSCreateThread(&th[11], mark, 'A', 0, STACK(11), 0, CORE1);
	OSCreateThreadType(&th[12], mark, 'I', 0, STACK(12), 31, CORE1, 1);
	OSResumeThread(&th[11]);
	OSResumeThread(&th[12]);
	int mid = on;
	OSRestoreInterrupts(ints);
	int iop = OSGetThreadPriority(&th[12]);
	OSJoinThread(&th[11], 0);
	OSJoinThread(&th[12], 0);
	OSReport("prio %d %d %d %s %d\n", seen, now, mid, order, iop);
}

// a thread holding a spin lock runs before a higher-priority one spinning
// for it on the same core
static OSSpinLock sl;
static volatile int slCount, slSeen = -1;
static int spinHigh(int argc, const char **argv) {
	OSAcquireSpinLock(&sl);
	slSeen = slCount;
	OSReleaseSpinLock(&sl);
	return 0;
}
static int spinLow(int argc, const char **argv) {
	OSAcquireSpinLock(&sl);
	OSCreateThread(&th[14], spinHigh, 0, 0, STACK(14), 5, CORE0);
	OSResumeThread(&th[14]);
	for (volatile int i = 0; i < 20000; i++)
		;
	slCount = 1;
	OSReleaseSpinLock(&sl);
	OSJoinThread(&th[14], 0);
	return 0;
}

static void testSpinLock(void) {
	OSInitSpinLock(&sl);
	OSCreateThread(&th[13], spinLow, 0, 0, STACK(13), 20, CORE0);
	OSResumeThread(&th[13]);
	OSJoinThread(&th[13], 0);
	OSReport("spin %d\n", slSeen);
}

static OSThreadQueue tq;
static volatile int wakeFlag;
static int sleeper(int argc, const char **argv) {
	BOOL ints = OSDisableInterrupts();
	while (!wakeFlag) OSSleepThread(&tq);
	OSRestoreInterrupts(ints);
	return 7;
}

static void testQueueAndRun(void) {
	OSInitThreadQueue(&tq);
	OSCreateThread(&th[15], sleeper, 0, 0, STACK(15), 16, CORE0);
	OSResumeThread(&th[15]);
	OSSleepTicks(ms);
	wakeFlag = 1;
	OSWakeupThread(&tq);
	int r = 0;
	OSJoinThread(&th[15], &r);
	// a thread made without an entry point is started by OSRunThread
	int made = OSCreateThread(&th[15], 0, 0, 0, STACK(15), 16, CORE1);
	int run = OSRunThread(&th[15], setRan, 4, 0);
	int res = 0;
	OSJoinThread(&th[15], &res);
	OSReport("queue %d run %d %d %d\n", r, made, run, res);
}

static volatile int fired;
static void fire(OSAlarm *a, void *ctx) { fired++; }

static void testAlarms(void) {
	static OSAlarm a1, a2;
	static OSAlarmQueue aq;
	OSCreateAlarm(&a1);
	int set = OSSetAlarm(&a1, 0, fire);
	OSSleepTicks(ms * 2);
	int f0 = fired;
	OSInitAlarmQueue(&aq);
	OSCreateAlarm(&a2);
	OSSetAlarmEx(&a2, ms, fire, &aq);
	OSAlarm *got = 0, *got2 = 0;
	void *ctx = 0, *ctx2 = 0;
	int g = OSGetAlarmFromQueue(&aq, &got, &ctx, 0);
	int none = OSGetAlarmFromQueue(&aq, &got2, &ctx2, 1);
	OSReport("alarm %d %d %d %d %d %d\n", set, f0, g, got == &a2, fired, none);
}

static OSRendezvous rv;
static int meet(int argc, const char **argv) {
	OSWaitRendezvous(&rv, 7);
	OSAddAtomic(&passed, 1);
	return 0;
}

static void testRendezvous(void) {
	passed = 0;
	OSInitRendezvous(&rv);
	OSCreateThread(&th[4], meet, 0, 0, STACK(4), 16, CORE0);
	OSCreateThread(&th[5], meet, 0, 0, STACK(5), 16, 4);
	OSResumeThread(&th[4]);
	OSSleepTicks(ms);
	int early = passed;
	OSResumeThread(&th[5]);
	OSWaitRendezvous(&rv, 7);
	OSJoinThread(&th[4], 0);
	OSJoinThread(&th[5], 0);
	OSReport("rendezvous %d %d\n", early, passed);
}

static void testSystemInfo(void) {
	OSSystemInfo *si = OSGetSystemInfo();
	int64_t d = OSGetTime() - (int64_t)si->baseTime - OSGetSystemTime();
	OSReport("sysinfo %d %d %d %d\n", (int)si->busClock, (int)si->coreClock, (int)si->ratio, d > -10000 && d <= 0);
}

int main(int argc, char **argv) {
	testSuspend();
	testJoin();
	testEvents();
	testSemaphores();
	testMutex();
	testMessages();
	testPriorities();
	testSpinLock();
	testQueueAndRun();
	testAlarms();
	testRendezvous();
	testSystemInfo();
	OSReport("sync done\n");
	return 0;
}
