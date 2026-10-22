// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "GDBStub.h"
#include <utility>

#if defined(ARCH_X86_64) && BOOST_OS_LINUX
#include <sys/types.h>

// x86 debug register access for other threads of this process (via ptrace
// from a forked helper, since a process cannot trace its own threads)
typedef unsigned long DRType;

DRType _GetDR(pid_t tid, int drIndex);
void _SetDR(pid_t tid, int drIndex, DRType newValue);
// DR6 of the calling thread (only the low 8 bits are returned)
DRType _ReadDR6();
#endif

// how a code trap behaves when it is hit
enum class BreakpointType
{
	BP_SINGLE,        // stop once, then disappear
	BP_PERSISTENT,    // stop every time (client breakpoint)
	BP_RESTORE_POINT, // never stops, re-arms disarmed breakpoints when hit
	BP_STEP_POINT,    // stops once and is dropped on the next trap anywhere
};

// A trap instruction placed into guest code. The replaced instruction is
// written back when the object is destroyed.
class GDBServer::CodeTrap
{
public:
	CodeTrap(MPTR address, BreakpointType type, bool visibleToClient, std::string stopReason);
	~CodeTrap();
	CodeTrap(const CodeTrap&) = delete;
	CodeTrap& operator=(const CodeTrap&) = delete;

	// word the client sees when it reads memory at this address
	uint32 ClientVisibleWord() const;
	// the instruction that was replaced by the trap
	uint32 SavedInstruction() const { return m_savedInstruction; }
	void SetSavedInstruction(uint32 instruction) { m_savedInstruction = instruction; }

	bool StopsThreads() const { return m_stops; }
	// true once after RequestStopOnce()
	bool ConsumeStopRequest()
	{
		bool pending = m_stopOnce;
		m_stopOnce = false;
		return pending;
	}
	void RequestStopOnce() { m_stopOnce = true; }
	bool StaysAfterHit() const { return m_staysAfterHit; }
	bool DroppedOnAnyTrap() const { return m_droppedOnAnyTrap; }
	const std::string& StopReason() const { return m_stopReason; }

	// puts the original instruction back so the thread can execute it
	void Disarm();
	// writes the trap instruction again after Disarm()
	void Rearm();
	bool IsDisarmed() const { return m_disarmed; }

private:
	const MPTR m_address;
	std::string m_stopReason;
	uint32 m_savedInstruction;
	bool m_visibleToClient;
	bool m_stops;
	bool m_stopOnce;
	bool m_staysAfterHit;
	bool m_droppedOnAnyTrap;
	bool m_disarmed = false;
};

// Z2/Z3/Z4 watchpoint types
enum class AccessPointType
{
	BP_WRITE = 2,
	BP_READ = 3,
	BP_BOTH = 4
};

// Data watchpoint using host debug registers DR2 (writes) and DR3 (reads
// and writes) of every scheduler thread. DR0/DR1 belong to the built-in
// debugger.
class GDBServer::Watchpoint
{
public:
	Watchpoint(MPTR address, AccessPointType type);
	~Watchpoint();
	Watchpoint(const Watchpoint&) = delete;
	Watchpoint& operator=(const Watchpoint&) = delete;

	MPTR GetAddress() const { return m_address; }
	AccessPointType GetType() const { return m_type; }

private:
	const MPTR m_address;
	const AccessPointType m_type;
};
