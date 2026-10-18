// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "TagV0.h"

namespace nfc
{
	namespace
	{
		constexpr uint8 kCcMagic = 0xE1;     // NDEF magic number in the capability container
		constexpr size_t kLockBlockLow = 0x0E;  // bytes 0..1: blocks 0x00..0x0F
		constexpr size_t kLockBlockHigh = 0x0F; // bytes 2..7: blocks 0x10..0x3F
	}

	bool Type1Tag::IsReserved(size_t block)
	{
		return block == 0x00 || block == 0x0D || block == 0x0E || block == 0x0F;
	}

	bool Type1Tag::IsLocked(size_t block) const
	{
		return m_locked[block];
	}

	std::unique_ptr<Type1Tag> Type1Tag::Load(std::span<const uint8> image)
	{
		if (image.size() != kImageSize)
		{
			cemuLog_log(LogType::Force, "nfc: tag file must be {} bytes, got {}", kImageSize, image.size());
			return nullptr;
		}
		auto tag = std::make_unique<Type1Tag>();
		std::copy(image.begin(), image.end(), tag->m_image.begin());

		// lock bits
		const uint8* low = tag->m_image.data() + kLockBlockLow * kBlockSize;
		const uint8* high = tag->m_image.data() + kLockBlockHigh * kBlockSize;
		for (size_t block = 0; block < kBlockCount; block++)
		{
			const uint8 bits = block < 16 ? low[block / 8] : high[2 + (block - 16) / 8];
			tag->m_locked[block] = (bits >> (block % 8)) & 1;
		}

		// split into locked area and data area
		std::vector<uint8> dataArea;
		for (size_t block = 0; block < kBlockCount; block++)
		{
			if (IsReserved(block))
				continue;
			const uint8* b = tag->m_image.data() + block * kBlockSize;
			std::vector<uint8>& dst = tag->IsLocked(block) ? tag->m_lockedArea : dataArea;
			dst.insert(dst.end(), b, b + kBlockSize);
		}

		// capability container
		if (dataArea.size() < tag->m_cc.size())
			return nullptr;
		std::copy_n(dataArea.begin(), tag->m_cc.size(), tag->m_cc.begin());
		if (tag->m_cc[0] != kCcMagic)
		{
			cemuLog_log(LogType::Force, "nfc: tag has no NDEF capability container");
			return nullptr;
		}
		if ((tag->m_cc[1] >> 4) != 1)
		{
			cemuLog_log(LogType::Force, "nfc: unsupported tag mapping version {:#x}", tag->m_cc[1]);
			return nullptr;
		}
		if ((tag->m_cc[2] + 1u) * 8u < kImageSize)
		{
			cemuLog_log(LogType::Force, "nfc: tag memory size in the capability container is too small");
			return nullptr;
		}

		if (!tlv::Parse(std::span<const uint8>(dataArea).subspan(tag->m_cc.size()), tag->m_tlvs) || tag->m_tlvs.empty())
		{
			cemuLog_log(LogType::Force, "nfc: tag has no TLVs");
			return nullptr;
		}
		auto it = std::find_if(tag->m_tlvs.begin(), tag->m_tlvs.end(), [](const tlv::Entry& e) { return e.type == tlv::kNdef; });
		if (it == tag->m_tlvs.end())
		{
			cemuLog_log(LogType::Force, "nfc: tag has no NDEF TLV");
			return nullptr;
		}
		tag->m_ndefIndex = (size_t)(it - tag->m_tlvs.begin());
		return tag;
	}

	bool Type1Tag::Save(std::vector<uint8>& image) const
	{
		std::vector<uint8> dataArea(m_cc.begin(), m_cc.end());
		for (const tlv::Entry& e : m_tlvs)
			tlv::Append(e, dataArea);

		image.assign(m_image.begin(), m_image.end());
		size_t pos = 0;
		for (size_t block = 0; block < kBlockCount; block++)
		{
			if (IsReserved(block) || IsLocked(block))
				continue;
			uint8* b = image.data() + block * kBlockSize;
			for (size_t i = 0; i < kBlockSize; i++, pos++)
				b[i] = pos < dataArea.size() ? dataArea[pos] : 0;
		}
		if (pos < dataArea.size())
		{
			cemuLog_log(LogType::Force, "nfc: NDEF message does not fit on the tag ({} of {} bytes)", dataArea.size(), pos);
			return false;
		}
		return true;
	}
}
