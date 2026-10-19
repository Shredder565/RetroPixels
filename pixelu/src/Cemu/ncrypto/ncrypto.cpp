// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "util/helpers/helpers.h"
#include "config/CemuConfig.h"

#include "openssl/bn.h"
#include "openssl/ec.h"
#include "openssl/x509.h"
#include "openssl/ssl.h"
#include "openssl/sha.h"
#include "openssl/ecdsa.h"

#include "util/crypto/aes128.h"

// provided by the IOSU crypto module (OTP / SEEPROM dumps)
void iosuCrypto_getDeviceCertificate(void* certOut, sint32 len);
void iosuCrypto_getDeviceCertPrivateKey(void* privKeyOut, sint32 len);
bool iosuCrypto_getDeviceId(uint32* deviceId);
void iosuCrypto_getDeviceSerialString(char* serialString);
void iosuCrypto_readOtpData(void* output, sint32 wordIndex, sint32 size);
void iosuCrypto_readSeepromData(void* output, sint32 wordIndex, sint32 size);
bool iosuCrypto_hasSeeprom();
extern bool hasOtpMem;

namespace NCrypto
{
	/* base64 (RFC 4648 alphabet, '=' padding) */

	namespace
	{
		constexpr char kB64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		// 6-bit value of a base64 character; 64 marks a character outside the alphabet
		uint32 B64Value(char ch)
		{
			if (ch >= 'A' && ch <= 'Z') return (uint32)(ch - 'A');
			if (ch >= 'a' && ch <= 'z') return (uint32)(ch - 'a' + 26);
			if (ch >= '0' && ch <= '9') return (uint32)(ch - '0' + 52);
			if (ch == '+') return 62;
			if (ch == '/') return 63;
			return 64;
		}
	}

	std::string base64Encode(const void* inputMem, size_t inputLen)
	{
		const uint8* in = static_cast<const uint8*>(inputMem);
		std::string out;
		out.reserve((inputLen + 2) / 3 * 4);
		size_t pos = 0;
		for (; pos + 3 <= inputLen; pos += 3)
		{
			const uint32 group = ((uint32)in[pos] << 16) | ((uint32)in[pos + 1] << 8) | (uint32)in[pos + 2];
			for (int shift = 18; shift >= 0; shift -= 6)
				out.push_back(kB64Alphabet[(group >> shift) & 63]);
		}
		const size_t rest = inputLen - pos;
		if (rest != 0)
		{
			uint32 group = (uint32)in[pos] << 16;
			if (rest == 2)
				group |= (uint32)in[pos + 1] << 8;
			out.push_back(kB64Alphabet[(group >> 18) & 63]);
			out.push_back(kB64Alphabet[(group >> 12) & 63]);
			out.push_back(rest == 2 ? kB64Alphabet[(group >> 6) & 63] : '=');
			out.push_back('=');
		}
		return out;
	}

	// Input must be a non-empty multiple of 4 characters (and longer than 3),
	// otherwise the result is empty. '=' counts as zero bits wherever it occurs;
	// only '=' in the last two places shortens the output.
	std::vector<uint8> base64Decode(std::string_view inputStr)
	{
		const size_t len = inputStr.size();
		if (len <= 3 || (len % 4) != 0)
			return {};
		size_t outLen = len / 4 * 3;
		if (inputStr[len - 1] == '=')
			outLen--;
		if (inputStr[len - 2] == '=')
			outLen--;
		std::vector<uint8> out(outLen);
		size_t w = 0;
		for (size_t g = 0; g < len; g += 4)
		{
			uint32 acc = 0;
			for (size_t k = 0; k < 4; k++)
			{
				const char ch = inputStr[g + k];
				acc = acc * 64 + (ch == '=' ? 0 : B64Value(ch));
			}
			for (int shift = 16; shift >= 0 && w < outLen; shift -= 8)
				out[w++] = (uint8)(acc >> shift);
		}
		return out;
	}

	void base64Tests()
	{
		for (size_t n = 0; n < 100; n++)
		{
			std::vector<uint8> bytes(n);
			for (size_t i = 0; i < n; i++)
				bytes[i] = (uint8)(n * 37 + i * 101);
			cemu_assert(base64Decode(base64Encode(bytes.data(), bytes.size())) == bytes);
		}
	}

	/* hashes */

	void GenerateHashSHA1(const void* data, size_t len, CHash160& hashOut)
	{
		SHA1(static_cast<const unsigned char*>(data), len, hashOut.b);
	}

	void GenerateHashSHA256(const void* data, size_t len, CHash256& hashOut)
	{
		SHA256(static_cast<const unsigned char*>(data), len, hashOut.b);
	}

	/* ticket (.tik) layout */

	namespace
	{
		struct TikHeader
		{
			/* +0x000 */ uint32be signatureType;
			/* +0x004 */ uint8 signature[0x100];
			/* +0x104 */ uint8 _pad104[0x7C];
			/* +0x180 */ ECCPubKey eccPublicKey;      // for personalized tickets
			/* +0x1BC */ uint8 formatVersion;
			/* +0x1BD */ uint8 _1BD[2];
			/* +0x1BF */ uint8 titleKey[16];          // encrypted with the common key
			/* +0x1CF */ uint8 _1CF;
			/* +0x1D0 */ uint32be ticketIdHigh;
			/* +0x1D4 */ uint32be ticketIdLow;
			/* +0x1D8 */ uint32be consoleId;           // 0 = not personalized
			/* +0x1DC */ uint32be titleIdHigh;
			/* +0x1E0 */ uint32be titleIdLow;
			/* +0x1E4 */ uint16be _1E4;
			/* +0x1E6 */ uint16be titleVersion;
			/* +0x1E8 */ uint8 _1E8[0x34];
			/* +0x21C */ uint32be accountId;
		};
		static_assert(sizeof(TikHeader) == 0x220);

		// format 1 extension, at 0x2A4
		constexpr size_t kTikExtOffset = 0x2A4;
		struct TikExtHeader
		{
			uint16be version;
			uint16be size;
			uint32be _04;
			uint32be sectionTableOffset; // relative to the extension
			uint16be sectionCount;
			uint16be sectionEntrySize;
		};
		static_assert(sizeof(TikExtHeader) == 0x10);

		struct TikExtSection
		{
			uint32be offset; // relative to the extension
			uint32be recordCount;
			uint32be recordSize;
			uint32be totalSize;
			uint16be type;
			uint16be _12;
		};
		static_assert(sizeof(TikExtSection) == 0x14);
		constexpr uint16 kSectionContentRights = 3;

		// one bit per content index, starting at firstIndex (bit n of byte n/8, LSB first)
		struct TikContentRights
		{
			uint32be firstIndex;
			uint8 bits[0x80];
		};
		static_assert(sizeof(TikContentRights) == 0x84);
		constexpr uint32 kRightsPerRecord = 0x80 * 8;

		// 128-bit Wii U common key used for ticket title keys
		constexpr uint8 kWiiUCommonKey[16] = { 0xD7, 0xB0, 0x04, 0x02, 0x65, 0x9B, 0xA2, 0xAB, 0xD2, 0xCB, 0x0D, 0xB2, 0x7F, 0xA2, 0xB6, 0x56 };

		void MakeIdIv(uint64 id, uint8 iv[16])
		{
			memset(iv, 0, 16);
			for (int i = 0; i < 8; i++)
				iv[i] = (uint8)(id >> (56 - 8 * i));
		}
	}

	bool ETicketParser::parse(const uint8* data, size_t size)
	{
		// pointer to [offset, offset + len) or null if outside the buffer
		auto at = [&](uint64 offset, uint64 len) -> const uint8* {
			if (offset > size || len > size - offset)
				return nullptr;
			return data + offset;
		};

		const TikHeader* tik = reinterpret_cast<const TikHeader*>(at(0, sizeof(TikHeader)));
		if (!tik)
			return false;
		m_titleId = ((uint64)(uint32)tik->titleIdHigh << 32) | (uint32)tik->titleIdLow;
		m_ticketId = ((uint64)(uint32)tik->ticketIdHigh << 32) | (uint32)tik->ticketIdLow;
		m_ticketFormatVersion = tik->formatVersion;
		m_titleVersion = tik->titleVersion;
		const uint32 titleIdHigh = (uint32)(m_titleId >> 32);
		if ((titleIdHigh >> 16) != 0x0005) // Wii U titles only
			return false;
		m_deviceId = tik->consoleId;
		m_isPersonalized = m_deviceId != 0;
		m_publicKey = tik->eccPublicKey;
		memcpy(m_encryptedTitleKey, tik->titleKey, sizeof(m_encryptedTitleKey));

		// only DLC tickets (0005000C) carry content rights
		if (m_ticketFormatVersion < 1 || titleIdHigh != 0x0005000C)
			return true;
		const TikExtHeader* ext = reinterpret_cast<const TikExtHeader*>(at(kTikExtOffset, sizeof(TikExtHeader)));
		if (!ext)
			return false;
		const uint32 sectionCount = ext->sectionCount;
		for (uint32 s = 0; s < sectionCount; s++)
		{
			const uint64 sectionPos = kTikExtOffset + (uint64)(uint32)ext->sectionTableOffset + (uint64)s * (uint16)ext->sectionEntrySize;
			const TikExtSection* section = reinterpret_cast<const TikExtSection*>(at(sectionPos, sizeof(TikExtSection)));
			if (!section)
				return false;
			if (section->type != kSectionContentRights)
				continue;
			if (section->recordSize != sizeof(TikContentRights))
			{
				cemuLog_log(LogType::Force, "ETicket: Failed to parse ticket with invalid rights size");
				return false;
			}
			const uint32 recordCount = section->recordCount;
			for (uint32 r = 0; r < recordCount; r++)
			{
				const uint64 recordPos = kTikExtOffset + (uint64)(uint32)section->offset + (uint64)r * sizeof(TikContentRights);
				const TikContentRights* rights = reinterpret_cast<const TikContentRights*>(at(recordPos, sizeof(TikContentRights)));
				if (!rights)
					return false;
				const uint32 first = rights->firstIndex;
				if (first > 0x1000)
				{
					cemuLog_log(LogType::Force, "ETicket: Invalid content rights index ({})", first);
					continue;
				}
				if (m_contentRights.size() < first + kRightsPerRecord)
					m_contentRights.resize(first + kRightsPerRecord);
				for (uint32 bit = 0; bit < kRightsPerRecord; bit++)
					m_contentRights[first + bit] = ((rights->bits[bit / 8] >> (bit % 8)) & 1) != 0;
			}
		}
		return true;
	}

	void ETicketParser::GetTitleKey(AesKey& key)
	{
		uint8 iv[16];
		MakeIdIv(m_titleId, iv);
		AES128_CBC_decrypt(key.b, m_encryptedTitleKey, 16, kWiiUCommonKey, iv);
	}

	// A personalized title key is additionally encrypted with a key derived via
	// ECDH between the console key and the ticket's public key (SHA-1 of the
	// shared secret, first 16 bytes; IV = ticket id).
	bool ETicketParser::Depersonalize(uint8* ticketData, size_t ticketSize, uint32 deviceId, const ECCPrivKey& devicePrivKey)
	{
		ETicketParser tik;
		if (!tik.parse(ticketData, ticketSize) || !tik.IsPersonalized())
			return false;
		if (tik.m_deviceId != deviceId)
		{
			cemuLog_log(LogType::Force, "Personalized ticket does not match deviceId");
			return false;
		}
		EC_KEY* ownKey = devicePrivKey.getPrivateKey();
		EC_POINT* peer = tik.m_publicKey.getPublicKeyAsPoint();
		uint8 secret[128]{};
		const int secretLen = ECDH_compute_key(secret, sizeof(secret), peer, ownKey, nullptr);
		EC_POINT_free(peer);
		EC_KEY_free(ownKey);
		cemu_assert(secretLen > 16);

		CHash160 digest;
		GenerateHashSHA1(secret, (size_t)secretLen, digest);
		uint8 iv[16];
		MakeIdIv(tik.m_ticketId, iv);
		uint8 commonKeyEncrypted[16];
		AES128_CBC_decrypt(commonKeyEncrypted, tik.m_encryptedTitleKey, 16, digest.b, iv);

		TikHeader* header = reinterpret_cast<TikHeader*>(ticketData);
		memcpy(header->titleKey, commonKeyEncrypted, 16);
		header->consoleId = 0;
		header->accountId = 0;
		return true;
	}

	/* title metadata (.tmd) layout */

	namespace
	{
		struct TmdHeader
		{
			/* +0x000 */ uint32be signatureType;
			/* +0x004 */ uint8 signature[0x100];
			/* +0x104 */ uint8 _104[0x7C];
			/* +0x180 */ uint8 version;
			/* +0x181 */ uint8 _181[3];
			/* +0x184 */ uint32be iosTitleIdHigh;
			/* +0x188 */ uint32be iosTitleIdLow;
			/* +0x18C */ uint32be titleIdHigh;
			/* +0x190 */ uint32be titleIdLow;
			/* +0x194 */ uint8 _194[0x48];
			/* +0x1DC */ uint16be titleVersion;
			/* +0x1DE */ uint16be contentCount;
			/* +0x1E0 */ uint8 _1E0[0x24];
			/* +0x204 */ uint8 contentInfoTable[64 * 36];
		};
		static_assert(sizeof(TmdHeader) == 0xB04);

		struct TmdContent
		{
			uint32be id;
			uint16be index;
			uint16be type;
			uint32be sizeHigh;
			uint32be sizeLow;
			uint8 hash[32];
		};
		static_assert(sizeof(TmdContent) == 0x30);
	}

	bool TMDParser::parse(const uint8* data, size_t size)
	{
		if (size < sizeof(TmdHeader))
		{
			cemuLog_log(LogType::Force, "TMD size {} below minimum size of {}", size, sizeof(TmdHeader));
			return false;
		}
		const TmdHeader* tmd = reinterpret_cast<const TmdHeader*>(data);
		m_titleId = ((uint64)(uint32)tmd->titleIdHigh << 32) | (uint32)tmd->titleIdLow;
		m_titleVersion = tmd->titleVersion;
		const uint16 count = tmd->contentCount;
		const size_t needed = sizeof(TmdHeader) + sizeof(TmdContent) * count;
		if (size < needed)
		{
			cemuLog_log(LogType::Force, "TMD size {} below expected size of {}. Content count: {}", size, needed, count);
			return false;
		}
		const TmdContent* list = reinterpret_cast<const TmdContent*>(data + sizeof(TmdHeader));
		for (uint16 i = 0; i < count; i++)
		{
			ContentEntry e{};
			e.contentId = list[i].id;
			e.index = list[i].index;
			e.contentFlags = (TMDContentFlags)(uint16)list[i].type;
			e.size = ((uint64)(uint32)list[i].sizeHigh << 32) | (uint32)list[i].sizeLow;
			memcpy(e.hash32, list[i].hash, sizeof(e.hash32));
			m_content.push_back(e);
		}
		return true;
	}

	/* ECC (sect233r1) */

	namespace
	{
		constexpr int kCurve = NID_sect233r1;

		BIGNUM* ToBignum(const uint8* bytes, size_t len)
		{
			return BN_bin2bn(bytes, (int)len, nullptr);
		}

		void FromBignum(const BIGNUM* bn, uint8* out, size_t len)
		{
			memset(out, 0, len);
			BN_bn2binpad(bn, out, (int)len);
		}
	}

	void ECCPrivKey::setPrivateKey(EC_KEY* key)
	{
		FromBignum(EC_KEY_get0_private_key(key), keyData, sizeof(keyData));
	}

	EC_KEY* ECCPrivKey::getPrivateKey() const
	{
		EC_KEY* key = EC_KEY_new_by_curve_name(kCurve);
		BIGNUM* d = ToBignum(keyData, sizeof(keyData));
		EC_KEY_set_private_key(key, d);
		BN_free(d);
		return key;
	}

	ECCPrivKey ECCPrivKey::getDeviceCertPrivateKey()
	{
		ECCPrivKey key{};
		iosuCrypto_getDeviceCertPrivateKey(key.keyData, sizeof(key.keyData));
		return key;
	}

	EC_KEY* ECCPubKey::getPublicKey()
	{
		EC_KEY* key = EC_KEY_new_by_curve_name(kCurve);
		BIGNUM* bx = ToBignum(x, sizeof(x));
		BIGNUM* by = ToBignum(y, sizeof(y));
		EC_KEY_set_public_key_affine_coordinates(key, bx, by);
		BN_free(by);
		BN_free(bx);
		return key;
	}

	EC_POINT* ECCPubKey::getPublicKeyAsPoint()
	{
		EC_GROUP* group = EC_GROUP_new_by_curve_name(kCurve);
		EC_POINT* point = EC_POINT_new(group);
		BIGNUM* bx = ToBignum(x, sizeof(x));
		BIGNUM* by = ToBignum(y, sizeof(y));
		BN_CTX* ctx = BN_CTX_new();
		EC_POINT_set_affine_coordinates(group, point, bx, by, ctx);
		BN_CTX_free(ctx);
		BN_free(by);
		BN_free(bx);
		EC_GROUP_free(group);
		return point;
	}

	ECCPubKey ECCPubKey::generateFromPrivateKey(ECCPrivKey& privKey)
	{
		ECCPubKey result{};
		EC_GROUP* group = EC_GROUP_new_by_curve_name(kCurve);
		EC_POINT* q = EC_POINT_new(group);
		BIGNUM* d = ToBignum(privKey.keyData, sizeof(privKey.keyData));
		BIGNUM* qx = BN_new();
		BIGNUM* qy = BN_new();
		EC_POINT_mul(group, q, d, nullptr, nullptr, nullptr);
		EC_POINT_get_affine_coordinates(group, q, qx, qy, nullptr);
		FromBignum(qx, result.x, sizeof(result.x));
		FromBignum(qy, result.y, sizeof(result.y));
		BN_free(qy);
		BN_free(qx);
		BN_free(d);
		EC_POINT_free(q);
		EC_GROUP_free(group);
		return result;
	}

	ECDSA_SIG* ECCSig::getSignature()
	{
		ECDSA_SIG* sig = ECDSA_SIG_new();
		ECDSA_SIG_set0(sig, ToBignum(r, sizeof(r)), ToBignum(s, sizeof(s))); // sig owns both
		return sig;
	}

	void ECCSig::setSignature(ECDSA_SIG* sig)
	{
		const BIGNUM* sr = nullptr;
		const BIGNUM* ss = nullptr;
		ECDSA_SIG_get0(sig, &sr, &ss);
		cemu_assert_debug(BN_num_bytes(sr) <= 30 && BN_num_bytes(ss) <= 30);
		FromBignum(sr, r, sizeof(r));
		FromBignum(ss, s, sizeof(s));
	}

	/* certificates */

	namespace
	{
		constexpr size_t kCertSignedOffset = 0x80; // 'issuer'
		constexpr size_t kCertSignedSize = 0x100;

		const uint8* SignedPart(const CertECC& cert)
		{
			return reinterpret_cast<const uint8*>(&cert) + kCertSignedOffset;
		}

		bool VerifyDigest(const uint8* digest, sint32 digestLen, ECCPubKey& pub, ECCSig& sig)
		{
			EC_KEY* key = pub.getPublicKey();
			ECDSA_SIG* s = sig.getSignature();
			const bool ok = ECDSA_do_verify(digest, digestLen, s, key) == 1;
			ECDSA_SIG_free(s);
			EC_KEY_free(key);
			return ok;
		}

		ECCSig SignDigest(const uint8* digest, sint32 digestLen, const ECCPrivKey& priv)
		{
			EC_KEY* key = priv.getPrivateKey();
			ECDSA_SIG* raw = ECDSA_do_sign(digest, digestLen, key);
			EC_KEY_free(key);
			ECCSig sig{};
			sig.setSignature(raw);
			ECDSA_SIG_free(raw);
			return sig;
		}
	}

	bool CertECC::decodeFromBase64(std::string_view input)
	{
		const std::vector<uint8> raw = base64Decode(input);
		if (raw.size() != sizeof(CertECC))
			return false;
		memcpy(this, raw.data(), sizeof(CertECC));
		return true;
	}

	std::string CertECC::encodeToBase64()
	{
		return base64Encode(this, sizeof(CertECC));
	}

	bool CertECC::verifySignatureViaPubKey(ECCPubKey& signerPubKey)
	{
		CHash256 digest;
		GenerateHashSHA256(SignedPart(*this), kCertSignedSize, digest);
		return VerifyDigest(digest.b, sizeof(digest.b), signerPubKey, signature);
	}

	void CertECC::sign(ECCPrivKey& signerPrivKey)
	{
		CHash256 digest;
		GenerateHashSHA256(SignedPart(*this), kCertSignedSize, digest);
		signature = SignDigest(digest.b, sizeof(digest.b), signerPrivKey);
	}

	CertECC CertECC::GetDeviceCertificate()
	{
		CertECC cert{};
		iosuCrypto_getDeviceCertificate(&cert, sizeof(CertECC));
		return cert;
	}

	CertECC CertECC::generateCertificate(uint32 signerTitleIdHigh, uint32 signerTitleIdLow, ECCPrivKey& privKeySigner, ECCPrivKey& privKeyIn, ECCPubKey& pubKeyOut)
	{
		CertECC cert = GetDeviceCertificate();
		cemu_assert(cert.signatureType == SIGTYPE::ECC_SHA256);
		cert.date = 0;
		// the new issuer is the device cert's issuer and subject joined by '-'
		// (appended in place after the existing text; later bytes stay as they are)
		const size_t issuerLen = strnlen(cert.issuer, sizeof(cert.issuer));
		const size_t subjectLen = strnlen(cert.ngName, sizeof(cert.ngName));
		cemu_assert(issuerLen + 1 + subjectLen < sizeof(cert.issuer));
		cert.issuer[issuerLen] = '-';
		memcpy(cert.issuer + issuerLen + 1, cert.ngName, subjectLen);
		cert.issuer[issuerLen + 1 + subjectLen] = '\0';
		// subject: "AP" + app title id
		const std::string subject = fmt::format("AP{:08x}{:08x}", signerTitleIdHigh, signerTitleIdLow);
		memset(cert.ngName, 0, sizeof(cert.ngName));
		memcpy(cert.ngName, subject.data(), subject.size());
		cert.publicKey = ECCPubKey::generateFromPrivateKey(privKeyIn);
		pubKeyOut = cert.publicKey;
		cert.sign(privKeySigner);
		return cert;
	}

	ECCSig signHash(uint32 signerTitleIdHigh, uint32 signerTitleIdLow, uint8* hash, sint32 hashLen, CertECC& certChainOut)
	{
		// fresh signing key
		ECCPrivKey signingKey{};
		{
			EC_KEY* generated = EC_KEY_new_by_curve_name(kCurve);
			EC_KEY_generate_key(generated);
			signingKey.setPrivateKey(generated);
			EC_KEY_free(generated);
		}
		ECCPubKey signingPub{};
		ECCPrivKey deviceKey = ECCPrivKey::getDeviceCertPrivateKey();
		certChainOut = CertECC::generateCertificate(signerTitleIdHigh, signerTitleIdLow, deviceKey, signingKey, signingPub);

		cemu_assert_debug(hashLen == 32);
		ECCSig sig = SignDigest(hash, hashLen, signingKey);
		cemu_assert(VerifyDigest(hash, hashLen, signingPub, sig));
		return sig;
	}

	bool verifyHashSignature(uint8* hash, sint32 hashLen, ECCPubKey& certChainPubKey, ECCSig& sig)
	{
		return VerifyDigest(hash, hashLen, certChainPubKey, sig);
	}

	bool verifyCert(CertECC& cert, ECCPubKey& signerPubKey)
	{
		switch ((CertECC::SIGTYPE)cert.signatureType)
		{
		case CertECC::SIGTYPE::ECC_SHA256:
		{
			CHash256 digest;
			GenerateHashSHA256(SignedPart(cert), kCertSignedSize, digest);
			return VerifyDigest(digest.b, 32, signerPubKey, cert.signature);
		}
		case CertECC::SIGTYPE::ECC_SHA1:
		{
			CHash160 digest;
			GenerateHashSHA1(SignedPart(cert), kCertSignedSize, digest);
			return VerifyDigest(digest.b, 20, signerPubKey, cert.signature);
		}
		default:
			cemu_assert_unimplemented();
			return false;
		}
	}

	/* console identity */

	uint32 GetDeviceId()
	{
		uint32 id = 0;
		return iosuCrypto_getDeviceId(&id) ? id : 0x11223344; // placeholder without an OTP dump
	}

	std::string GetSerial()
	{
		char serial[128]{};
		iosuCrypto_getDeviceSerialString(serial);
		return serial;
	}

	bool SEEPROM_IsPresent()
	{
		return iosuCrypto_hasSeeprom();
	}

	CafeConsoleRegion SEEPROM_GetRegion()
	{
		// the region is the low byte of the big endian word at SEEPROM word 0xA4
		uint8 word[4]{};
		iosuCrypto_readSeepromData(word, 0xA4, 4);
		if (word[3] == 0)
			cemuLog_log(LogType::Force, "SEEPROM region is invalid (0)");
		return (CafeConsoleRegion)word[3];
	}

	bool OTP_IsPresent()
	{
		return hasOtpMem;
	}

	bool HasDataForConsoleCert()
	{
		return SEEPROM_IsPresent() && OTP_IsPresent();
	}

	std::string GetRegionAsString(CafeConsoleRegion regionCode)
	{
		switch (regionCode)
		{
		case CafeConsoleRegion::EUR: return "EUR";
		case CafeConsoleRegion::USA: return "USA";
		case CafeConsoleRegion::JPN: return "JPN";
		case CafeConsoleRegion::CHN: return "CHN";
		case CafeConsoleRegion::KOR: return "KOR";
		case CafeConsoleRegion::TWN: return "TWN";
		default: break;
		}
		cemuLog_log(LogType::Force, "Unknown region code 0x{:x}", (uint32)regionCode);
		return "UKN";
	}

	namespace
	{
		// Wii U system country codes (index -> ISO 3166 alpha-2)
		struct CountryCode
		{
			sint32 index;
			const char* code;
		};
		constexpr CountryCode kCountries[] = {
			{1, "JP"},
			{8, "AI"}, {9, "AG"}, {10, "AR"}, {11, "AW"}, {12, "BS"}, {13, "BB"}, {14, "BZ"}, {15, "BO"}, {16, "BR"},
			{17, "VG"}, {18, "CA"}, {19, "KY"}, {20, "CL"}, {21, "CO"}, {22, "CR"}, {23, "DM"}, {24, "DO"}, {25, "EC"},
			{26, "SV"}, {27, "GF"}, {28, "GD"}, {29, "GP"}, {30, "GT"}, {31, "GY"}, {32, "HT"}, {33, "HN"}, {34, "JM"},
			{35, "MQ"}, {36, "MX"}, {37, "MS"}, {38, "AN"}, {39, "NI"}, {40, "PA"}, {41, "PY"}, {42, "PE"}, {43, "KN"},
			{44, "LC"}, {45, "VC"}, {46, "SR"}, {47, "TT"}, {48, "TC"}, {49, "US"}, {50, "UY"}, {51, "VI"}, {52, "VE"},
			{64, "AL"}, {65, "AU"}, {66, "AT"}, {67, "BE"}, {68, "BA"}, {69, "BW"}, {70, "BG"}, {71, "HR"}, {72, "CY"},
			{73, "CZ"}, {74, "DK"}, {75, "EE"}, {76, "FI"}, {77, "FR"}, {78, "DE"}, {79, "GR"}, {80, "HU"}, {81, "IS"},
			{82, "IE"}, {83, "IT"}, {84, "LV"}, {85, "LS"}, {86, "LI"}, {87, "LT"}, {88, "LU"}, {89, "MK"}, {90, "MT"},
			{91, "ME"}, {92, "MZ"}, {93, "NA"}, {94, "NL"}, {95, "NZ"}, {96, "NO"}, {97, "PL"}, {98, "PT"}, {99, "RO"},
			{100, "RU"}, {101, "RS"}, {102, "SK"}, {103, "SI"}, {104, "ZA"}, {105, "ES"}, {106, "SZ"}, {107, "SE"},
			{108, "CH"}, {109, "TR"}, {110, "GB"}, {111, "ZM"}, {112, "ZW"}, {113, "AZ"}, {114, "MR"}, {115, "ML"},
			{116, "NE"}, {117, "TD"}, {118, "SD"}, {119, "ER"}, {120, "DJ"}, {121, "SO"}, {122, "AD"}, {123, "GI"},
			{124, "GG"}, {125, "IM"}, {126, "JE"}, {127, "MC"},
			{128, "TW"},
			{136, "KR"},
			{144, "HK"}, {145, "MO"},
			{152, "ID"}, {153, "SG"}, {154, "TH"}, {155, "PH"}, {156, "MY"},
			{160, "CN"},
			{168, "AE"}, {170, "EG"}, {171, "OM"}, {172, "QA"}, {173, "KW"}, {174, "SA"}, {175, "SY"}, {176, "BH"}, {177, "JO"},
			{184, "SM"}, {185, "VA"}, {186, "BM"}, {187, "IN"},
			{192, "NG"}, {193, "AO"}, {194, "GH"},
		};
	}

	const char* GetCountryAsString(sint32 index)
	{
		for (const CountryCode& c : kCountries)
		{
			if (c.index == index)
				return c.code;
		}
		return "NN";
	}

	size_t GetCountryCount()
	{
		return std::size(kCountries);
	}

	void unitTests()
	{
		base64Tests();
	}
}
