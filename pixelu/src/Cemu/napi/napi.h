// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Client side of the console's online services: the account server (ACT), the
// SOAP shop services (NUS, IAS, ECS), the content servers (CCS), icon database
// (IDBE) and the update version lists (TAGAYA).

enum class NetworkService;
enum class CafeConsoleRegion;
enum class CafeConsoleLanguage;

enum class NAPI_RESULT
{
	SUCCESS = 0,
	FAILED = 1,        // transport failure or anything not covered below
	XML_ERROR = 2,     // reply could not be parsed or lacks expected elements
	DATA_ERROR = 3,    // reply parsed but carried unusable values
	SERVICE_ERROR = 4, // server reported an error; see serviceError
};

namespace NAPI
{
	// Identity used when talking to the account, IAS and ECS servers
	struct AuthInfo
	{
		// Nintendo Network ID credentials
		std::string accountId;
		std::array<uint8, 32> passwordHash;
		// console identity
		uint32 deviceId;
		std::string serial;
		CafeConsoleRegion region;
		std::string country;
		std::string deviceCertBase64;

		// device id as sent to the SOAP services: platform 5 in the upper word
		uint64 getDeviceIdWithPlatform()
		{
			return (uint64(5) << 32) | uint64(deviceId);
		}

		// obtained from IAS, attached to later SOAP requests
		struct
		{
			std::string accountId;
			std::string deviceToken;
		}IASToken;

		// when empty, the globally configured service is used
		std::optional<NetworkService> serviceOverwrite;

		NetworkService GetService() const;
	};

	// fills authInfo from the active account and console keys; false when those are missing
	bool NAPI_MakeAuthInfoFromCurrentAccount(AuthInfo& authInfo);

	// error numbers seen in SOAP replies
	enum class EC_ERROR_CODE
	{
		NONE = 0,
		ECS_INVALID_DEVICE_TOKEN = 903,
		IAS_DEVICE_CERT_ERROR = 928,
		IAS_INVALID_CHALLENGE = 954,
		ECS_NO_PERMISSION = 701,
		NUS_SOAP_INVALID_VERSION = 1301,
	};

	// error numbers seen in account server replies
	enum class ACT_ERROR_CODE
	{
		NONE = 0,
		ACT_GAME_SERVER_NOT_FOUND = 1021,
	};

	struct _NAPI_CommonResultSOAP
	{
		NAPI_RESULT apiError{ NAPI_RESULT::FAILED };
		EC_ERROR_CODE serviceError{ EC_ERROR_CODE::NONE };

		bool isValid() const { return apiError == NAPI_RESULT::SUCCESS; }
	};

	struct _NAPI_CommonResultACT
	{
		NAPI_RESULT apiError{ NAPI_RESULT::FAILED };
		ACT_ERROR_CODE serviceError{ ACT_ERROR_CODE::NONE };

		bool isValid() const { return apiError == NAPI_RESULT::SUCCESS; }
	};

	/* ---- account server ---- */

	struct ACTGetProfileResult
	{
		int todo;
	};

	bool ACT_GetProfile(AuthInfo& authInfo, ACTGetProfileResult& result);

	// layout is shared with emulated software, keep it fixed
	struct ACTNexToken
	{
		char token[0x201];
		uint8 _padding201[3];
		char nexPassword[0x41];
		uint8 _padding245[3];
		char host[0x10];
		uint16be port;
		uint8 _padding25A[2];
	};
	static_assert(sizeof(ACTNexToken) == 0x25C);
	static_assert(offsetof(ACTNexToken, nexPassword) == 0x204);
	static_assert(offsetof(ACTNexToken, host) == 0x248);
	static_assert(offsetof(ACTNexToken, port) == 0x258);

	struct ACTGetNexTokenResult : public _NAPI_CommonResultACT
	{
		ACTNexToken nexToken;
	};

	ACTGetNexTokenResult ACT_GetNexToken_WithCache(AuthInfo& authInfo, uint64 titleId, uint16 titleVersion, uint32 serverId);

	struct ACTGetIndependentTokenResult : public _NAPI_CommonResultACT
	{
		std::string token;
		sint64 expiresIn{};
	};

	ACTGetIndependentTokenResult ACT_GetIndependentToken_WithCache(AuthInfo& authInfo, uint64 titleId, uint16 titleVersion, std::string_view clientId);

	struct ACTConvertNnidToPrincipalIdResult : public _NAPI_CommonResultACT
	{
		bool isFound{false};
		uint32 principalId{};
	};

	ACTConvertNnidToPrincipalIdResult ACT_ACTConvertNnidToPrincipalId(AuthInfo& authInfo, std::string_view nnid);

	/* ---- NUS ---- */

	struct NAPI_NUSGetSystemCommonETicket_Result : public _NAPI_CommonResultSOAP
	{
		std::vector<uint8> eTicket;
		std::vector<std::vector<uint8>> certs;
	};

	NAPI_NUSGetSystemCommonETicket_Result NUS_GetSystemCommonETicket(AuthInfo& authInfo, uint64 titleId);

	/* ---- IAS / ECS ---- */

	struct NAPI_IASGetChallenge_Result : public _NAPI_CommonResultSOAP
	{
		std::string challenge;
	};

	struct NAPI_IASGetRegistrationInfo_Result : public _NAPI_CommonResultSOAP
	{
		std::string accountId;
		std::string deviceToken;
	};

	struct NAPI_ECSGetAccountStatus_Result : public _NAPI_CommonResultSOAP
	{
		enum class AccountStatus
		{
			REGISTERED = 'R',
			TRANSFERRED = 'T',
			UNREGISTERED = 'U',
			UNKNOWN = '\0',
		};

		std::string accountId;
		AccountStatus accountStatus{ AccountStatus::UNKNOWN };

		struct
		{
			std::string ContentPrefixURL;
			std::string UncachedContentPrefixURL;
			std::string SystemContentPrefixURL;
			std::string SystemUncachedContentPrefixURL;
			std::string EcsURL;
			std::string IasURL;
			std::string CasURL;
			std::string NusURL;
		}serviceURLs;
	};

	struct NAPI_ECSAccountListETicketIds_Result : public _NAPI_CommonResultSOAP
	{
		struct TIV
		{
			sint64 ticketId;
			uint32 ticketVersion;
		};

		std::vector<TIV> tivs;
	};

	struct NAPI_ECSAccountGetETickets_Result : public _NAPI_CommonResultSOAP
	{
		std::vector<uint8> eTickets;
		std::vector<std::vector<uint8>> certs;
	};

	NAPI_IASGetChallenge_Result IAS_GetChallenge(AuthInfo& authInfo);
	NAPI_IASGetRegistrationInfo_Result IAS_GetRegistrationInfo_QueryInfo(AuthInfo& authInfo, std::string challenge);

	NAPI_ECSGetAccountStatus_Result ECS_GetAccountStatus(AuthInfo& authInfo);
	NAPI_ECSAccountListETicketIds_Result ECS_AccountListETicketIds(AuthInfo& authInfo);
	NAPI_ECSAccountGetETickets_Result ECS_AccountGetETickets(AuthInfo& authInfo, sint64 ticketId);

	/* ---- CCS (plain HTTP file downloads) ---- */

	struct NAPI_CCSGetTMD_Result
	{
		bool isValid{ false };
		std::vector<uint8> tmdData;
	};

	struct NAPI_CCSGetETicket_Result
	{
		bool isValid{ false };
		std::vector<uint8> cetkData;
	};

	struct NAPI_CCSGetContentH3_Result
	{
		bool isValid{ false };
		std::vector<uint8> tmdData;
	};

	NAPI_CCSGetTMD_Result CCS_GetTMD(AuthInfo& authInfo, uint64 titleId, uint16 titleVersion);
	NAPI_CCSGetTMD_Result CCS_GetTMD(AuthInfo& authInfo, uint64 titleId);
	NAPI_CCSGetETicket_Result CCS_GetCETK(NetworkService service, uint64 titleId, uint16 titleVersion);
	bool CCS_GetContentFile(NetworkService service, uint64 titleId, uint32 contentId, bool(*cbWriteCallback)(void* userData, const void* ptr, size_t len, bool isLast), void* userData);
	NAPI_CCSGetContentH3_Result CCS_GetContentH3File(NetworkService service, uint64 titleId, uint32 contentId);

	/* ---- IDBE (title icons and names) ---- */

	// decrypted icon file, 0x12060 bytes
	struct IDBEIconDataV0
	{
		struct LanguageInfo
		{
			std::string GetGameNameUTF8();
			std::string GetGameLongNameUTF8();
			std::string GetPublisherNameUTF8();
		private:
			uint16be m_shortName[0x40]{};
			uint16be m_longName[0x80]{};
			uint16be m_publisher[0x40]{};
		};

		LanguageInfo& GetLanguageStrings(CafeConsoleLanguage index)
		{
			uint32 i = (uint32)index;
			return languageInfo[i < 16 ? i : 0];
		}

	private:
		uint32be m_titleIdHigh{};
		uint32be m_titleIdLow{};
		uint32 m_unknown[10];
		LanguageInfo languageInfo[16];   // at 0x30
		uint8 m_iconTga[0x10030]{};      // at 0x2030
	};
	static_assert(sizeof(IDBEIconDataV0::LanguageInfo) == 0x200);
	static_assert(sizeof(IDBEIconDataV0) == 0x12060);

	struct IDBEHeader
	{
		uint8 formatVersion;
		uint8 keyIndex;
		uint8 hashSHA256[32];
	};
	static_assert(sizeof(IDBEHeader) == 2+32);

	std::optional<IDBEIconDataV0> IDBE_Request(NetworkService networkService, uint64 titleId);
	// the downloaded file as is: header plus still encrypted icon data
	std::vector<uint8> IDBE_RequestRawEncrypted(NetworkService networkService, uint64 titleId);

	/* ---- TAGAYA (title update version lists) ---- */

	struct NAPI_VersionListVersion_Result
	{
		bool isValid{false};
		uint32 version{0};
		std::string fqdnURL;
	};

	NAPI_VersionListVersion_Result TAG_GetVersionListVersion(AuthInfo& authInfo);

	struct NAPI_VersionList_Result
	{
		bool isValid{ false };
		std::unordered_map<uint64, uint16> titleVersionList; // titleId -> latest version
	};

	NAPI_VersionList_Result TAG_GetVersionList(AuthInfo& authInfo, std::string_view fqdnURL, uint32 versionListVersion);
}

void NAPI_NUS_GetSystemUpdate();
void NAPI_NUS_GetSystemTitleHash();
