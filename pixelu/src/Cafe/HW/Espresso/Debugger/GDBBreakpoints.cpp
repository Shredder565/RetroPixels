// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "GDBBreakpoints.h"
#include "Debugger.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"

#if defined(ARCH_X86_64) && BOOST_OS_LINUX
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <unistd.h>
#endif

namespace coreinit
{
#if BOOST_OS_LINUX
	std::vector<pid_t>& OSGetSchedulerThreadIds();
#endif
	std::vector<std::thread::native_handle_type>& OSGetSchedulerThreads();
}

namespace
{
	void WriteGuestCodeWord(MPTR address, uint32 word)
	{
		memory_writeU32(address, word);
		PPCRecompiler_invalidateRange(address, address + 4);
	}

#if defined(ARCH_X86_64)
	// DR7 layout for the two slots owned by the GDB stub
	//   slot 2: local enable bit 4, R/W bits 24-25, LEN bits 26-27
	//   slot 3: local enable bit 6, R/W bits 28-29, LEN bits 30-31
	// R/W: 1 = data write, 3 = data read or write. LEN: 3 = four bytes.
	template<typename T>
	void ConfigureWatchSlots(T& dr7, bool enable)
	{
		const uint32 on = enable ? 1 : 0;
		SetBits(dr7, 4, 1, on);
		SetBits(dr7, 24, 2, enable ? 1 : 0);
		SetBits(dr7, 26, 2, enable ? 3 : 0);
		SetBits(dr7, 6, 1, on);
		SetBits(dr7, 28, 2, enable ? 3 : 0);
		SetBits(dr7, 30, 2, enable ? 3 : 0);
	}
#endif
}

#if defined(ARCH_X86_64) && BOOST_OS_LINUX

namespace
{
	size_t DebugRegisterUserOffset(int drIndex)
	{
		return offsetof(struct user, u_debugreg) + sizeof(((struct user*)nullptr)->u_debugreg[0]) * drIndex;
	}

	// Linux refuses ptrace on threads of our own thread group, so the
	// debug registers are programmed from a short-lived child process.
	template<typename TFunc>
	void RunInTracerChild(TFunc&& body)
	{
		pid_t child = fork();
		if (child < 0)
		{
			perror("fork");
			return;
		}
		if (child == 0)
		{
			body();
			_exit(0);
		}
		waitpid(child, nullptr, 0);
	}

	bool AttachAndWait(pid_t tid)
	{
		if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) == -1)
		{
			perror("ptrace(PTRACE_ATTACH)");
			return false;
		}
		waitpid(tid, nullptr, 0);
		return true;
	}

	void Detach(pid_t tid)
	{
		if (ptrace(PTRACE_DETACH, tid, nullptr, nullptr) == -1)
			perror("ptrace(PTRACE_DETACH)");
	}

	void ProgramWatchSlotsOnAllCores(DRType address, bool enable)
	{
		RunInTracerChild([&]() {
			for (pid_t tid : coreinit::OSGetSchedulerThreadIds())
			{
				AttachAndWait(tid);
				DRType dr7 = _GetDR(tid, 7);
				ConfigureWatchSlots(dr7, enable);
				_SetDR(tid, 2, address);
				_SetDR(tid, 3, address);
				_SetDR(tid, 7, dr7);
				Detach(tid);
			}
		});
	}
}

DRType _GetDR(pid_t tid, int drIndex)
{
	long value = ptrace(PTRACE_PEEKUSER, tid, DebugRegisterUserOffset(drIndex), nullptr);
	if (value == -1)
		perror("ptrace(PTRACE_PEEKUSER)");
	return (DRType)value;
}

void _SetDR(pid_t tid, int drIndex, DRType newValue)
{
	if (ptrace(PTRACE_POKEUSER, tid, DebugRegisterUserOffset(drIndex), newValue) == -1)
		perror("ptrace(PTRACE_POKEUSER)");
}

DRType _ReadDR6()
{
	const pid_t self = gettid();
	pid_t child = fork();
	if (child < 0)
	{
		perror("fork");
		return 0;
	}
	if (child == 0)
	{
		int code = 0;
		if (ptrace(PTRACE_ATTACH, self, nullptr, nullptr) == 0)
		{
			waitpid(self, nullptr, 0);
			// the exit status only carries 8 bits; the hit flags B0-B3 are in the lowest nibble
			code = (int)(_GetDR(self, 6) & 0xFF);
			Detach(self);
		}
		else
			perror("ptrace(PTRACE_ATTACH)");
		_exit(code);
	}
	int status = 0;
	waitpid(child, &status, 0);
	return (DRType)WEXITSTATUS(status);
}

#elif defined(ARCH_X86_64) && BOOST_OS_WINDOWS

namespace
{
	void ProgramWatchSlotsOnAllCores(DWORD64 address, bool enable)
	{
		for (auto& nativeHandle : coreinit::OSGetSchedulerThreads())
		{
			HANDLE thread = (HANDLE)nativeHandle;
			SuspendThread(thread);
			CONTEXT context{};
			context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			GetThreadContext(thread, &context);
			context.Dr2 = address;
			context.Dr3 = address;
			ConfigureWatchSlots(context.Dr7, enable);
			SetThreadContext(thread, &context);
			ResumeThread(thread);
		}
	}
}

#endif

/* CodeTrap */

GDBServer::CodeTrap::CodeTrap(MPTR address, BreakpointType type, bool visibleToClient, std::string stopReason)
	: m_address(address), m_stopReason(std::move(stopReason))
{
	m_stops = (type == BreakpointType::BP_SINGLE || type == BreakpointType::BP_PERSISTENT);
	m_staysAfterHit = (type == BreakpointType::BP_PERSISTENT);
	m_droppedOnAnyTrap = (type == BreakpointType::BP_STEP_POINT);
	m_stopOnce = (type == BreakpointType::BP_STEP_POINT);
	m_visibleToClient = m_stops ? visibleToClient : false;

	m_savedInstruction = memory_readU32(address);
	WriteGuestCodeWord(address, DEBUGGER_BP_T_GDBSTUB_TW);
}

GDBServer::CodeTrap::~CodeTrap()
{
	if (!m_disarmed)
		WriteGuestCodeWord(m_address, m_savedInstruction);
}

uint32 GDBServer::CodeTrap::ClientVisibleWord() const
{
	if (m_disarmed)
		return memory_readU32(m_address);
	return m_visibleToClient ? memory_readU32(m_address) : m_savedInstruction;
}

void GDBServer::CodeTrap::Disarm()
{
	if (m_disarmed)
		return;
	WriteGuestCodeWord(m_address, m_savedInstruction);
	m_disarmed = true;
}

void GDBServer::CodeTrap::Rearm()
{
	if (!m_disarmed)
		return;
	m_savedInstruction = memory_readU32(m_address);
	WriteGuestCodeWord(m_address, DEBUGGER_BP_T_GDBSTUB_TW);
	m_disarmed = false;
}

/* Watchpoint */

GDBServer::Watchpoint::Watchpoint(MPTR address, AccessPointType type)
	: m_address(address), m_type(type)
{
#if defined(ARCH_X86_64) && BOOST_OS_LINUX
	ProgramWatchSlotsOnAllCores((DRType)memory_getPointerFromVirtualOffset(address), true);
#elif defined(ARCH_X86_64) && BOOST_OS_WINDOWS
	ProgramWatchSlotsOnAllCores((DWORD64)memory_getPointerFromVirtualOffset(address), true);
#else
	cemuLog_log(LogType::Force, "Debugger read/write breakpoints are not supported on non-x86 CPUs yet.");
#endif
}

GDBServer::Watchpoint::~Watchpoint()
{
#if defined(ARCH_X86_64) && BOOST_OS_LINUX
	ProgramWatchSlotsOnAllCores(0, false);
#elif defined(ARCH_X86_64) && BOOST_OS_WINDOWS
	ProgramWatchSlotsOnAllCores(0, false);
#endif
}
