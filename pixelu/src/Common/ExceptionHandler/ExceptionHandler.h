// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// installs the platform crash handlers (call once at startup)
void ExceptionHandler_Init();

#if !BOOST_OS_WINDOWS
// SIGINT and SIGTERM end PixelU at once. After this call the first of them only sets a flag, read with
// ExceptionHandler_StopRequested, so the GUI can close the normal way (finishing a recording); a second
// one still ends PixelU at once.
void ExceptionHandler_DeferStopSignal();
bool ExceptionHandler_StopRequested();
#endif

// Crash log output. CrashLog_Create opens log.txt for the crash report and
// returns false if a crash log was already started (only one per run).
bool CrashLog_Create();
void CrashLog_SetOutputChannels(bool writeToStdErr, bool writeToLogTxt);
void CrashLog_WriteLine(std::string_view text, bool newLine = true);
void CrashLog_WriteHeader(const char* header);

// writes the emulated system state (title, PPC registers and threads) to the crash log
void ExceptionHandler_LogGeneralInfo();
