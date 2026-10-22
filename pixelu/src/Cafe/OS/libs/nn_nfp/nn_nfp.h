// Original code for RetroPixels (PixelU). Not derived from Cemu.
// nn_nfp: amiibo figures (NTAG215 tags read through the GamePad).
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace nn::nfp
{
	// nfc's NFCGetTagInfo is routed here while a game uses nn_nfp
	uint32 NFCGetTagInfo(uint32 index, uint32 timeout, MPTR functionPtr, void* userParam);

	COSModule* GetModule();
}

// called by the scheduler: lifts the amiibo off the reader after a while
void nnNfp_update();

bool nnNfp_isInitialized();
// GUI (through nfc::TouchTagFromFile): put an amiibo dump on the reader
bool nnNfp_touchNfcTagFromFile(const fs::path& filePath, uint32* nfcError);
