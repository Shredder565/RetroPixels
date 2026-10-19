// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Console identity and TLS certificate store. Uses the user supplied OTP and
// SEEPROM dumps to build the device certificate, and loads the system
// certificate title (0005001b-10054000) from the mlc for online services.
#include "iosu_crypto.h"
#include "config/ActiveSettings.h"
#include "openssl/bn.h"
#include "openssl/obj_mac.h"
#include "openssl/ec.h"
#include "openssl/x509.h"
#include "openssl/ssl.h"
#include "openssl/ecdsa.h"
#include "util/crypto/aes128.h"
#include "Common/FileStream.h"

// read by ncrypto
bool hasOtpMem = false;

namespace
{
	constexpr size_t kOtpSize = 1024;
	constexpr size_t kSeepromSize = 512;

	uint8 g_otp[kOtpSize];
	uint8 g_seeprom[kSeepromSize];
	bool g_hasSeeprom = false;

	CertECC_t g_deviceCert;
	ECCPrivKey g_devicePrivKey{};

	struct StoredCertificate
	{
		sint32 id;
		X509* cert;
		std::vector<uint8> certData;
		RSA* pkey; // owned by an EVP_PKEY that is kept alive for the process lifetime
		std::vector<uint8> pkeyDERData;
	};
	std::vector<StoredCertificate> g_certStore;
	bool g_certStoreComplete = false;

	struct CertificateFile
	{
		int id;
		const wchar_t* name;
		const wchar_t* key; // empty: no private key
	};

	// files of the system certificate title, relative to its content folder
	const CertificateFile kCertificateFiles[] = {
		// client certificates
		{ 1, L"ccerts/WIIU_COMMON_1_CERT.der", L"ccerts/WIIU_COMMON_1_RSA_KEY.aes" },
		{ 3, L"ccerts/WIIU_ACCOUNT_1_CERT.der", L"ccerts/WIIU_ACCOUNT_1_RSA_KEY.aes" },
		{ 4, L"ccerts/WIIU_OLIVE_1_CERT.der", L"ccerts/WIIU_OLIVE_1_RSA_KEY.aes" },
		{ 5, L"ccerts/WIIU_VINO_1_CERT.der", L"ccerts/WIIU_VINO_1_RSA_KEY.aes" },
		{ 6, L"ccerts/WIIU_WOOD_1_CERT.der", L"ccerts/WIIU_WOOD_1_RSA_KEY.aes" },
		{ 7, L"ccerts/WIIU_OLIVE_1_CERT.der", L"ccerts/WIIU_OLIVE_1_RSA_KEY.aes" },
		{ 8, L"ccerts/WIIU_WOOD_1_CERT.der", L"ccerts/WIIU_WOOD_1_RSA_KEY.aes" },
		// Nintendo CA certificates
		{ 100, L"scerts/CACERT_NINTENDO_CA.der", L"" },
		{ 101, L"scerts/CACERT_NINTENDO_CA_G2.der", L"" },
		{ 102, L"scerts/CACERT_NINTENDO_CA_G3.der", L"" },
		{ 103, L"scerts/CACERT_NINTENDO_CLASS2_CA.der", L"" },
		{ 104, L"scerts/CACERT_NINTENDO_CLASS2_CA_G2.der", L"" },
		{ 105, L"scerts/CACERT_NINTENDO_CLASS2_CA_G3.der", L"" },
		// commercial CA certificates
		{ 1001, L"scerts/BALTIMORE_CYBERTRUST_ROOT_CA.der", L"" },
		{ 1002, L"scerts/CYBERTRUST_GLOBAL_ROOT_CA.der", L"" },
		{ 1003, L"scerts/VERIZON_GLOBAL_ROOT_CA.der", L"" },
		{ 1004, L"scerts/GLOBALSIGN_ROOT_CA.der", L"" },
		{ 1005, L"scerts/GLOBALSIGN_ROOT_CA_R2.der", L"" },
		{ 1006, L"scerts/GLOBALSIGN_ROOT_CA_R3.der", L"" },
		{ 1007, L"scerts/VERISIGN_CLASS3_PUBLIC_PRIMARY_CA_G3.der", L"" },
		{ 1008, L"scerts/VERISIGN_UNIVERSAL_ROOT_CA.der", L"" },
		{ 1009, L"scerts/VERISIGN_CLASS3_PUBLIC_PRIMARY_CA_G5.der", L"" },
		{ 1010, L"scerts/THAWTE_PRIMARY_ROOT_CA_G3.der", L"" },
		{ 1011, L"scerts/THAWTE_PRIMARY_ROOT_CA.der", L"" },
		{ 1012, L"scerts/GEOTRUST_GLOBAL_CA.der", L"" },
		{ 1013, L"scerts/GEOTRUST_GLOBAL_CA2.der", L"" },
		{ 1014, L"scerts/GEOTRUST_PRIMARY_CA.der", L"" },
		{ 1015, L"scerts/GEOTRUST_PRIMARY_CA_G3.der", L"" },
		{ 1016, L"scerts/ADDTRUST_EXT_CA_ROOT.der", L"" },
		{ 1017, L"scerts/COMODO_CA.der", L"" },
		{ 1018, L"scerts/UTN_DATACORP_SGC_CA.der", L"" },
		{ 1019, L"scerts/UTN_USERFIRST_HARDWARE_CA.der", L"" },
		{ 1020, L"scerts/DIGICERT_HIGH_ASSURANCE_EV_ROOT_CA.der", L"" },
		{ 1021, L"scerts/DIGICERT_ASSURED_ID_ROOT_CA.der", L"" },
		{ 1022, L"scerts/DIGICERT_GLOBAL_ROOT_CA.der", L"" },
		{ 1023, L"scerts/GTE_CYBERTRUST_GLOBAL_ROOT.der", L"" },
		{ 1024, L"scerts/VERISIGN_CLASS3_PUBLIC_PRIMARY_CA.der", L"" },
		{ 1025, L"scerts/THAWTE_PREMIUM_SERVER_CA.der", L"" },
		{ 1026, L"scerts/EQUIFAX_SECURE_CA.der", L"" },
		{ 1027, L"scerts/ENTRUST_SECURE_SERVER_CA.der", L"" },
		{ 1028, L"scerts/VERISIGN_CLASS3_PUBLIC_PRIMARY_CA_G2.der", L"" },
		{ 1029, L"scerts/ENTRUST_CA_2048.der", L"" },
		{ 1030, L"scerts/ENTRUST_ROOT_CA.der", L"" },
		{ 1031, L"scerts/ENTRUST_ROOT_CA_G2.der", L"" },
		{ 1032, L"scerts/DIGICERT_ASSURED_ID_ROOT_CA_G2.der", L"" },
		{ 1033, L"scerts/DIGICERT_GLOBAL_ROOT_CA_G2.der", L"" },
	};

	constexpr const wchar_t* kCertTitleContentW = L"sys/title/0005001b/10054000/content/";
	constexpr const char* kCertTitleContent = "sys/title/0005001b/10054000/content/";

	uint32 OtpWord(sint32 wordIndex)
	{
		uint32be v;
		memcpy(&v, g_otp + wordIndex * 4, sizeof(v));
		return v;
	}

	const StoredCertificate* FindCertificate(sint32 id)
	{
		for (const StoredCertificate& c : g_certStore)
			if (c.id == id)
				return &c;
		return nullptr;
	}

	// writes 'bn' right aligned into a field of 'width' bytes
	void StoreRightAligned(const BIGNUM* bn, uint8* field, sint32 width)
	{
		BN_bn2bin(bn, field + (width - BN_num_bytes(bn)));
	}

	void BuildDeviceCertificate()
	{
		static_assert(sizeof(g_deviceCert) == 0x180);
		memset(&g_deviceCert, 0, sizeof(g_deviceCert));
		if (!hasOtpMem)
			return; // the certificate is derived entirely from OTP values
		CertECC_t& c = g_deviceCert;
		c.sigType = CertECC_t::SIGTYPE::ECC_SHA256;
		c.ukn0C0[3] = 0x02;
		iosuCrypto_readOtpData(c.signature, 0xA3, 0x3C);
		sprintf(c.certificateSubject, "Root-CA%08x-MS%08x", OtpWord(0xA1), OtpWord(0xA0));
		sprintf(c.ngName, "NG%08x", OtpWord(0x87));
		iosuCrypto_readOtpData(&c.ngKeyId, 0xA2, sizeof(uint32));

		// private key from OTP, public key = privateKey * G on sect233r1
		uint8 privKey[0x1E];
		iosuCrypto_readOtpData(privKey, 0x88, sizeof(privKey));
		memcpy(g_devicePrivKey.keyData, privKey, sizeof(privKey));

		BN_CTX* ctx = BN_CTX_new();
		BN_CTX_start(ctx);
		BIGNUM* k = BN_CTX_get(ctx);
		BIGNUM* x = BN_CTX_get(ctx);
		BIGNUM* y = BN_CTX_get(ctx);
		BN_bin2bn(privKey, sizeof(privKey), k);
		EC_GROUP* curve = EC_GROUP_new_by_curve_name(NID_sect233r1);
		EC_POINT* pub = EC_POINT_new(curve);
		EC_POINT_mul(curve, pub, k, nullptr, nullptr, nullptr);
		EC_POINT_get_affine_coordinates(curve, pub, x, y, nullptr);
		uint8 pubKey[0x3C] = {};
		StoreRightAligned(x, pubKey, 0x1E);
		StoreRightAligned(y, pubKey + 0x1E, 0x1E);
		memcpy(c.publicKey, pubKey, sizeof(pubKey));
		EC_POINT_free(pub);
		BN_CTX_end(ctx);
		BN_CTX_free(ctx);
	}

	bool LoadCertificate(sint32 id, std::wstring_view certSubpath, std::wstring_view keySubpath)
	{
		const auto certData = FileStream::LoadIntoMemory(ActiveSettings::GetMlcPath(certSubpath));
		if (!certData)
			return false;
		std::optional<std::vector<uint8>> keyData;
		if (!keySubpath.empty())
		{
			const auto keyPath = ActiveSettings::GetMlcPath(keySubpath);
			keyData = FileStream::LoadIntoMemory(keyPath);
			if (!keyData || keyData->empty())
			{
				cemuLog_log(LogType::Force, "Unable to load private key file {}", keyPath.generic_string());
				return false;
			}
			if ((keyData->size() % 16) != 0)
			{
				cemuLog_log(LogType::Force, "Private key file has invalid length. Possibly corrupted? File: {}", keyPath.generic_string());
				return false;
			}
		}
		const unsigned char* certCursor = certData->data();
		X509* cert = d2i_X509(nullptr, &certCursor, (long)certData->size());
		if (!cert)
		{
			cemuLog_log(LogType::Force, "IOSU_CRYPTO: Unable to load certificate \"{}\"", boost::nowide::narrow(std::wstring(certSubpath)));
			return false;
		}
		StoredCertificate entry{ id, cert, *certData, nullptr, {} };
		if (keyData)
		{
			// RSA key encrypted with AES-128-CBC, key from OTP, zero IV
			uint8 aesKey[16];
			uint8 iv[16] = {};
			iosuCrypto_readOtpData(aesKey, 0x120 / 4, 16);
			std::vector<uint8> plain(keyData->size());
			AES128_CBC_decrypt(plain.data(), keyData->data(), (uint32)keyData->size(), aesKey, iv);
			const unsigned char* keyCursor = plain.data();
			RSA* rsa = d2i_RSAPrivateKey(nullptr, &keyCursor, (long)keyData->size());
			if (!rsa)
			{
				cemuLog_log(LogType::Force, "IOSU_CRYPTO: Unable to decrypt private key \"{}\"", boost::nowide::narrow(std::wstring(keySubpath)));
				return false;
			}
			// the EVP_PKEY takes ownership of the RSA key and is intentionally never freed
			EVP_PKEY* evp = EVP_PKEY_new();
			EVP_PKEY_assign_RSA(evp, rsa);
			const int derSize = i2d_PrivateKey(evp, nullptr);
			entry.pkeyDERData.resize(derSize > 0 ? derSize : 0);
			unsigned char* derCursor = entry.pkeyDERData.data();
			i2d_PrivateKey(evp, &derCursor);
			entry.pkey = rsa;
		}
		g_certStore.push_back(std::move(entry));
		return true;
	}

	void LoadCertificateStore()
	{
		if (g_certStoreComplete)
			return;
		if (!hasOtpMem)
			return; // private keys can not be decrypted without OTP
		bool complete = true;
		for (const CertificateFile& f : kCertificateFiles)
		{
			const std::wstring certPath = std::wstring(kCertTitleContentW) + f.name;
			const std::wstring keyPath = *f.key ? std::wstring(kCertTitleContentW) + f.key : std::wstring();
			if (!LoadCertificate(f.id, certPath, keyPath))
			{
				cemuLog_log(LogType::Force, "Unable to load certificate \"{}\"", boost::nowide::narrow(certPath));
				complete = false;
			}
		}
		g_certStoreComplete = complete;
	}

	// loads a dump that must have exactly 'size' bytes
	bool LoadDump(const fs::path& path, uint8* out, size_t size, const char* wrongSizeMsg, const char* missingMsg)
	{
		std::ifstream file(path, std::ifstream::in | std::ios::binary);
		if (!file.is_open())
		{
			cemuLog_log(LogType::Force, "{}", missingMsg);
			return false;
		}
		file.seekg(0, std::ifstream::end);
		const auto length = file.tellg();
		file.seekg(0, std::ifstream::beg);
		if (length != (std::streamoff)size)
		{
			cemuLog_log(LogType::Force, "{}", wrongSizeMsg);
			return false;
		}
		file.read((char*)out, size);
		return (bool)file;
	}

	bool MlcFileExists(std::string_view subpath, std::string& pathOut)
	{
		const auto path = ActiveSettings::GetMlcPath(subpath);
		pathOut = _pathToUtf8(path);
		return FileStream::LoadIntoMemory(path).has_value();
	}

	const char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	// standard base64 with '=' padding; returns the number of characters written
	size_t Base64Encode(const uint8* in, size_t len, char* out)
	{
		size_t o = 0;
		size_t i = 0;
		for (; i + 3 <= len; i += 3)
		{
			const uint32 v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
			out[o++] = kBase64Alphabet[(v >> 18) & 63];
			out[o++] = kBase64Alphabet[(v >> 12) & 63];
			out[o++] = kBase64Alphabet[(v >> 6) & 63];
			out[o++] = kBase64Alphabet[v & 63];
		}
		const size_t rest = len - i;
		if (rest != 0)
		{
			const uint32 v = (in[i] << 16) | (rest > 1 ? (in[i + 1] << 8) : 0);
			out[o++] = kBase64Alphabet[(v >> 18) & 63];
			out[o++] = kBase64Alphabet[(v >> 12) & 63];
			out[o++] = rest > 1 ? kBase64Alphabet[(v >> 6) & 63] : '=';
			out[o++] = '=';
		}
		return o;
	}
}

void iosuCrypto_readOtpData(void* output, sint32 wordIndex, sint32 size)
{
	memcpy(output, g_otp + wordIndex * 4, size);
}

void iosuCrypto_readSeepromData(void* output, sint32 wordIndex, sint32 size)
{
	memcpy(output, g_seeprom + wordIndex * 2, size);
}

bool iosuCrypto_getDeviceId(uint32* deviceId)
{
	*deviceId = 0;
	if (!hasOtpMem)
		return false;
	*deviceId = OtpWord(0x87);
	return true;
}

void iosuCrypto_getDeviceSerialString(char* serialString)
{
	if (!g_hasSeeprom)
	{
		strcpy(serialString, "FEH000000000");
		return;
	}
	// product code and serial number, both zero terminated within their field
	char code[32] = {};
	char serial[32] = {};
	iosuCrypto_readSeepromData(code, 0xAC, 8);
	iosuCrypto_readSeepromData(serial, 0xB0, 0x10);
	sprintf(serialString, "%s%s", code, serial);
}

std::string iosuCrypto_base64Encode(unsigned char const* bytes_to_encode, unsigned int inputLen)
{
	std::string out;
	out.resize((inputLen + 2) / 3 * 4);
	out.resize(Base64Encode(bytes_to_encode, inputLen, out.data()));
	return out;
}

void iosuCrypto_getDeviceCertPrivateKey(void* privKeyOut, sint32 len)
{
	cemu_assert(len == 30);
	memcpy(privKeyOut, g_devicePrivKey.keyData, 30);
}

void iosuCrypto_getDeviceCertificate(void* certOut, sint32 len)
{
	cemu_assert(len == 0x180);
	memcpy(certOut, &g_deviceCert, 0x180);
}

sint32 iosuCrypto_getDeviceCertificateBase64Encoded(char* output)
{
	const size_t len = Base64Encode((const uint8*)&g_deviceCert, sizeof(g_deviceCert), output);
	output[len] = '\0';
	return (sint32)len;
}

bool iosuCrypto_addClientCertificate(void* sslctx, sint32 certificateId)
{
	SSL_CTX* ctx = (SSL_CTX*)sslctx;
	const StoredCertificate* c = FindCertificate(certificateId);
	if (!c)
	{
		cemuLog_log(LogType::Force, "Certificate not found (verify required files for online mode or disable online mode)");
		return false;
	}
	if (SSL_CTX_use_certificate(ctx, c->cert) != 1)
	{
		cemuLog_log(LogType::Force, "Unable to setup certificate {}", certificateId);
		return false;
	}
	if (SSL_CTX_use_RSAPrivateKey(ctx, c->pkey) != 1)
	{
		cemuLog_log(LogType::Force, "Unable to setup certificate {} RSA private key", certificateId);
		return false;
	}
	if (SSL_CTX_check_private_key(ctx) == 0)
		cemuLog_log(LogType::Force, "Certificate private key could not be validated (verify required files for online mode or disable online mode)");
	return true;
}

bool iosuCrypto_addCACertificate(void* sslctx, sint32 certificateId)
{
	const StoredCertificate* c = FindCertificate(certificateId);
	if (!c)
		return false;
	X509_STORE_add_cert(SSL_CTX_get_cert_store((SSL_CTX*)sslctx), c->cert);
	return true;
}

bool iosuCrypto_addCustomCACertificate(void* sslctx, uint8* certData, sint32 certLength)
{
	const unsigned char* cursor = certData;
	X509* cert = d2i_X509(nullptr, &cursor, certLength);
	if (!cert)
	{
		cemuLog_log(LogType::Force, "Invalid custom server PKI certificate");
		return false;
	}
	X509_STORE_add_cert(SSL_CTX_get_cert_store((SSL_CTX*)sslctx), cert);
	return true;
}

uint8* iosuCrypto_getCertificateDataById(sint32 certificateId, sint32* certificateSize)
{
	StoredCertificate* c = const_cast<StoredCertificate*>(FindCertificate(certificateId));
	if (!c)
		return nullptr;
	*certificateSize = (sint32)c->certData.size();
	return c->certData.data();
}

uint8* iosuCrypto_getCertificatePrivateKeyById(sint32 certificateId, sint32* certificateSize)
{
	StoredCertificate* c = const_cast<StoredCertificate*>(FindCertificate(certificateId));
	if (!c)
		return nullptr;
	*certificateSize = (sint32)c->pkeyDERData.size();
	return c->pkeyDERData.data();
}

void iosuCrypto_init()
{
	hasOtpMem = LoadDump(ActiveSettings::GetUserDataPath("otp.bin"), g_otp, kOtpSize,
		"IOSU_CRYPTO: otp.bin has wrong size (must be 1024 bytes)", "IOSU_CRYPTO: No otp.bin found. Online mode cannot be used");
	g_hasSeeprom = LoadDump(ActiveSettings::GetUserDataPath("seeprom.bin"), g_seeprom, kSeepromSize,
		"IOSU_CRYPTO: seeprom.bin has wrong size (must be 512 bytes)", "IOSU_CRYPTO: No Seeprom.bin found. Online mode cannot be used");
	BuildDeviceCertificate();
	LoadCertificateStore();
}

sint32 iosuCrypt_checkRequirementsForOnlineMode(std::string& additionalErrorInfo)
{
	std::error_code ec;
	const auto otp = ActiveSettings::GetUserDataPath("otp.bin");
	if (!fs::exists(otp, ec))
		return IOS_CRYPTO_ONLINE_REQ_OTP_MISSING;
	if (fs::file_size(otp, ec) != kOtpSize)
		return IOS_CRYPTO_ONLINE_REQ_OTP_CORRUPTED;
	const auto seeprom = ActiveSettings::GetUserDataPath("seeprom.bin");
	if (!fs::exists(seeprom, ec))
		return IOS_CRYPTO_ONLINE_REQ_SEEPROM_MISSING;
	if (fs::file_size(seeprom, ec) != kSeepromSize)
		return IOS_CRYPTO_ONLINE_REQ_SEEPROM_CORRUPTED;
	for (const CertificateFile& f : kCertificateFiles)
	{
		for (const wchar_t* file : { f.name, f.key })
		{
			if (!*file)
				continue;
			const std::string subPath = fmt::format("{}{}", kCertTitleContent, boost::nowide::narrow(file));
			if (!MlcFileExists(subPath, additionalErrorInfo))
			{
				cemuLog_log(LogType::Force, "Missing dumped file for online mode: {}", subPath);
				return IOS_CRYPTO_ONLINE_REQ_MISSING_FILE;
			}
		}
	}
	return IOS_CRYPTO_ONLINE_REQ_OK;
}

std::vector<const wchar_t*> iosuCrypt_getCertificateKeys()
{
	std::vector<const wchar_t*> keys;
	for (const CertificateFile& f : kCertificateFiles)
		if (*f.key)
			keys.push_back(f.key);
	return keys;
}

std::vector<const wchar_t*> iosuCrypt_getCertificateNames()
{
	std::vector<const wchar_t*> names;
	for (const CertificateFile& f : kCertificateFiles)
		names.push_back(f.name);
	return names;
}
