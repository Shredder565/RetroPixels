// Raw 3DS system calls and IPC for the PixelDS test program (our own
// code; it follows the documented 3DS calling convention).
typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef long long s64;

static inline u32 *cmdbuf(void) {
	u32 tls;
	__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tls));
	return (u32 *)(tls + 0x80);
}

// A system call with up to six arguments in r0-r5 and results back in
// r0-r3.
struct regs { u32 r0, r1, r2, r3; };

#define DEFINE_SVC(name, num)                                                         \
	static inline struct regs name(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5) { \
		register u32 r0 __asm__("r0") = a0;                                           \
		register u32 r1 __asm__("r1") = a1;                                           \
		register u32 r2 __asm__("r2") = a2;                                           \
		register u32 r3 __asm__("r3") = a3;                                           \
		register u32 r4 __asm__("r4") = a4;                                           \
		register u32 r5 __asm__("r5") = a5;                                           \
		__asm__ volatile("svc " #num                                                  \
				 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5)  \
				 :                                                            \
				 : "memory", "r12", "lr");                                    \
		struct regs o = {r0, r1, r2, r3};                                             \
		return o;                                                                     \
	}

DEFINE_SVC(svc_ControlMemory, 0x01)
DEFINE_SVC(svc_CreateThread, 0x08)
DEFINE_SVC(svc_ExitThread, 0x09)
DEFINE_SVC(svc_SleepThread, 0x0A)
DEFINE_SVC(svc_CreateMutex, 0x13)
DEFINE_SVC(svc_ReleaseMutex, 0x14)
DEFINE_SVC(svc_CreateEvent, 0x17)
DEFINE_SVC(svc_SignalEvent, 0x18)
DEFINE_SVC(svc_CreateTimer, 0x1A)
DEFINE_SVC(svc_SetTimer, 0x1B)
DEFINE_SVC(svc_MapMemoryBlock, 0x1F)
DEFINE_SVC(svc_CreateAddressArbiter, 0x21)
DEFINE_SVC(svc_ArbitrateAddress, 0x22)
DEFINE_SVC(svc_WaitSynchronization1, 0x24)
DEFINE_SVC(svc_WaitSynchronizationN, 0x25)
DEFINE_SVC(svc_GetSystemTick, 0x28)
DEFINE_SVC(svc_ConnectToPort, 0x2D)
DEFINE_SVC(svc_SendSyncRequest, 0x32)

// srv:GetServiceHandle
static inline s32 getService(u32 srv, const char *name, u32 *out) {
	u32 *c = cmdbuf();
	u32 n = 0;
	while (name[n]) n++;
	c[0] = 0x00050100;
	c[1] = c[2] = 0;
	for (u32 i = 0; i < n; i++) ((u8 *)&c[1])[i] = name[i];
	c[3] = n;
	c[4] = 0;
	struct regs r = svc_SendSyncRequest(srv, 0, 0, 0, 0, 0);
	if (r.r0) return r.r0;
	*out = c[3];
	return c[1];
}
