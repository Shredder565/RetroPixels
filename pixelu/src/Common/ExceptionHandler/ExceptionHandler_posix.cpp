// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include <signal.h>
#include <execinfo.h>
#include <string.h>
#include <string>
#include "config/CemuConfig.h"
#include "util/helpers/StringHelpers.h"
#include "ExceptionHandler.h"

#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "Cafe/HW/Espresso/Debugger/GDBBreakpoints.h"

#if BOOST_OS_LINUX
	#include "ELFSymbolTable.h"
#endif

namespace
{
#if BOOST_OS_LINUX
	// backtrace_symbols() lines look like "binary(symbol+0xoffset) [0xaddress]".
	// For functions that are not in the dynamic symbol table the symbol part is
	// empty; those are looked up in the executable's static symbol table.
	// Symbols are written demangled.
	void writeBacktrace(char** lines, size_t count)
	{
		ELFSymbolTable staticSymbols;
		for (size_t n = 0; n < count; n++)
		{
			const std::string_view line(lines[n]);
			const size_t open = line.rfind('(');
			const size_t close = line.rfind(')');
			const size_t plus = line.rfind('+');
			if (open == std::string_view::npos || close == std::string_view::npos || plus == std::string_view::npos ||
				plus < open || plus > close)
			{
				CrashLog_WriteLine(line);
				continue;
			}

			std::string_view symbol = line.substr(open + 1, plus - open - 1);
			uint64 offsetInSymbol = (uint64)-1;
			std::string symbolStorage;
			if (symbol.empty())
			{
				const uint64 fileOffset = (uint64)StringHelpers::ToInt64(line.substr(plus + 1, close - plus - 1));
				symbolStorage = std::string(staticSymbols.OffsetToSymbol(fileOffset, offsetInSymbol));
			}
			else
				symbolStorage = std::string(symbol);

			CrashLog_WriteLine(line.substr(0, open + 1), false);
			CrashLog_WriteLine(boost::core::demangle(symbolStorage.c_str()), false);
			CrashLog_WriteLine("+", false);
			if (offsetInSymbol != (uint64)-1)
			{
				CrashLog_WriteLine(fmt::format("0x{:x}", offsetInSymbol), false);
				CrashLog_WriteLine(line.substr(close));
			}
			else
				CrashLog_WriteLine(line.substr(plus + 1));
		}
	}
#endif

	// SIGSEGV, SIGABRT and the other signals that would normally dump core:
	// write a crash log with a host backtrace and the emulated system state,
	// then either re-raise for a core dump (when enabled) or exit
	void onFatalSignal(int sig, siginfo_t* info, void* context)
	{
#if defined(ARCH_X86_64) && BOOST_OS_LINUX
		// hardware watchpoints set by the GDB stub arrive as SIGTRAP
		if (info->si_signo == SIGTRAP && info->si_code == TRAP_HWBKPT)
		{
			if (g_gdbstub)
				g_gdbstub->HandleAccessException(_ReadDR6());
			return;
		}
#endif
		if (!CrashLog_Create())
			return; // a crash log is already being written

		const char* description = strsignal(sig);
		printf("%s!\n", description ? description : "Unknown core dumping signal");

		void* frames[128];
		const int frameCount = backtrace(frames, 128);
#if defined(ARCH_X86_64) && BOOST_OS_LINUX
		// the innermost frame is the signal handler, show the faulting instruction instead
		frames[0] = (void*)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP];
#endif

		CrashLog_WriteLine(fmt::format("Error: signal {}:", sig));
#if BOOST_OS_LINUX
		if (char** lines = backtrace_symbols(frames, frameCount))
		{
			writeBacktrace(lines, (size_t)frameCount);
			free(lines);
		}
		else
			CrashLog_WriteLine("Failed to read backtrace");
#else
		backtrace_symbols_fd(frames, frameCount, STDERR_FILENO);
#endif

		std::cerr << "\nStacktrace and additional info written to:" << std::endl;
		std::cerr << cemuLog_GetLogFilePath().generic_string() << std::endl;

		// the emulator state only goes to the log file
		CrashLog_SetOutputChannels(false, true);
		ExceptionHandler_LogGeneralInfo();
		CrashLog_SetOutputChannels(true, true);

		if (GetConfig().crash_dump == CrashDump::Enabled)
		{
			signal(sig, SIG_DFL);
			raise(sig);
			return;
		}
		_Exit(1);
	}

	// Ctrl+C / termination request. There is no orderly shutdown path from a
	// signal handler (destroying running threads aborts), so exit right away.
	void onTerminateSignal(int)
	{
		_Exit(0);
	}
}

void ExceptionHandler_Init()
{
	struct sigaction terminate{};
	sigfillset(&terminate.sa_mask); // block other signals while the handler runs
	terminate.sa_handler = onTerminateSignal;
	sigaction(SIGINT, &terminate, nullptr);
	sigaction(SIGTERM, &terminate, nullptr);

	struct sigaction fatal{};
	sigfillset(&fatal.sa_mask);
	fatal.sa_flags = SA_SIGINFO;
	fatal.sa_sigaction = onFatalSignal;
	for (int sig : {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGIOT, SIGQUIT, SIGSEGV, SIGSYS, SIGTRAP})
		sigaction(sig, &fatal, nullptr);
}
