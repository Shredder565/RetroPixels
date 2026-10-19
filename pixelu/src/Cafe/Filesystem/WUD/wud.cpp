// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wud.h"
#include "Common/FileStream.h"

// .wux layout (little endian):
//   0x00 "WUX0"  0x04 magic 0x1099D02E  0x08 sector size  0x10 uncompressed size (u64)
//   0x20 table with one u32 per sector of the image: index into the sector array
//   the sector array follows, starting at the next multiple of the sector size
struct wud_t
{
	std::unique_ptr<FileStream> file;
	long long imageSize{0};
	bool isWux{false};
	uint32 sectorSize{0};
	std::vector<uint32> sectorMap; // image sector -> stored sector
	long long storedSectorsOffset{0};
};

namespace
{
	constexpr size_t kWuxHeaderSize = 0x20;
	constexpr uint32 kWuxMagic0 = 0x30585557; // "WUX0"
	constexpr uint32 kWuxMagic1 = 0x1099D02E;

	uint32 LoadLE32(const uint8* p)
	{
		return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
	}

	uint64 LoadLE64(const uint8* p)
	{
		return (uint64)LoadLE32(p) | ((uint64)LoadLE32(p + 4) << 32);
	}

	bool SetUpWux(wud_t& wud, const uint8* header, uint64 fileSize)
	{
		wud.isWux = true;
		wud.sectorSize = LoadLE32(header + 0x08);
		wud.imageSize = (long long)LoadLE64(header + 0x10);
		if (wud.sectorSize < 0x100 || wud.sectorSize >= 0x10000000)
			return false;
		const uint64 sectorCount = ((uint64)wud.imageSize + wud.sectorSize - 1) / wud.sectorSize;
		if (sectorCount > (fileSize - kWuxHeaderSize) / 4)
			return false; // the sector table cannot be complete
		wud.sectorMap.resize(sectorCount);
		const uint64 tableEnd = kWuxHeaderSize + sectorCount * 4;
		wud.storedSectorsOffset = (long long)((tableEnd + wud.sectorSize - 1) / wud.sectorSize * wud.sectorSize);
		std::vector<uint8> table(sectorCount * 4);
		wud.file->SetPosition(kWuxHeaderSize);
		if (wud.file->readData(table.data(), (uint32)table.size()) != table.size())
			return false;
		for (size_t i = 0; i < sectorCount; i++)
			wud.sectorMap[i] = LoadLE32(table.data() + i * 4);
		return true;
	}
}

wud_t* wud_open(const fs::path& path)
{
	auto wud = std::make_unique<wud_t>();
	wud->file.reset(FileStream::openFile2(path));
	if (!wud->file)
		return nullptr;
	const uint64 fileSize = wud->file->GetSize();
	uint8 header[kWuxHeaderSize];
	if (wud->file->readData(header, sizeof(header)) != sizeof(header))
		return nullptr; // too small for either format
	if (LoadLE32(header) == kWuxMagic0 && LoadLE32(header + 4) == kWuxMagic1)
	{
		if (!SetUpWux(*wud, header, fileSize))
			return nullptr;
	}
	else
		wud->imageSize = (long long)fileSize;
	return wud.release();
}

void wud_close(wud_t* wud)
{
	delete wud;
}

bool wud_isWUXCompressed(wud_t* wud)
{
	return wud->isWux;
}

unsigned int wud_readData(wud_t* wud, void* buffer, unsigned int length, long long offset)
{
	if (offset >= wud->imageSize)
		return 0;
	if ((long long)length > wud->imageSize - offset)
		length = (unsigned int)(wud->imageSize - offset);
	if (!wud->isWux)
	{
		wud->file->SetPosition((uint64)offset);
		return wud->file->readData(buffer, length);
	}
	// .wux: read sector by sector through the sector map
	uint8* out = (uint8*)buffer;
	unsigned int total = 0;
	while (length > 0)
	{
		const uint64 imageSector = (uint64)offset / wud->sectorSize;
		const uint32 inSector = (uint32)((uint64)offset % wud->sectorSize);
		const uint32 chunk = std::min<uint32>(length, wud->sectorSize - inSector);
		const long long stored = wud->storedSectorsOffset + (long long)wud->sectorMap[imageSector] * wud->sectorSize + inSector;
		wud->file->SetPosition((uint64)stored);
		total += wud->file->readData(out, chunk);
		out += chunk;
		offset += chunk;
		length -= chunk;
	}
	return total;
}

long long wud_getWUDSize(wud_t* wud)
{
	return wud->imageSize;
}
