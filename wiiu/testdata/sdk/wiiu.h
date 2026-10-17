/* RetroPixels' tiny Wii U SDK for its own test programs: declarations of the
   system library functions they use. Build with clang --target=powerpc-eabi. */
#pragma once
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned long size_t;
typedef int BOOL;
typedef int64_t OSTime;
#define NULL ((void *)0)

/* coreinit */
void OSReport(const char *fmt, ...);
int __os_snprintf(char *buf, size_t n, const char *fmt, ...);
void OSFatal(const char *msg);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
OSTime OSGetTime(void);
OSTime OSGetSystemTime(void);
void OSSleepTicks(OSTime ticks);
uint32_t OSGetCoreId(void);
void exit(int code);

typedef struct OSThread { uint8_t data[0x6a0]; } __attribute__((aligned(8))) OSThread;
typedef int (*OSThreadEntry)(int argc, const char **argv);
BOOL OSCreateThread(OSThread *t, OSThreadEntry entry, int argc, char *argv, void *stackTop, uint32_t stackSize, int32_t prio, uint8_t attr);
int32_t OSResumeThread(OSThread *t);
BOOL OSJoinThread(OSThread *t, int *result);
OSThread *OSGetCurrentThread(void);
void OSYieldThread(void);
BOOL OSRunThread(OSThread *t, OSThreadEntry entry, int argc, const char **argv);
OSThread *OSGetDefaultThread(uint32_t core);

typedef struct OSMutex { uint8_t data[0x2c]; } OSMutex;
void OSInitMutex(OSMutex *m);
void OSLockMutex(OSMutex *m);
void OSUnlockMutex(OSMutex *m);
typedef struct OSCondition { uint8_t data[0x1c]; } OSCondition;
void OSInitCond(OSCondition *c);
void OSWaitCond(OSCondition *c, OSMutex *m);
void OSSignalCond(OSCondition *c);
typedef struct OSEvent { uint8_t data[0x24]; } OSEvent;
void OSInitEvent(OSEvent *e, BOOL value, int mode);
void OSSignalEvent(OSEvent *e);
void OSWaitEvent(OSEvent *e);
typedef struct OSMessage { void *message; uint32_t args[3]; } OSMessage;
typedef struct OSMessageQueue { uint8_t data[0x3c]; } OSMessageQueue;
void OSInitMessageQueue(OSMessageQueue *q, OSMessage *msgs, int32_t size);
BOOL OSSendMessage(OSMessageQueue *q, OSMessage *m, uint32_t flags);
BOOL OSReceiveMessage(OSMessageQueue *q, OSMessage *m, uint32_t flags);
typedef struct OSAlarm { uint8_t data[0x58]; } __attribute__((aligned(8))) OSAlarm;
typedef void (*OSAlarmCallback)(OSAlarm *a, void *ctx);
void OSCreateAlarm(OSAlarm *a);
BOOL OSSetAlarm(OSAlarm *a, OSTime time, OSAlarmCallback cb);
BOOL OSSetPeriodicAlarm(OSAlarm *a, OSTime start, OSTime period, OSAlarmCallback cb);
BOOL OSCancelAlarm(OSAlarm *a);
uint32_t OSAddAtomic(volatile uint32_t *p, uint32_t v);

void *MEMGetBaseHeapHandle(int arena);
void *MEMAllocFromExpHeapEx(void *heap, uint32_t size, int align);
void MEMFreeToExpHeap(void *heap, void *p);
void *MEMAllocFromFrmHeapEx(void *heap, uint32_t size, int align);
void MEMFreeToFrmHeap(void *heap, int mode);
extern void *(*MEMAllocFromDefaultHeapEx)(uint32_t size, int align);
extern void (*MEMFreeToDefaultHeap)(void *p);

void OSScreenInit(void);
uint32_t OSScreenGetBufferSizeEx(int screen);
void OSScreenSetBufferEx(int screen, void *buf);
void OSScreenEnableEx(int screen, BOOL on);
void OSScreenClearBufferEx(int screen, uint32_t rgba);
void OSScreenPutFontEx(int screen, uint32_t col, uint32_t row, const char *s);
void OSScreenPutPixelEx(int screen, uint32_t x, uint32_t y, uint32_t rgba);
void OSScreenFlipBuffersEx(int screen);
void DCFlushRange(void *p, uint32_t n);

/* vpad */
typedef struct VPADStatus {
	uint32_t hold, trigger, release;
	float lx, ly, rx, ry;
	uint8_t rest[0xac - 0x1c];
} VPADStatus;
int32_t VPADRead(int chan, VPADStatus *buf, uint32_t n, int32_t *err);

/* proc_ui */
uint32_t ProcUIProcessMessages(BOOL block);
void ProcUIInit(void *cb);
