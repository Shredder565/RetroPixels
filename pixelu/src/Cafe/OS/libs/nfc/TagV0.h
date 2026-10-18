// Original code for RetroPixels (PixelU). Not derived from Cemu.
// NFC Forum Type 1 tag image (512 bytes, 64 blocks of 8 bytes) as stored in
// the tag files the user touches.
//
// Block 0 holds the UID, block 0x0D is reserved, blocks 0x0E/0x0F hold the
// lock bits: bytes 0-1 of block 0x0E lock blocks 0x00-0x0F, bytes 2-7 of
// block 0x0F lock blocks 0x10-0x3F (bit 0 = lowest block). Locked blocks form
// the read-only "locked area"; every other non-reserved block, in order, forms
// the data area: a 4-byte capability container followed by TLVs, one of which
// carries the NDEF message.
#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "TLV.h"

namespace nfc
{
	class Type1Tag
	{
	public:
		static constexpr size_t kImageSize = 512;
		static constexpr size_t kBlockSize = 8;
		static constexpr size_t kBlockCount = kImageSize / kBlockSize;

		// nullptr if the image is not a usable version 1.x tag with an NDEF TLV
		static std::unique_ptr<Type1Tag> Load(std::span<const uint8> image);

		// the full image with the current NDEF message written into the data
		// area; false if the message no longer fits
		bool Save(std::vector<uint8>& image) const;

		const uint8* Uid() const { return m_image.data(); } // 7 bytes
		const std::vector<uint8>& Ndef() const { return m_tlvs[m_ndefIndex].value; }
		const std::vector<uint8>& LockedArea() const { return m_lockedArea; }
		void SetNdef(std::vector<uint8> message) { m_tlvs[m_ndefIndex].value = std::move(message); }

	private:
		static bool IsReserved(size_t block);
		bool IsLocked(size_t block) const;

		std::array<uint8, kImageSize> m_image{};
		std::array<bool, kBlockCount> m_locked{};
		std::vector<uint8> m_lockedArea;
		std::array<uint8, 4> m_cc{};
		std::vector<tlv::Entry> m_tlvs;
		size_t m_ndefIndex = 0;
	};
}
