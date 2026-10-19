// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Console crypto helpers: base64, SHA hashes, the sect233r1 ECC keys,
// signatures and certificates used by the Wii U, eShop tickets (.tik) and
// title metadata (.tmd), and queries about the console's OTP/SEEPROM data.

enum class CafeConsoleRegion;

// OpenSSL types, declared here so this header does not pull in OpenSSL
typedef struct ec_key_st EC_KEY;
typedef struct ec_point_st EC_POINT;
typedef struct ECDSA_SIG_st ECDSA_SIG;

namespace NCrypto
{
	std::string base64Encode(const void* inputMem, size_t inputLen);
	std::vector<uint8> base64Decode(std::string_view inputStr);

	struct AesKey
	{
		static constexpr size_t SIZE = 16;
		uint8 b[SIZE];
	};

	struct AesIv
	{
		static constexpr size_t SIZE = 16;
		uint8 iv[SIZE];
	};

	// sect233r1 private key, 30 bytes big endian
	struct ECCPrivKey
	{
		uint8 keyData[30];

		void setPrivateKey(EC_KEY* key);
		EC_KEY* getPrivateKey() const; // caller frees with EC_KEY_free

		static ECCPrivKey getDeviceCertPrivateKey();
	};

	// sect233r1 public key as affine coordinates, 30 bytes each, big endian
	struct ECCPubKey
	{
		uint8 x[30];
		uint8 y[30];

		EC_KEY* getPublicKey();         // caller frees with EC_KEY_free
		EC_POINT* getPublicKeyAsPoint(); // caller frees with EC_POINT_free

		[[nodiscard]] static ECCPubKey generateFromPrivateKey(ECCPrivKey& privKey);
	};

	// ECDSA signature, r and s as 30 bytes big endian each
	struct ECCSig
	{
		uint8 r[30];
		uint8 s[30];

		ECDSA_SIG* getSignature(); // caller frees with ECDSA_SIG_free
		void setSignature(ECDSA_SIG* sig);

		bool doesMatch(ECCSig& otherSig) const
		{
			return memcmp(r, otherSig.r, sizeof(r)) == 0 && memcmp(s, otherSig.s, sizeof(s)) == 0;
		}

		std::string encodeToBase64()
		{
			return base64Encode(this, sizeof(r) + sizeof(s));
		}
	};

	struct CHash256 // SHA-256
	{
		uint8 b[32];
	};

	struct CHash160 // SHA-1
	{
		uint8 b[20];
	};

	// ECC certificate as stored by the console (0x180 bytes, big endian fields).
	// The signature covers the 0x100 bytes from 'issuer' onwards.
	struct CertECC
	{
		enum class SIGTYPE : uint32
		{
			ECC_SHA1 = 0x00010002,
			ECC_SHA256 = 0x00010005
		};

		/* +0x000 */ betype<SIGTYPE> signatureType;
		/* +0x004 */ ECCSig signature;
		/* +0x040 */ uint8 ukn040[0x40];
		/* +0x080 */ char issuer[0x40];   // e.g. "Root-CA%08x-MS%08x"
		/* +0x0C0 */ char ukn0C0[0x4];
		/* +0x0C4 */ char ngName[0x40];   // subject, e.g. "NG%08x"
		/* +0x104 */ uint32 date;
		/* +0x108 */ ECCPubKey publicKey;
		/* +0x144 */ uint8 padding[0x180 - 0x144];

		bool decodeFromBase64(std::string_view input);
		std::string encodeToBase64();

		bool verifySignatureViaPubKey(ECCPubKey& signerPubKey);
		void sign(ECCPrivKey& signerPrivKey);

		static CertECC GetDeviceCertificate();
		// a certificate for the app "AP<titleId>" holding the public key of
		// privKeyIn, issued under the device certificate and signed by privKeySigner
		static CertECC generateCertificate(uint32 signerTitleIdHigh, uint32 signerTitleIdLow, ECCPrivKey& privKeySigner, ECCPrivKey& privKeyIn, ECCPubKey& pubKeyOut);
	};

	static_assert(sizeof(CertECC) == 0x180);

	// eShop ticket (.tik)
	class ETicketParser
	{
	public:
		bool parse(const uint8* data, size_t size);
		// turns a ticket personalized for this console into a common one (in place)
		static bool Depersonalize(uint8* ticketData, size_t ticketSize, uint32 deviceId, const ECCPrivKey& devicePrivKey);

		uint64 GetTicketId() const { return m_ticketId; }
		uint64 GetTitleId() const { return m_titleId; }
		uint16 GetTicketVersion() const { return m_titleVersion; }

		void GetTitleKey(AesKey& key);

		bool IsPersonalized() { return m_isPersonalized; }

		bool CheckRight(size_t index)
		{
			return index < m_contentRights.size() && m_contentRights[index];
		}

	private:
		uint64 m_titleId{};
		uint16 m_titleVersion{};
		uint64 m_ticketId{};
		uint8 m_ticketFormatVersion{};
		uint8 m_encryptedTitleKey[16]{};
		bool m_isPersonalized{};
		uint32 m_deviceId{};
		ECCPubKey m_publicKey{};
		std::vector<bool> m_contentRights;
	};

	// title metadata (.tmd)
	class TMDParser
	{
	public:
		enum class TMDContentFlags : uint16
		{
			FLAG_SHA1 = 0x2000,           // content hash is SHA-1 (else SHA-256)
			FLAG_HASHED_CONTENT = 0x0002, // .app is stored in the hashed (h3) layout
		};

		struct ContentEntry
		{
			uint32 contentId;
			uint16 index;
			TMDContentFlags contentFlags;
			uint64 size;
			uint8 hash32[32];
		};

		uint64 getTitleId() const { return m_titleId; }
		uint16 getTitleVersion() const { return m_titleVersion; }

		bool parse(const uint8* data, size_t size);
		std::vector<ContentEntry>& GetContentList() { return m_content; }

	private:
		uint64 m_titleId{};
		uint16 m_titleVersion{};
		std::vector<ContentEntry> m_content;
	};

	DEFINE_ENUM_FLAG_OPERATORS(TMDParser::TMDContentFlags);

	void GenerateHashSHA256(const void* data, size_t len, CHash256& hashOut);
	// signs a 32-byte hash with a fresh key whose certificate (issued under the
	// device certificate) is returned in certChainOut
	ECCSig signHash(uint32 signerTitleIdHigh, uint32 signerTitleIdLow, uint8* hash, sint32 hashLen, CertECC& certChainOut);

	uint32 GetDeviceId();
	std::string GetSerial();
	CafeConsoleRegion SEEPROM_GetRegion();
	std::string GetRegionAsString(CafeConsoleRegion regionCode);
	const char* GetCountryAsString(sint32 index); // "NN" for unknown indices
	size_t GetCountryCount();
	bool SEEPROM_IsPresent();
	bool OTP_IsPresent();

	bool HasDataForConsoleCert();

	void unitTests();
}
