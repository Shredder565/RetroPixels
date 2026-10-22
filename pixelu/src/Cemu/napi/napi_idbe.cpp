// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "napi.h"
#include "napi_helper.h"

#include "curl/curl.h"
#include "pugixml.hpp"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "openssl/sha.h"
#include "util/crypto/aes128.h"
#include "util/helpers/StringHelpers.h"
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"

// IDBE files hold a title's icon and its names in every language.
// File layout: u8 version (0), u8 key index, 32 byte encrypted SHA-256 of the
// icon data, then the AES-128-CBC encrypted IDBEIconDataV0.

namespace NAPI
{
	std::string IDBEIconDataV0::LanguageInfo::GetGameNameUTF8()
	{
		return StringHelpers::ToUtf8(m_shortName);
	}

	std::string IDBEIconDataV0::LanguageInfo::GetGameLongNameUTF8()
	{
		return StringHelpers::ToUtf8(m_longName);
	}

	std::string IDBEIconDataV0::LanguageInfo::GetPublisherNameUTF8()
	{
		return StringHelpers::ToUtf8(m_publisher);
	}

	namespace
	{
		constexpr size_t kIdbeHeaderSize = sizeof(IDBEHeader); // 0x22

		constexpr uint8 kIdbeKeys[4][16] =
		{
			{ 0x4A,0xB9,0xA4,0x0E,0x14,0x69,0x75,0xA8,0x4B,0xB1,0xB4,0xF3,0xEC,0xEF,0xC4,0x7B },
			{ 0x90,0xA0,0xBB,0x1E,0x0E,0x86,0x4A,0xE8,0x7D,0x13,0xA6,0xA0,0x3D,0x28,0xC9,0xB8 },
			{ 0xFF,0xBB,0x57,0xC1,0x4E,0x98,0xEC,0x69,0x75,0xB3,0x84,0xFC,0xF4,0x07,0x86,0xB5 },
			{ 0x80,0x92,0x37,0x99,0xB4,0x1F,0x36,0xA6,0xA7,0x5F,0xB8,0xB4,0x8C,0x95,0xF6,0x6F },
		};

		constexpr uint8 kIdbeHashIv[16] = { 0xA4,0x69,0x87,0xAE,0x47,0xD8,0x2B,0xB4,0xFA,0x8A,0xBC,0x04,0x50,0x28,0x5F,0xA4 };

		bool IsFriendListTitle(uint64 titleId)
		{
			return titleId == 0x000500301001500A || titleId == 0x000500301001510A || titleId == 0x000500301001520A;
		}

		std::string IconServerUrl(NetworkService service)
		{
			if (service == NetworkService::Pretendo)
				return PretendoURLs::IDBEURL;
			if (service == NetworkService::Custom)
				return GetNetworkConfig().urls.IDBE.GetValue();
			return NintendoURLs::IDBEURL;
		}
	}

	std::vector<uint8> IDBE_RequestRawEncrypted(NetworkService networkService, uint64 titleId)
	{
		// files are grouped into directories by the second lowest byte of the title id
		std::string url = IconServerUrl(networkService);
		url.append(fmt::format("/{:02X}/{:016X}.idbe", (uint32)((titleId >> 8) & 0xFF), titleId));
		CurlRequestHelper req;
		req.initate(networkService, url, CurlRequestHelper::SERVER_SSL_CONTEXT::IDBE);
		if (!req.submitRequest(false))
		{
			cemuLog_log(LogType::Force, "Failed to request IDBE icon for title {:016X}", titleId);
			return {};
		}
		return req.getReceivedData();
	}

	std::optional<IDBEIconDataV0> IDBE_Request(NetworkService networkService, uint64 titleId)
	{
		if (IsFriendListTitle(titleId))
		{
			cemuLog_logDebug(LogType::Force, "Requesting IDBE for Friend List. Return none instead");
			return std::nullopt;
		}

		std::vector<uint8> file = IDBE_RequestRawEncrypted(networkService, titleId);
		if (file.size() < kIdbeHeaderSize)
			return std::nullopt;
		IDBEHeader header;
		memcpy(&header, file.data(), kIdbeHeaderSize);
		if (header.formatVersion != 0)
		{
			cemuLog_log(LogType::Force, "IDBE_Request: File has invalid version");
			return std::nullopt;
		}
		if (header.keyIndex >= std::size(kIdbeKeys))
		{
			cemuLog_log(LogType::Force, "IDBE_Request: Key index out of range");
			return std::nullopt;
		}
		if (file.size() < kIdbeHeaderSize + sizeof(IDBEIconDataV0))
		{
			cemuLog_log(LogType::Force, "IDBE_Request: File size does not match");
			return std::nullopt;
		}

		const uint8* key = kIdbeKeys[header.keyIndex];
		// The icon data is chained to the encrypted hash: its IV is the second
		// ciphertext block of the hash field. Grab it before decrypting the hash.
		uint8 iconIv[16];
		memcpy(iconIv, header.hashSHA256 + 16, 16);
		uint8 expectedHash[32];
		AES128_CBC_decrypt(expectedHash, header.hashSHA256, 32, key, kIdbeHashIv);

		auto icon = std::make_unique<IDBEIconDataV0>();
		AES128_CBC_decrypt((uint8*)icon.get(), file.data() + kIdbeHeaderSize, sizeof(IDBEIconDataV0), key, iconIv);

		uint8 actualHash[SHA256_DIGEST_LENGTH];
		SHA256((const unsigned char*)icon.get(), sizeof(IDBEIconDataV0), actualHash);
		if (memcmp(actualHash, expectedHash, sizeof(expectedHash)) != 0)
		{
			cemuLog_log(LogType::Force, "IDBE_Request: Hash mismatch");
			return std::nullopt;
		}
		return std::optional<IDBEIconDataV0>(*icon);
	}
};
