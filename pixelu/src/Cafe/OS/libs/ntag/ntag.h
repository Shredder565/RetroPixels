// Original code for RetroPixels (PixelU). Not derived from Cemu.
// ntag: NTAG-format NFC tags (game figures and cards).
#pragma once
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/RPL/COSModule.h"

// results reported to games
#define NTAG_RESULT_SUCCESS			(0)
#define NTAG_RESULT_UNINITIALIZED	(-0x3E7)
#define NTAG_RESULT_INVALID_STATE	(-0x3E6)
#define NTAG_RESULT_NO_TAG			(-0x3E5)
#define NTAG_RESULT_INVALID			(-0x3E1)
#define NTAG_RESULT_UID_MISMATCH	(-0x3DB)

namespace ntag
{
	// which tags the game accepts (NTAGSetFormatSettings)
	struct NTAGFormatSettings
	{
		/* +0x00 */ uint8 version;
		/* +0x01 */ uint8 pad[3];
		/* +0x04 */ uint32be makerCode;
		/* +0x08 */ uint32be identifyCode;
		/* +0x0C */ uint8 reserved[0x1C];
	};
	static_assert(sizeof(NTAGFormatSettings) == 0x28);

	sint32 NTAGInit(uint32 chan);
	sint32 NTAGInitEx(uint32 chan);
	sint32 NTAGShutdown(uint32 chan);
	bool NTAGIsInit(uint32 chan);
	void NTAGProc(uint32 chan);
	void NTAGSetFormatSettings(NTAGFormatSettings* formatSettings);
	void NTAGSetTagDetectCallback(uint32 chan, MPTR callback, void* context);
	sint32 NTAGAbort(uint32 chan, MPTR callback, void* context);
	sint32 NTAGRead(uint32 chan, uint32 timeout, nfc::NFCUid* uid, nfc::NFCUid* uidMask, MPTR callback, void* context);
	sint32 NTAGWrite(uint32 chan, uint32 timeout, nfc::NFCUid* uid, uint32 rwSize, void* rwData, MPTR callback, void* context);
	sint32 NTAGFormat(uint32 chan, uint32 timeout, nfc::NFCUid* uid, uint32 rwSize, void* rwData, MPTR callback, void* context);

	COSModule* GetModule();
}
