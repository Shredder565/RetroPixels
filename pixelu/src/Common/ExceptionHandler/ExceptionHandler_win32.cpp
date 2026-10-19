// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "Cafe/CafeSystem.h"
#include "ExceptionHandler.h"

#include <Windows.h>
#include <Dbghelp.h>
#include <shellapi.h>

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"

// implemented by the PPC debugger
void debugger_handleSingleStepException(uint64 dr6);

void DumpThreadStackTrace();

namespace
{
	// file name part of a module's path
	std::string moduleFileName(HMODULE module)
	{
		char path[512]{};
		GetModuleFileNameA(module, path, (DWORD)sizeof(path));
		const char* name = path;
		for (const char* p = path; *p; p++)
		{
			if (*p == '\\' || *p == '/')
				name = p + 1;
		}
		return name;
	}

	HMODULE moduleContaining(const void* address)
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module))
			return nullptr;
		return module;
	}

	// "yyyymmdd_hhmmss" in UTC, used for the crash dump file names
	std::string utcTimestamp()
	{
		const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm t{};
		gmtime_s(&t, &now);
		return fmt::format("{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}", 1900 + t.tm_year, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
	}

	// leave out the image of modules that are only on the module list
	BOOL CALLBACK miniDumpFilter(PVOID, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output)
	{
		if (!input || !output)
			return FALSE;
		switch (input->CallbackType)
		{
		case IncludeModuleCallback:
		case IncludeThreadCallback:
		case ThreadCallback:
		case ThreadExCallback:
			return TRUE;
		case ModuleCallback:
			if ((output->ModuleWriteFlags & ModuleReferencedByMemory) == 0)
				output->ModuleWriteFlags &= ~ModuleWriteModule;
			return TRUE;
		default:
			return FALSE;
		}
	}

	// writes <user data>/crashdump/crash_<timestamp>.dmp; true if written or disabled
	bool writeMiniDump(CrashDump mode, EXCEPTION_POINTERS* exceptionPointers)
	{
		if (mode == CrashDump::Disabled)
			return true;
		fs::path dir = ActiveSettings::GetUserDataPath("crashdump");
		std::error_code ec;
		fs::create_directories(dir, ec);
		if (ec)
			return false;
		const fs::path file = dir / fmt::format("crash_{}.dmp", utcTimestamp());
		HANDLE hFile = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
		exceptionInfo.ThreadId = GetCurrentThreadId();
		exceptionInfo.ExceptionPointers = exceptionPointers;
		exceptionInfo.ClientPointers = FALSE;

		MINIDUMP_CALLBACK_INFORMATION callback{};
		callback.CallbackRoutine = (MINIDUMP_CALLBACK_ROUTINE)miniDumpFilter;
		callback.CallbackParam = nullptr;

		const DWORD flags = mode == CrashDump::Full
								? (MiniDumpWithPrivateReadWriteMemory | MiniDumpWithDataSegs | MiniDumpWithHandleData |
								   MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
								: (MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
		const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, (MINIDUMP_TYPE)flags, &exceptionInfo, nullptr, &callback);
		CloseHandle(hFile);
		return ok != FALSE;
	}

	void writeCrashLog(EXCEPTION_POINTERS* exceptionPointers, PCONTEXT ctx)
	{
		if (!CrashLog_Create())
			return; // a crash log is already being written

		const CrashDump dumpMode = GetConfig().crash_dump.GetValue();
		const bool dumpWritten = writeMiniDump(dumpMode, exceptionPointers);
		if (!dumpWritten)
			cemuLog_writeLineToLog(fmt::format("couldn't write minidump {:#x}", GetLastError()), false, true);

		cemuLog_writePlainToLog(fmt::format("\nCrashlog for {}\n", BUILD_VERSION_WITH_NAME_STRING));
		SYSTEMTIME now;
		GetSystemTime(&now);
		cemuLog_writePlainToLog(fmt::format("Date: {:02d}-{:02d}-{:04d} {:02d}:{:02d}:{:02d}\n\n", now.wDay, now.wMonth, now.wYear, now.wHour, now.wMinute, now.wSecond));

		DumpThreadStackTrace();

		if (const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord)
		{
			const uint64 address = (uint64)record->ExceptionAddress;
			if (HMODULE module = moduleContaining(record->ExceptionAddress))
				cemuLog_writePlainToLog(fmt::format("Exception 0x{:08x} at 0x{:x}(+0x{:x}) in module {}\n", (uint32)record->ExceptionCode, address, address - (uint64)module, moduleFileName(module)));
			else
				cemuLog_writePlainToLog(fmt::format("Exception 0x{:08x} at 0x{:x}\n", (uint32)record->ExceptionCode, address));
		}
		cemuLog_writePlainToLog(fmt::format("cemu.exe at 0x{:x}\n", (uint64)GetModuleHandle(nullptr)));

		cemuLog_writePlainToLog("\n");
#if defined(_M_X64)
		cemuLog_writePlainToLog(fmt::format("RAX={:016x} RBX={:016x} RCX={:016x} RDX={:016x}\n", ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx));
		cemuLog_writePlainToLog(fmt::format("RSP={:016x} RBP={:016x} RDI={:016x} RSI={:016x}\n", ctx->Rsp, ctx->Rbp, ctx->Rdi, ctx->Rsi));
		cemuLog_writePlainToLog(fmt::format("R8 ={:016x} R9 ={:016x} R10={:016x} R11={:016x}\n", ctx->R8, ctx->R9, ctx->R10, ctx->R11));
		cemuLog_writePlainToLog(fmt::format("R12={:016x} R13={:016x} R14={:016x} R15={:016x}\n", ctx->R12, ctx->R13, ctx->R14, ctx->R15));
#endif

		// the emulator state only goes to the log file
		CrashLog_SetOutputChannels(false, true);
		ExceptionHandler_LogGeneralInfo();
		CrashLog_SetOutputChannels(true, true);

		cemuLog_waitForFlush();

		// keep a copy of the log next to the dump
		if (dumpWritten && dumpMode != CrashDump::Disabled)
		{
			const fs::path logCopy = ActiveSettings::GetUserDataPath("crashdump") / fmt::format("log_{}.txt", utcTimestamp());
			std::error_code ec;
			fs::copy_file(ActiveSettings::GetUserDataPath("log.txt"), logCopy, ec);
		}
		exit(0);
	}

	// debug register hits (PPC debugger breakpoints and GDB stub watchpoints)
	LONG WINAPI onVectoredException(PEXCEPTION_POINTERS exceptionPointers)
	{
		if (exceptionPointers->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
			return EXCEPTION_CONTINUE_SEARCH;
		const uint64 dr6 = exceptionPointers->ContextRecord->Dr6;
		if (dr6 & 0b0011) // DR0 or DR1
			debugger_handleSingleStepException(dr6);
		else if (dr6 & 0b1100) // DR2 or DR3
		{
			if (g_gdbstub)
				g_gdbstub->HandleAccessException(dr6);
		}
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* exceptionPointers)
	{
		writeCrashLog(exceptionPointers, exceptionPointers->ContextRecord);
		return EXCEPTION_NONCONTINUABLE_EXCEPTION;
	}
}

// Writes the host call stack of the calling thread to log.txt. Also used by
// the wxWidgets assert handler.
void DumpThreadStackTrace()
{
	HANDLE process = GetCurrentProcess();
	SymInitialize(process, nullptr, TRUE);

	void* frames[100];
	const USHORT frameCount = CaptureStackBackTrace(0, 40, frames, nullptr);

	constexpr size_t kMaxName = 255;
	std::vector<uint8> symbolStorage(sizeof(SYMBOL_INFO) + kMaxName + 1, 0);
	SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolStorage.data();
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen = (ULONG)kMaxName;

	CrashLog_WriteHeader("Stack trace");
	for (USHORT i = 0; i < frameCount; i++)
	{
		const DWORD64 address = (DWORD64)frames[i];
		symbol->Name[0] = '\0';
		SymFromAddr(process, address, nullptr, symbol);

		std::string line = fmt::format("0x{:016x} ", (uint64)address);
		if (HMODULE module = moduleContaining(frames[i]))
		{
			const uint32 offset = (uint32)(address - (DWORD64)module);
			line += fmt::format("+0x{:08x} {:<16}", offset, moduleFileName(module));
		}
		else
			line += fmt::format("+0x00000000 {:<16}", "NULL");
		line += fmt::format(" {}\n", (const char*)symbol->Name);
		cemuLog_writePlainToLog(line);
	}
}

void ExceptionHandler_Init()
{
	SetUnhandledExceptionFilter(onUnhandledException);
	AddVectoredExceptionHandler(1, onVectoredException);
	SetErrorMode(SEM_FAILCRITICALERRORS);
}
