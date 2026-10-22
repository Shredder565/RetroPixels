// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

// TCL is the lowest layer of the Wii U graphics stack: it owns the GPU ring
// buffer. GX2 hands it finished command buffers (as indirect-buffer packets)
// and TCL appends a retire marker so the CPU can tell when the GPU is done.
namespace TCL
{
	enum class TCLTimestampId
	{
		TIMESTAMP_LAST_BUFFER_RETIRED = 1,
	};

	enum class TCLSubmissionFlag : uint32
	{
		SURFACE_SYNC = 0x400000, // flush caches before the submitted commands (not needed by our GPU)
		NO_MARKER_INTERRUPT = 0x200000, // write the retire marker without raising an interrupt
		USE_RETIRED_MARKER = 0x20000000, // count the submission with the retire marker and return its timestamp
	};

	int TCLTimestamp(TCLTimestampId id, uint64be* timestampOut);
	int TCLWaitTimestamp(TCLTimestampId id, uint64 waitTs, uint64 timeout);
	int TCLSubmitToRing(uint32be* cmd, uint32 cmdLen, betype<TCLSubmissionFlag>* controlFlags, uint64be* timestampValueOut);

	// GPU side (Latte command processor)
	bool TCLGPUReadRBWord(uint32& cmdWord);
	void TCLGPUNotifyNewRetirementTimestamp();

	COSModule* GetModule();
}
ENABLE_BITMASK_OPERATORS(TCL::TCLSubmissionFlag);
