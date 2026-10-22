// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "Cafe/Account/Account.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "napi.h"
#include "napi_helper.h"
#include "curl/curl.h"
#include "config/ActiveSettings.h"
#include "util/helpers/StringHelpers.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

// Account server (ACT) client. Every call is authorised by an OAuth2 access
// token which is fetched with the account's password hash and then cached.

namespace NAPI
{
	namespace
	{
		constexpr uint64 kAccountSettingsTitleId = 0x0005001010001C00;

		std::string AccountServerBaseUrl(NetworkService service)
		{
			if (service == NetworkService::Pretendo)
				return PretendoURLs::ACTURL;
			if (service == NetworkService::Custom)
				return GetNetworkConfig().urls.ACT.GetValue();
			return NintendoURLs::ACTURL;
		}

		void BeginAccountRequest(CurlRequestHelper& req, AuthInfo& authInfo, std::string_view path)
		{
			const NetworkService service = authInfo.GetService();
			req.initate(service, AccountServerBaseUrl(service).append(path), CurlRequestHelper::SERVER_SSL_CONTEXT::ACT);
		}

		// headers every account request carries
		void AddClientHeaders(CurlRequestHelper& req, const AuthInfo& authInfo)
		{
			req.addHeaderField("X-Nintendo-Platform-ID", "1");
			req.addHeaderField("X-Nintendo-Device-Type", "2");
			req.addHeaderField("X-Nintendo-Client-ID", "a2efa818a34fa16b8afbc8a74eba3eda");
			req.addHeaderField("X-Nintendo-Client-Secret", "c91cdb5658bd4954ade78533a339cf9a");
			req.addHeaderField("Accept", "*/*");
			req.addHeaderField("X-Nintendo-System-Version", authInfo.region == CafeConsoleRegion::USA ? "0270" : "0260");
		}

		void AddDeviceHeaders(CurlRequestHelper& req, const AuthInfo& authInfo)
		{
			req.addHeaderField("X-Nintendo-Device-ID", fmt::format("{}", authInfo.deviceId));
			req.addHeaderField("X-Nintendo-Serial-Number", authInfo.serial);
		}

		void AddLocaleHeaders(CurlRequestHelper& req, const AuthInfo& authInfo)
		{
			req.addHeaderField("X-Nintendo-Region", fmt::format("{}", (uint32)authInfo.region));
			req.addHeaderField("X-Nintendo-Country", authInfo.country);
		}

		void AddTitleHeaders(CurlRequestHelper& req, uint64 titleId, uint32 uniqueId, uint16 titleVersion)
		{
			req.addHeaderField("X-Nintendo-FPD-Version", "0000");
			req.addHeaderField("X-Nintendo-Environment", "L1");
			req.addHeaderField("X-Nintendo-Title-ID", fmt::format("{:016x}", titleId));
			req.addHeaderField("X-Nintendo-Unique-ID", fmt::format("{:05x}", uniqueId));
			req.addHeaderField("X-Nintendo-Application-Version", fmt::format("{:04x}", titleVersion));
		}

		uint32 UniqueIdFromTitle(uint64 titleId)
		{
			return (uint32)((titleId >> 8) & 0xFFFFF);
		}

		// the standard header set for requests made on behalf of a title
		void AddTokenRequestHeaders(CurlRequestHelper& req, const AuthInfo& authInfo, uint64 titleId, uint32 uniqueId, uint16 titleVersion, std::string_view bearer)
		{
			AddClientHeaders(req, authInfo);
			AddDeviceHeaders(req, authInfo);
			AddLocaleHeaders(req, authInfo);
			AddTitleHeaders(req, titleId, uniqueId, titleVersion);
			req.addHeaderField("Authorization", fmt::format("Bearer {}", bearer));
		}

		// Loads the reply and handles an <errors> document. Returns false when the
		// caller should stop (result is then already filled in).
		bool LoadAccountReply(CurlRequestHelper& req, _NAPI_CommonResultACT& result, pugi::xml_document& doc)
		{
			const std::vector<uint8>& body = req.getReceivedData();
			if (!doc.load_buffer(body.data(), body.size()))
			{
				cemuLog_log(LogType::Force, "Invalid XML in account service response");
				result.apiError = NAPI_RESULT::XML_ERROR;
				return false;
			}
			pugi::xml_node errorList = doc.child("errors");
			if (!errorList)
				return true;
			result.apiError = NAPI_RESULT::XML_ERROR;
			pugi::xml_node firstError = errorList.child("error");
			if (!firstError)
				return false;
			const sint32 code = StringHelpers::ToInt(std::string_view(firstError.child_value("code")));
			if (code == 0)
			{
				cemuLog_log(LogType::Force, "Account response with unexpected error code 0");
				return false;
			}
			result.apiError = NAPI_RESULT::SERVICE_ERROR;
			result.serviceError = (ACT_ERROR_CODE)code;
			cemuLog_log(LogType::Force, "Account response with error code {}", code);
			std::string_view message = firstError.child_value("message");
			if (!message.empty())
				cemuLog_log(LogType::Force, "Message from server: {}", message);
			return false;
		}

		bool IsSameCredentials(const AuthInfo& authInfo, const std::string& accountId, const std::array<uint8, 32>& passwordHash)
		{
			return authInfo.accountId == accountId && authInfo.passwordHash == passwordHash;
		}

		sint64 NowInSeconds()
		{
			return (sint64)HighResolutionTimer::now().getTickInSeconds();
		}

		/* access token cache */

		struct AccessToken : _NAPI_CommonResultACT
		{
			std::string token;
			std::string refreshToken;
		};

		struct CachedAccessToken
		{
			std::string accountId;
			std::array<uint8, 32> passwordHash;
			NetworkService service;
			std::string token;
			std::string refreshToken;
			uint64 validUntil;
		};

		std::mutex s_accessTokenMtx;
		std::vector<CachedAccessToken> s_accessTokens;

		/* nex token cache (entries never expire) */

		struct CachedNexToken
		{
			std::string accountId;
			std::array<uint8, 32> passwordHash;
			NetworkService service;
			uint32 gameServerId;
			ACTNexToken token;
		};

		std::mutex s_nexTokenMtx;
		std::vector<CachedNexToken> s_nexTokens;

		/* service ("independent") token cache */

		struct CachedServiceToken
		{
			std::string accountId;
			std::array<uint8, 32> passwordHash;
			NetworkService service;
			std::string clientId;
			sint64 validUntil;
			std::string token;
		};

		std::mutex s_serviceTokenMtx;
		std::vector<CachedServiceToken> s_serviceTokens;

		AccessToken GetAccessToken(AuthInfo& authInfo, uint64 titleId, uint16 titleVersion)
		{
			AccessToken result{};
			const NetworkService service = authInfo.GetService();
			{
				std::lock_guard lock(s_accessTokenMtx);
				for (size_t i = 0; i < s_accessTokens.size();)
				{
					CachedAccessToken& entry = s_accessTokens[i];
					if (entry.service != service || !IsSameCredentials(authInfo, entry.accountId, entry.passwordHash))
					{
						i++;
						continue;
					}
					if (HighResolutionTimer::now().getTickInSeconds() >= entry.validUntil)
					{
						s_accessTokens.erase(s_accessTokens.begin() + i);
						continue;
					}
					result.token = entry.token;
					result.refreshToken = entry.refreshToken;
					result.apiError = NAPI_RESULT::SUCCESS;
					return result;
				}
			}

			CurlRequestHelper req;
			BeginAccountRequest(req, authInfo, "/v1/api/oauth20/access_token/generate");
			AddClientHeaders(req, authInfo);
			AddDeviceHeaders(req, authInfo);
			AddLocaleHeaders(req, authInfo);
			req.addHeaderField("X-Nintendo-Device-Cert", authInfo.deviceCertBase64);
			AddTitleHeaders(req, titleId, UniqueIdFromTitle(titleId), titleVersion);

			std::string hashHex;
			for (uint8 b : authInfo.passwordHash)
				hashHex.append(fmt::format("{:02x}", b));
			req.addPostField("grant_type", "password");
			req.addPostField("user_id", authInfo.accountId);
			req.addPostField("password", hashHex);
			req.addPostField("password_type", "hash");
			req.addHeaderField("Content-type", "application/x-www-form-urlencoded");

			if (!req.submitRequest(true))
			{
				cemuLog_log(LogType::Force, "Failed request /oauth20/access_token/generate");
				result.apiError = NAPI_RESULT::FAILED;
				return result;
			}

			// <OAuth20><access_token><token/><refresh_token/><expires_in/></access_token></OAuth20>
			pugi::xml_document doc;
			if (!LoadAccountReply(req, result, doc))
				return result;
			pugi::xml_node root = doc.child("OAuth20");
			if (!root)
			{
				cemuLog_log(LogType::Force, "Response does not contain OAuth20 node");
				result.apiError = NAPI_RESULT::XML_ERROR;
				return result;
			}
			pugi::xml_node access = root.child("access_token");
			if (!access)
			{
				cemuLog_log(LogType::Force, "Response does not contain OAuth20/access_token node");
				result.apiError = NAPI_RESULT::XML_ERROR;
				return result;
			}
			result.token = access.child_value("token");
			result.refreshToken = access.child_value("refresh_token");
			result.apiError = NAPI_RESULT::SUCCESS;
			if (result.token.empty())
				cemuLog_log(LogType::Force, "OAuth20/token is empty");

			// shave off a little to account for the time the request took
			sint64 lifetime = StringHelpers::ToInt64(std::string_view(access.child_value("expires_in"))) - 30;
			if (lifetime > 0)
			{
				std::lock_guard lock(s_accessTokenMtx);
				s_accessTokens.push_back({ authInfo.accountId, authInfo.passwordHash, service, result.token, result.refreshToken,
					HighResolutionTimer::now().getTickInSeconds() + (uint64)lifetime });
			}
			return result;
		}

		// shared failure mapping when no access token could be obtained
		template<typename TResult>
		void PropagateAccessTokenFailure(const AccessToken& token, TResult& result)
		{
			if (token.apiError == NAPI_RESULT::SERVICE_ERROR)
			{
				result.apiError = NAPI_RESULT::SERVICE_ERROR;
				result.serviceError = token.serviceError;
			}
			else
				result.apiError = NAPI_RESULT::DATA_ERROR;
		}

		void CopyTruncated(char* dst, std::string_view src, size_t maxLen)
		{
			memcpy(dst, src.data(), std::min(src.size(), maxLen));
		}
	}

	bool ACT_GetProfile(AuthInfo& authInfo, ACTGetProfileResult& result)
	{
		CurlRequestHelper req;
		BeginAccountRequest(req, authInfo, "/v1/api/people/@me/profile");
		AddClientHeaders(req, authInfo);
		AddDeviceHeaders(req, authInfo);
		[[maybe_unused]] AccessToken token = GetAccessToken(authInfo, kAccountSettingsTitleId, 0x1C);
		cemu_assert_unimplemented();
		return true;
	}

	ACTGetNexTokenResult ACT_GetNexToken_WithCache(AuthInfo& authInfo, uint64 titleId, uint16 titleVersion, uint32 serverId)
	{
		ACTGetNexTokenResult result{};
		{
			std::lock_guard lock(s_nexTokenMtx);
			for (const CachedNexToken& entry : s_nexTokens)
			{
				if (IsSameCredentials(authInfo, entry.accountId, entry.passwordHash) && entry.service == authInfo.GetService() && entry.gameServerId == serverId)
				{
					result.nexToken = entry.token;
					result.apiError = NAPI_RESULT::SUCCESS;
					return result;
				}
			}
		}

		AccessToken access = GetAccessToken(authInfo, titleId, titleVersion);
		if (!access.isValid())
		{
			cemuLog_log(LogType::Force, "ACT_GetNexToken(): Failed to retrieve OAuth token");
			PropagateAccessTokenFailure(access, result);
			return result;
		}

		CurlRequestHelper req;
		BeginAccountRequest(req, authInfo, fmt::format("/v1/api/provider/nex_token/@me?game_server_id={:08X}", serverId));
		AddTokenRequestHeaders(req, authInfo, titleId, UniqueIdFromTitle(titleId), titleVersion, access.token);
		if (!req.submitRequest(false))
		{
			cemuLog_log(LogType::Force, "Failed request /provider/nex_token/@me");
			result.apiError = NAPI_RESULT::FAILED;
			return result;
		}

		// <nex_token><host/><nex_password/><pid/><port/><token/></nex_token>
		pugi::xml_document doc;
		if (!LoadAccountReply(req, result, doc))
			return result;
		pugi::xml_node nexNode = doc.child("nex_token");
		if (!nexNode)
		{
			cemuLog_log(LogType::Force, "Response does not contain NexToken node");
			result.apiError = NAPI_RESULT::XML_ERROR;
			return result;
		}
		std::string_view host = nexNode.child_value("host");
		std::string_view password = nexNode.child_value("nex_password");
		std::string_view portText = nexNode.child_value("port");
		std::string_view token = nexNode.child_value("token");

		ACTNexToken& out = result.nexToken;
		memset(&out, 0, sizeof(out));
		// each field keeps room for its terminating zero
		if (host.size() > sizeof(out.host) - 1)
			cemuLog_log(LogType::Force, "NexToken response: host field too long");
		if (password.size() > sizeof(out.nexPassword) - 1)
			cemuLog_log(LogType::Force, "NexToken response: nex_password field too long");
		if (token.size() > sizeof(out.token) - 1)
			cemuLog_log(LogType::Force, "NexToken response: token field too long");
		CopyTruncated(out.host, host, sizeof(out.host) - 1);
		CopyTruncated(out.nexPassword, password, sizeof(out.nexPassword) - 1);
		CopyTruncated(out.token, token, sizeof(out.token) - 1);
		out.port = (uint16)StringHelpers::ToInt(portText);
		result.apiError = NAPI_RESULT::SUCCESS;

		std::lock_guard lock(s_nexTokenMtx);
		s_nexTokens.push_back({ authInfo.accountId, authInfo.passwordHash, authInfo.GetService(), serverId, out });
		return result;
	}

	ACTGetIndependentTokenResult ACT_GetIndependentToken_WithCache(AuthInfo& authInfo, uint64 titleId, uint16 titleVersion, std::string_view clientId)
	{
		ACTGetIndependentTokenResult result{};
		{
			std::lock_guard lock(s_serviceTokenMtx);
			for (size_t i = 0; i < s_serviceTokens.size();)
			{
				CachedServiceToken& entry = s_serviceTokens[i];
				const sint64 now = NowInSeconds();
				if (now >= entry.validUntil)
				{
					s_serviceTokens.erase(s_serviceTokens.begin() + i);
					continue;
				}
				if (IsSameCredentials(authInfo, entry.accountId, entry.passwordHash) && entry.service == authInfo.GetService() && entry.clientId == clientId)
				{
					result.token = entry.token;
					result.expiresIn = std::max<sint64>(entry.validUntil - NowInSeconds(), 0);
					result.apiError = NAPI_RESULT::SUCCESS;
					return result;
				}
				i++;
			}
		}

		AccessToken access = GetAccessToken(authInfo, titleId, titleVersion);
		if (!access.isValid())
		{
			cemuLog_log(LogType::Force, "ACT_GetIndependentToken(): Failed to retrieve OAuth token");
			PropagateAccessTokenFailure(access, result);
			return result;
		}

		CurlRequestHelper req;
		BeginAccountRequest(req, authInfo, fmt::format("/v1/api/provider/service_token/@me?client_id={}", clientId));
		AddTokenRequestHeaders(req, authInfo, titleId, UniqueIdFromTitle(titleId), titleVersion, access.token);
		if (!req.submitRequest(false))
		{
			cemuLog_log(LogType::Force, "Failed request /provider/service_token/@me");
			result.apiError = NAPI_RESULT::FAILED;
			return result;
		}

		// <service_token><token/></service_token>
		pugi::xml_document doc;
		if (!LoadAccountReply(req, result, doc))
			return result;
		pugi::xml_node tokenNode = doc.child("service_token");
		if (!tokenNode)
		{
			cemuLog_log(LogType::Force, "Response does not contain service_token node");
			result.apiError = NAPI_RESULT::XML_ERROR;
			return result;
		}
		result.token = tokenNode.child_value("token");
		result.apiError = NAPI_RESULT::SUCCESS;

		// the server does not say how long the token lasts, assume an hour
		std::lock_guard lock(s_serviceTokenMtx);
		s_serviceTokens.push_back({ authInfo.accountId, authInfo.passwordHash, authInfo.GetService(), std::string(clientId), NowInSeconds() + 3600, result.token });
		return result;
	}

	ACTConvertNnidToPrincipalIdResult ACT_ACTConvertNnidToPrincipalId(AuthInfo& authInfo, std::string_view nnid)
	{
		ACTConvertNnidToPrincipalIdResult result{};
		AccessToken access = GetAccessToken(authInfo, kAccountSettingsTitleId, 0);
		if (!access.isValid())
		{
			cemuLog_log(LogType::Force, "ACT_ACTConvertNnidToPrincipalId(): Failed to retrieve OAuth token");
			PropagateAccessTokenFailure(access, result);
			return result;
		}

		CurlRequestHelper req;
		BeginAccountRequest(req, authInfo, fmt::format("/v1/api/admin/mapped_ids?input_type=user_id&output_type=pid&input={}", nnid));
		AddTokenRequestHeaders(req, authInfo, kAccountSettingsTitleId, 0x50010, 0, access.token);
		if (!req.submitRequest(false))
		{
			cemuLog_log(LogType::Force, "Failed request /admin/mapped_ids");
			result.apiError = NAPI_RESULT::FAILED;
			return result;
		}

		// <mapped_ids><mapped_id><in_id/><out_id/></mapped_id></mapped_ids>
		pugi::xml_document doc;
		if (!LoadAccountReply(req, result, doc))
			return result;
		pugi::xml_node list = doc.child("mapped_ids");
		if (!list)
		{
			cemuLog_log(LogType::Force, "Response does not contain mapped_ids node");
			result.apiError = NAPI_RESULT::XML_ERROR;
			return result;
		}
		pugi::xml_node mapping = list.child("mapped_id");
		if (!mapping)
		{
			cemuLog_log(LogType::Force, "Response does not contain mapped_id node");
			result.apiError = NAPI_RESULT::XML_ERROR;
			return result;
		}
		std::string_view pidText = mapping.child_value("out_id");
		result.apiError = NAPI_RESULT::SUCCESS;
		result.isFound = !pidText.empty();
		result.principalId = result.isFound ? StringHelpers::ToInt(pidText) : 0;
		return result;
	}

	bool NAPI_MakeAuthInfoFromCurrentAccount(AuthInfo& authInfo)
	{
		authInfo = {};
		if (!NCrypto::SEEPROM_IsPresent())
			return false;
		const Account& account = Account::GetCurrentAccount();
		authInfo.accountId = account.GetAccountId();
		authInfo.passwordHash = account.GetAccountPasswordCache();
		const bool hasPassword = std::any_of(authInfo.passwordHash.begin(), authInfo.passwordHash.end(), [](uint8 b) { return b != 0; });
		if (!hasPassword)
		{
			static std::atomic_bool s_warned{ false };
			if (!s_warned.exchange(true))
				cemuLog_log(LogType::Force, "Account login is impossible because the cached password hash is not set");
			return false;
		}
		authInfo.deviceId = NCrypto::GetDeviceId();
		authInfo.serial = NCrypto::GetSerial();
		authInfo.region = NCrypto::SEEPROM_GetRegion();
		authInfo.country = NCrypto::GetCountryAsString(account.GetCountry());
		authInfo.deviceCertBase64 = NCrypto::CertECC::GetDeviceCertificate().encodeToBase64();
		return true;
	}
}
