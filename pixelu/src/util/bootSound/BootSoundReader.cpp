// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "BootSoundReader.h"
#include "Cafe/CafeSystem.h"

namespace
{
	constexpr size_t kFrameBytes = sizeof(sint16be) * 2; // one stereo frame

	// Super Mario 3D World (all regions) ships a boot sound with a wrong loop point
	bool HasBrokenLoopPoint(uint64 titleId)
	{
		return titleId == 0x0005000010145D00 || titleId == 0x0005000010145C00 || titleId == 0x0005000010106100;
	}
}

BootSoundReader::BootSoundReader(FSCVirtualFile* bootsndFile, sint32 blockSize) : m_file(bootsndFile), m_blockBytes(blockSize)
{
	cemu_assert(blockSize % kFrameBytes == 0);

	fsc_setFileSeek(m_file, 0);
	fsc_readFile(m_file, &m_muteMask, 4);
	fsc_readFile(m_file, &m_loopFrame, 4);

	m_samples.resize(blockSize / sizeof(sint16));
	m_raw.resize(blockSize / sizeof(sint16be));

	if (HasBrokenLoopPoint(CafeSystem::GetForegroundTitleId()))
		m_loopFrame = 113074;
}

sint16* BootSoundReader::getSamples()
{
	const size_t blockBytes = (size_t)m_blockBytes;
	size_t filled = 0;
	while (filled < blockBytes)
	{
		const uint32 got = fsc_readFile(m_file, m_raw.data(), (uint32)(blockBytes - filled));
		if (got == 0)
		{
			cemuLog_log(LogType::Force, "failed to read PCM samples from bootSound.btsnd");
			return nullptr;
		}
		if (got % kFrameBytes != 0)
		{
			cemuLog_log(LogType::Force, "failed to play bootSound.btsnd: reading PCM data stopped at an odd number of samples (is the file corrupt?)");
			return nullptr;
		}
		const size_t count = got / sizeof(sint16be);
		sint16* dst = m_samples.data() + filled / sizeof(sint16);
		for (size_t i = 0; i < count; i++)
			dst[i] = m_raw[i];
		filled += got;
		if (filled < blockBytes)
			fsc_setFileSeek(m_file, (uint32)loopOffset()); // hit the end of the file, continue at the loop point
	}
	// a block ending exactly at the end of the file: wrap now so the next call starts at the loop point
	if (fsc_getFileSeek(m_file) == fsc_getFileSize(m_file))
		fsc_setFileSeek(m_file, (uint32)loopOffset());
	return m_samples.data();
}
