// Original code for RetroPixels (PixelU). Not derived from Cemu.
// amiibo (NTAG215) data encryption, written from the publicly documented
// scheme (as used by tools like amiitool):
//
// Two key sets are derived per tag, "unfixed infos" for the user data and
// "locked secret" for the tag identity. Each derivation feeds the tag's write
// counter, UID and keygen salt (mixed with the master key's type string,
// magic bytes and xor pad) into an HMAC-SHA256 DRBG keyed with the master
// key's HMAC key; the first 48 output bytes are an AES-128 key, an AES-CTR IV
// and an HMAC key.
//
// Plain ("internal") layout, 0x208 bytes:
//   0x000 lock bytes + capability container   0x008 data HMAC
//   0x028 0xA5, write counter (BE16), unk      0x02C..0x1B4 encrypted part
//   0x1B4 tag HMAC   0x1D4 UID (8)   0x1DC model info (12)   0x1E8 salt (32)
// Tag HMAC = HMAC(locked key, 0x1D4..0x208), data HMAC = HMAC(unfixed key,
// 0x029..0x208) computed after the tag HMAC is in place.
#pragma once

#include <array>
#include "openssl/evp.h"
#include "openssl/hmac.h"

namespace nn::nfp::amiibo
{
	constexpr size_t kRawSize = 0x214;   // pages 0..0x84 of an NTAG215 dump
	constexpr size_t kPlainSize = 0x208;

	using RawTag = std::array<uint8, kRawSize>;
	using PlainTag = std::array<uint8, kPlainSize>;

	namespace detail
	{
		struct MasterKey
		{
			const char* typeString; // hashed including its terminating zero
			uint8 magic[16];
			size_t magicLen;
			uint8 xorPad[32];
			uint8 hmacKey[16];
		};

		inline const MasterKey& UnfixedInfosKey()
		{
			static const MasterKey k{
				"unfixed infos",
				{0xDB, 0x4B, 0x9E, 0x3F, 0x45, 0x27, 0x8F, 0x39, 0x7E, 0xFF, 0x9B, 0x4F, 0xB9, 0x93},
				14,
				{0x04, 0x49, 0x17, 0xDC, 0x76, 0xB4, 0x96, 0x40, 0xD6, 0xF8, 0x39, 0x39, 0x96, 0x0F, 0xAE, 0xD4,
				 0xEF, 0x39, 0x2F, 0xAA, 0xB2, 0x14, 0x28, 0xAA, 0x21, 0xFB, 0x54, 0xE5, 0x45, 0x05, 0x47, 0x66},
				{0x1D, 0x16, 0x4B, 0x37, 0x5B, 0x72, 0xA5, 0x57, 0x28, 0xB9, 0x1D, 0x64, 0xB6, 0xA3, 0xC2, 0x05}};
			return k;
		}

		inline const MasterKey& LockedSecretKey()
		{
			static const MasterKey k{
				"locked secret",
				{0xFD, 0xC8, 0xA0, 0x76, 0x94, 0xB8, 0x9E, 0x4C, 0x47, 0xD3, 0x7D, 0xE8, 0xCE, 0x5C, 0x74, 0xC1},
				16,
				{0x04, 0x49, 0x17, 0xDC, 0x76, 0xB4, 0x96, 0x40, 0xD6, 0xF8, 0x39, 0x39, 0x96, 0x0F, 0xAE, 0xD4,
				 0xEF, 0x39, 0x2F, 0xAA, 0xB2, 0x14, 0x28, 0xAA, 0x21, 0xFB, 0x54, 0xE5, 0x45, 0x05, 0x47, 0x66},
				{0x7F, 0x75, 0x2D, 0x28, 0x73, 0xA2, 0x00, 0x17, 0xFE, 0xF8, 0x5C, 0x05, 0x75, 0x90, 0x4B, 0x6D}};
			return k;
		}

		struct TagKeys
		{
			uint8 aesKey[16];
			uint8 aesIv[16];
			uint8 hmacKey[16];
		};

		inline void HmacSha256(const uint8* key, size_t keyLen, const uint8* data, size_t len, uint8 out[32])
		{
			unsigned int outLen = 32;
			HMAC(EVP_sha256(), key, (int)keyLen, data, len, out, &outLen);
		}

		inline TagKeys DeriveKeys(const MasterKey& mk, const PlainTag& plain)
		{
			// DRBG input: 2-byte counter, then type string, the first
			// (16 - magicLen) bytes of the write-counter block, the magic,
			// UID twice, salt xor pad
			std::vector<uint8> msg = {0, 0};
			msg.insert(msg.end(), mk.typeString, mk.typeString + strlen(mk.typeString) + 1);
			uint8 counterBlock[16]{};
			counterBlock[0] = plain[0x029];
			counterBlock[1] = plain[0x02A];
			msg.insert(msg.end(), counterBlock, counterBlock + (16 - mk.magicLen));
			msg.insert(msg.end(), mk.magic, mk.magic + mk.magicLen);
			msg.insert(msg.end(), plain.begin() + 0x1D4, plain.begin() + 0x1DC);
			msg.insert(msg.end(), plain.begin() + 0x1D4, plain.begin() + 0x1DC);
			for (size_t i = 0; i < 32; i++)
				msg.push_back(plain[0x1E8 + i] ^ mk.xorPad[i]);

			uint8 stream[64];
			for (uint8 step = 0; step < 2; step++)
			{
				msg[1] = step; // big-endian counter 0, 1
				HmacSha256(mk.hmacKey, sizeof(mk.hmacKey), msg.data(), msg.size(), stream + step * 32);
			}
			TagKeys keys;
			memcpy(&keys, stream, sizeof(keys));
			return keys;
		}

		// AES-128-CTR over the encrypted part (same operation both ways)
		inline void CryptUserData(const TagKeys& keys, PlainTag& tag)
		{
			EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
			int outLen = 0;
			uint8 tmp[0x188];
			EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, keys.aesKey, keys.aesIv);
			EVP_EncryptUpdate(ctx, tmp, &outLen, tag.data() + 0x02C, 0x188);
			EVP_CIPHER_CTX_free(ctx);
			memcpy(tag.data() + 0x02C, tmp, 0x188);
		}

		// (raw offset, plain offset, length)
		struct Span { size_t raw; size_t plain; size_t len; };
		constexpr Span kLayout[] = {
			{0x008, 0x000, 0x008}, {0x080, 0x008, 0x020}, {0x010, 0x028, 0x024}, {0x0A0, 0x04C, 0x168},
			{0x034, 0x1B4, 0x020}, {0x000, 0x1D4, 0x008}, {0x054, 0x1DC, 0x02C},
		};

		inline void Sign(const TagKeys& unfixed, const TagKeys& locked, PlainTag& plain)
		{
			HmacSha256(locked.hmacKey, 16, plain.data() + 0x1D4, 0x34, plain.data() + 0x1B4);
			HmacSha256(unfixed.hmacKey, 16, plain.data() + 0x029, 0x1DF, plain.data() + 0x008);
		}
	}

	// Decrypts a tag dump. Returns false if a signature does not match (the
	// data is still decrypted and usable).
	inline bool Decrypt(const RawTag& raw, PlainTag& plain)
	{
		using namespace detail;
		for (const Span& s : kLayout)
			memcpy(plain.data() + s.plain, raw.data() + s.raw, s.len);
		const TagKeys unfixed = DeriveKeys(UnfixedInfosKey(), plain);
		const TagKeys locked = DeriveKeys(LockedSecretKey(), plain);
		CryptUserData(unfixed, plain);

		uint8 storedTagHmac[32], storedDataHmac[32];
		memcpy(storedTagHmac, plain.data() + 0x1B4, 32);
		memcpy(storedDataHmac, plain.data() + 0x008, 32);
		Sign(unfixed, locked, plain);
		const bool tagOk = memcmp(storedTagHmac, plain.data() + 0x1B4, 32) == 0;
		const bool dataOk = memcmp(storedDataHmac, plain.data() + 0x008, 32) == 0;
		if (!tagOk)
			cemuLog_log(LogType::Force, "nn_nfp: amiibo tag signature does not match");
		if (!dataOk)
			cemuLog_log(LogType::Force, "nn_nfp: amiibo data signature does not match");
		return tagOk && dataOk;
	}

	// Signs and encrypts plain data. The bytes after the user memory (dynamic
	// lock and config pages) are taken from 'original'.
	inline void Encrypt(const PlainTag& plainIn, const RawTag& original, RawTag& raw)
	{
		using namespace detail;
		PlainTag plain = plainIn;
		const TagKeys unfixed = DeriveKeys(UnfixedInfosKey(), plain);
		const TagKeys locked = DeriveKeys(LockedSecretKey(), plain);
		Sign(unfixed, locked, plain);
		CryptUserData(unfixed, plain);
		raw = original;
		for (const Span& s : kLayout)
			memcpy(raw.data() + s.raw, plain.data() + s.plain, s.len);
	}
}
