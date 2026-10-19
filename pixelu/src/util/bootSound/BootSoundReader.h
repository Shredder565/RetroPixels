// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/Filesystem/fsc.h"

// Streams the boot sound of a title (bootSound.btsnd). The file starts with
// two big-endian words (a mute mask and the loop point in stereo frames)
// followed by big-endian 16-bit stereo PCM. Playback loops endlessly from the
// loop point.
class BootSoundReader
{
public:
	BootSoundReader() = delete;
	// blockSize: bytes returned per getSamples() call, a whole number of stereo frames
	BootSoundReader(FSCVirtualFile* bootsndFile, sint32 blockSize);

	// next block as host-endian samples, nullptr on a read error
	sint16* getSamples();

private:
	size_t loopOffset() const { return 8 + (size_t)(uint32)m_loopFrame * 4; }

	FSCVirtualFile* m_file{};
	sint32 m_blockBytes{};
	uint32be m_muteMask{};
	uint32be m_loopFrame{};
	std::vector<sint16> m_samples{};
	std::vector<sint16be> m_raw{};
};
