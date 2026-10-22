// Original code for RetroPixels (PixelU). Not derived from Cemu.
// ACT (account) service: per-slot account data taken from the configured PixelU account,
// the private /dev/act request used by nn_act, and NNID token requests.
#include "iosu_act.h"
#include "iosu_ioctl.h"

#include "Cafe/OS/libs/nn_common.h"

#include <algorithm>
#include <mutex>

#include "openssl/evp.h"
#include "openssl/sha.h"
#include "Cafe/Account/Account.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"

#include "Cemu/napi/napi.h"
#include "Cemu/ncrypto/ncrypto.h"

#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Cafe/IOSU/nn/iosu_nn_service.h"

static_assert(sizeof(NAPI::ACTNexToken) == 0x25C);

namespace
{
	constexpr size_t kSlotCount = 12;

	struct SlotData
	{
		bool inUse;
		bool networkAccount;
		uint8 uuid[16];
		uint32 persistentId;
		uint64 transferableIdBase;
		uint32 simpleAddressId;
		uint32 principalId;
		char accountId[64];
		uint8 passwordCache[32];
		uint32 countryIndex;
		char country[8];
		char timeZoneId[64];
		sint64 utcOffsetSeconds;
		FFLData_t mii;
		uint16le nickname[ACT_NICKNAME_LENGTH];
	};

	SlotData g_slots[kSlotCount] = {};
	bool g_slotsLoaded = false;
	std::mutex g_slotMutex; // guards token requests
	bool g_legacyThreadStarted = false;

	constexpr nnResult kResultOk = 0;
	constexpr nnResult kResultInvalidValue = BUILD_NN_RESULT(NN_RESULT_LEVEL_LVL6, NN_RESULT_MODULE_NN_ACT, 0x12F00); // 0xC0712F00
	constexpr nnResult kResultOutOfRange = BUILD_NN_RESULT(NN_RESULT_LEVEL_LVL6, NN_RESULT_MODULE_NN_ACT, 0x12D80); // 0xC0712D80
	constexpr nnResult kResultNoAccount = BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_ACT, NN_ACT_RESULT_ACCOUNT_DOES_NOT_EXIST); // 0xA071F480
	constexpr nnResult kResultNotNetworkAccount = BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_ACT, 0x1FE80); // 0xA071FE80

	template<size_t N>
	void CopyText(char (&dst)[N], std::string_view src)
	{
		size_t n = std::min(src.size(), N - 1);
		memcpy(dst, src.data(), n);
		dst[n] = '\0';
	}

	void LoadSlot(SlotData& slot, const Account& account, bool onlineAllowed)
	{
		slot = {};
		slot.inUse = true;
		slot.networkAccount = account.IsValidOnlineAccount();
		memcpy(slot.uuid, account.GetUuid().data(), sizeof(slot.uuid));
		slot.persistentId = account.GetPersistentId();
		slot.transferableIdBase = account.GetTransferableIdBase();
		slot.simpleAddressId = account.GetSimpleAddressId();
		slot.principalId = account.GetPrincipalId();
		CopyText(slot.accountId, account.GetAccountId());
		memcpy(slot.passwordCache, account.GetAccountPasswordCache().data(), sizeof(slot.passwordCache));
		slot.countryIndex = account.GetCountry();
		CopyText(slot.country, NCrypto::GetCountryAsString(slot.countryIndex));
		CopyText(slot.timeZoneId, account.GetTimeZoneId());
		slot.utcOffsetSeconds = account.GetUtcOffset() / 1'000'000;
		memcpy(&slot.mii, account.GetMiiData().data(), sizeof(FFLData_t));
		std::wstring_view name = account.GetMiiName();
		for (size_t i = 0; i < std::min<size_t>(name.size(), ACT_NICKNAME_LENGTH); i++)
			slot.nickname[i] = (uint16)name[i];
		if (!onlineAllowed)
		{
			// offline mode: every account behaves like a local one
			slot.networkAccount = false;
			slot.principalId = 0;
			slot.simpleAddressId = 0;
			memset(slot.accountId, 0, sizeof(slot.accountId));
		}
	}

	void LoadSlots()
	{
		if (g_slotsLoaded)
			return;
		// only the account selected in the settings is exposed, always in slot 1
		const Account& active = Account::GetAccount(ActiveSettings::GetPersistentId());
		LoadSlot(g_slots[0], active, ActiveSettings::IsOnlineEnabled());
		cemuLog_log(LogType::Force, "IOSU_ACT: slot 1 uses account {}", boost::nowide::narrow(active.GetMiiName()));
		g_slotsLoaded = true;
	}

	// slot number -> table entry, nullptr for unknown or empty slots
	SlotData* FindSlot(uint8 slotNo)
	{
		size_t index;
		if (slotNo == iosu::act::ACT_SLOT_CURRENT || slotNo == 0xFF)
			index = 0;
		else if (slotNo >= 1 && slotNo <= kSlotCount)
			index = slotNo - 1;
		else
			return nullptr;
		return g_slots[index].inUse ? &g_slots[index] : nullptr;
	}

	struct ServerErrorMapping
	{
		uint32 serverCode;
		uint32 errorCode;
	};

	// account server error -> NN_ERROR_CODE reported to the title
	constexpr ServerErrorMapping kServerErrors[] =
	{
		{ 1, 2401 }, { 2, 2402 }, { 3, 2403 }, { 4, 2458 }, { 5, 2642 }, { 6, 2641 }, { 7, 2522 }, { 8, 2534 },
		{ 9, 2404 }, { 10, 2451 }, { 11, 2511 }, { 12, 2812 }, { 100, 2571 }, { 101, 2572 }, { 103, 2575 },
		{ 104, 2452 }, { 105, 2592 }, { 106, 2611 }, { 107, 2502 }, { 108, 2802 }, { 109, 2503 }, { 110, 2501 },
		{ 111, 2632 }, { 112, 2631 }, { 113, 2452 }, { 114, 2593 }, { 115, 2591 }, { 116, 2614 }, { 117, 2651 },
		{ 118, 2484 }, { 119, 2803 }, { 120, 2813 }, { 121, 2804 }, { 122, 2814 }, { 123, 2882 }, { 124, 2512 },
		{ 125, 2485 }, { 126, 2805 }, { 127, 2815 }, { 128, 2661 }, { 129, 2615 }, { 130, 2531 }, { 131, 2616 },
		{ 132, 2822 }, { 133, 2832 }, { 134, 2823 }, { 135, 2833 }, { 136, 2824 }, { 137, 2834 }, { 138, 2825 },
		{ 139, 2835 }, { 142, 2635 }, { 143, 2634 }, { 1004, 2503 }, { 1006, 2471 }, { 1016, 2532 }, { 1017, 2483 },
		{ 1018, 2533 }, { 1019, 2481 }, { 1020, 2473 }, { 1021, 2482 }, { 1022, 2472 }, { 1023, 2612 },
		{ 1024, 2535 }, { 1025, 2536 }, { 1031, 2537 }, { 1032, 2636 }, { 1033, 2662 }, { 1035, 2671 },
		{ 1036, 2679 }, { 1037, 2672 }, { 1038, 2674 }, { 1039, 2680 }, { 1040, 2675 }, { 1041, 2673 },
		{ 1042, 2676 }, { 1043, 2681 }, { 1044, 2678 }, { 1045, 2677 }, { 1046, 2596 }, { 1100, 2541 },
		{ 1101, 2542 }, { 1103, 2594 }, { 1104, 2576 }, { 1105, 2613 }, { 1106, 2633 }, { 1107, 2577 },
		{ 1111, 2538 }, { 1115, 2597 }, { 1125, 2585 }, { 1126, 2586 }, { 1134, 2587 }, { 1200, 2884 },
		{ 2001, 2931 }, { 2002, 2881 }, { 2999, 2883 },
	};

	nnResult MapServerError(NAPI::ACT_ERROR_CODE code)
	{
		for (const ServerErrorMapping& m : kServerErrors)
		{
			if (m.serverCode == (uint32)code)
				return nnResultStatus(NN_RESULT_MODULE_NN_ACT, (NN_ERROR_CODE)m.errorCode);
		}
		cemuLog_log(LogType::Force, "IOSU_ACT: unknown account server error {}", (uint32)code);
		return nnResultStatus(NN_RESULT_MODULE_NN_ACT, NN_ERROR_CODE::ACT_UNKNOWN_SERVER_ERROR);
	}

	uint32 LookupPrincipalId(const char* nnid, uint32* pidOut)
	{
		NAPI::AuthInfo auth;
		NAPI::NAPI_MakeAuthInfoFromCurrentAccount(auth);
		NAPI::ACTConvertNnidToPrincipalIdResult r = NAPI::ACT_ACTConvertNnidToPrincipalId(auth, nnid);
		if (!r.isValid() || !r.isFound)
		{
			// titles such as the friend list expect this lookup to succeed, so only a status level result is returned
			*pidOut = 0;
			return BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_ACT, 0);
		}
		*pidOut = r.principalId;
		return 0;
	}

	nnResult RequestNexToken(uint8 slotNo, uint64 titleId, uint16 titleVersion, uint32 serverId, uint8* out, uint32 outSize)
	{
		if (slotNo != iosu::act::ACT_SLOT_CURRENT)
			return kResultInvalidValue;
		std::lock_guard lock(g_slotMutex);
		SlotData* slot = FindSlot(slotNo);
		if (!slot)
			return kResultNoAccount;
		if (!slot->networkAccount)
			return kResultNotNetworkAccount;
		cemu_assert_debug(ActiveSettings::IsOnlineEnabled());
		if (outSize != sizeof(NAPI::ACTNexToken))
			return kResultOutOfRange;
		NAPI::AuthInfo auth;
		NAPI::NAPI_MakeAuthInfoFromCurrentAccount(auth);
		NAPI::ACTGetNexTokenResult r = NAPI::ACT_GetNexToken_WithCache(auth, titleId, titleVersion, serverId);
		if (r.isValid())
		{
			memcpy(out, &r.nexToken, sizeof(NAPI::ACTNexToken));
			return kResultOk;
		}
		if (r.apiError == NAPI_RESULT::SERVICE_ERROR)
		{
			nnResult mapped = MapServerError(r.serviceError);
			cemu_assert_debug((mapped & 0x80000000) != 0);
			return mapped;
		}
		return nnResultStatus(NN_RESULT_MODULE_NN_ACT, NN_ERROR_CODE::ACT_UNKNOWN_SERVER_ERROR);
	}

	nnResult RequestIndependentToken(uint8 slotNo, uint64 titleId, uint16 titleVersion, std::string_view clientId, uint8* out, uint32 outSize)
	{
		constexpr size_t kMaxTokenSize = 513; // 512 characters and the terminator
		if (slotNo != iosu::act::ACT_SLOT_CURRENT)
			return kResultInvalidValue;
		NAPI::AuthInfo auth;
		{
			std::lock_guard lock(g_slotMutex);
			SlotData* slot = FindSlot(slotNo);
			if (!slot)
				return kResultNoAccount;
			if (!slot->networkAccount)
				return kResultNotNetworkAccount;
			cemu_assert_debug(ActiveSettings::IsOnlineEnabled());
			if (outSize < kMaxTokenSize)
				return kResultOutOfRange;
			NAPI::NAPI_MakeAuthInfoFromCurrentAccount(auth);
		}
		// the server request runs without holding the account lock
		NAPI::ACTGetIndependentTokenResult r = NAPI::ACT_GetIndependentToken_WithCache(auth, titleId, titleVersion, clientId);
		if (!r.isValid())
			return 0x80000000;
		size_t n = std::min(r.token.size(), kMaxTokenSize);
		if (n > 0)
		{
			memcpy(out, r.token.data(), n);
			out[n] = '\0';
		}
		return 0;
	}

	// name based UUID: the first 10 bytes stay, the last 6 come from SHA-256(be32 name || 3A275E09 || last 6 bytes)
	void DeriveNamedUuid(uint8* uuid, uint32 name)
	{
		uint8 prefix[8] = { (uint8)(name >> 24), (uint8)(name >> 16), (uint8)(name >> 8), (uint8)name, 0x3A, 0x27, 0x5E, 0x09 };
		uint8 digest[SHA256_DIGEST_LENGTH];
		EVP_MD_CTX* md = EVP_MD_CTX_new();
		EVP_DigestInit(md, EVP_sha256());
		EVP_DigestUpdate(md, prefix, sizeof(prefix));
		EVP_DigestUpdate(md, uuid + 10, 6);
		EVP_DigestFinal_ex(md, digest, nullptr);
		EVP_MD_CTX_free(md);
		memcpy(uuid + 10, digest + 10, 6);
	}

	// ---- legacy /dev/act request queue ----

	struct LegacyActCommand
	{
		uint32be ukn00;
		uint32be ukn04;
		uint32be ukn08;
		uint32be subcommand;
		uint8 ukn10;
		uint8 ukn11;
		uint8 ukn12;
		uint8 slot;
		uint32be unique;
	};

	struct LegacyTransferableIdReply
	{
		uint32be result;
		uint8 transferableId[8];
	};

	constexpr uint32 kLegacyGetTransferableId = 4;
	constexpr uint32 kLegacyInitialize = 0x14;

	void HandleLegacyCommand(ioQueueEntry_t* entry)
	{
		if (entry->countIn != 1 || entry->countOut != 1)
			assert_dbg();
		ioBufferVector_t& vec = entry->bufferVectors[0];
		LegacyActCommand* cmd = (LegacyActCommand*)vec.unknownBuffer.GetPtr();
		switch ((uint32)cmd->subcommand)
		{
		case kLegacyInitialize:
			break;
		case kLegacyGetTransferableId:
		{
			LegacyTransferableIdReply* reply = (LegacyTransferableIdReply*)vec.buffer.GetPtr();
			reply->result = 0;
			*(uint64be*)reply->transferableId = 0x1122334455667788ull; // fixed placeholder value
			break;
		}
		default:
			assert_dbg();
			break;
		}
	}

	// returns false when the slot is empty; the request is then already answered
	bool RequireSlot(iosuActCemuRequest_t* req, SlotData*& slot)
	{
		slot = FindSlot(req->accountSlot);
		if (slot)
			return true;
		req->setACTReturnCode(kResultNoAccount);
		req->resultU64.u64 = 0;
		return false;
	}

	void HandlePrivateRequest(iosuActCemuRequest_t* req)
	{
		SlotData* slot = nullptr;
		switch (req->requestCode)
		{
		case IOSU_ARC_INIT:
			LoadSlots();
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_ACCOUNT_ID:
			if (!RequireSlot(req, slot))
				return;
			strcpy(req->resultString.strBuffer, slot->accountId);
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_UUID:
		{
			uint8* uuid = req->resultBinary.binBuffer;
			if (req->accountSlot == 0xFF)
			{
				// console wide uuid, a fixed placeholder pattern
				for (uint32 i = 0; i < 16; i++)
					uuid[i] = (uint8)(249 * i - 1);
			}
			else
			{
				if (!RequireSlot(req, slot))
					return;
				memcpy(uuid, slot->uuid, 16);
			}
			cemu_assert_debug(req->uuidName != -1);
			if (req->uuidName == -1)
				cemuLog_logDebug(LogType::Force, "IOSU_ACT: uuid mode {} not supported", req->uuidName);
			else if (req->uuidName != -2) // -2 returns the plain account uuid
				DeriveNamedUuid(uuid, (uint32)req->uuidName);
			req->setACTReturnCode(0);
			break;
		}
		case IOSU_ARC_SIMPLEADDRESS:
			if (!RequireSlot(req, slot))
				return;
			req->resultU32.u32 = slot->simpleAddressId;
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_PRINCIPALID:
			if (!RequireSlot(req, slot))
				return;
			req->resultU32.u32 = slot->principalId;
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_TRANSFERABLEID:
			if (!RequireSlot(req, slot))
				return;
			req->resultU64.u64 = slot->transferableIdBase;
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_PERSISTENTID:
			if (req->accountSlot == 0)
			{
				// slot 0 is not a real slot, but some titles (F1 Race Stars) ask for it anyway
				req->resultU32.u32 = 0;
				req->setACTReturnCode(0);
				break;
			}
			if (!RequireSlot(req, slot))
				return;
			req->resultU32.u32 = slot->persistentId;
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_COUNTRY:
			if (!RequireSlot(req, slot))
				return;
			strcpy(req->resultString.strBuffer, slot->country);
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_TIMEZONEID:
			if (!RequireSlot(req, slot))
				return;
			strcpy(req->resultString.strBuffer, slot->timeZoneId);
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_ISNETWORKACCOUNT:
			if (!RequireSlot(req, slot))
				return;
			req->resultU32.u32 = slot->networkAccount ? 1 : 0;
			req->setACTReturnCode(0);
			break;
		case IOSU_ARC_ACQUIRENEXTOKEN:
			req->setACTReturnCode(RequestNexToken(req->accountSlot, req->titleId, req->titleVersion, req->serverId, req->resultBinary.binBuffer, sizeof(NAPI::ACTNexToken)));
			break;
		case IOSU_ARC_ACQUIREINDEPENDENTTOKEN:
			req->setACTReturnCode(RequestIndependentToken(req->accountSlot, req->titleId, req->titleVersion, req->clientId, req->resultBinary.binBuffer, sizeof(req->resultBinary.binBuffer)));
			break;
		case IOSU_ARC_ACQUIREPIDBYNNID:
			req->setACTReturnCode(LookupPrincipalId(req->clientId, &req->resultU32.u32));
			break;
		case IOSU_ARC_MIIDATA:
			if (!RequireSlot(req, slot))
				return;
			memcpy(req->resultBinary.binBuffer, &slot->mii, sizeof(FFLData_t));
			req->setACTReturnCode(0);
			break;
		default:
			assert_dbg();
			break;
		}
	}

	void LegacyRequestLoop()
	{
		SetThreadName("iosuAct_thread");
		for (;;)
		{
			ioQueueEntry_t* entry = iosuIoctl_getNextWithWait(IOS_DEVICE_ACT);
			if (entry->request == 0)
				HandleLegacyCommand(entry);
			else if (entry->request == IOSU_ACT_REQUEST_CEMU)
				HandlePrivateRequest((iosuActCemuRequest_t*)entry->bufferVectors[0].buffer.GetPtr());
			else
				assert_dbg();
			iosuIoctl_completeRequest(entry, 0);
		}
	}
}

bool iosuAct_isAccountDataLoaded()
{
	return g_slotsLoaded;
}

uint32 iosuAct_getAccountIdOfCurrentAccount()
{
	cemu_assert_debug(g_slots[0].inUse);
	return g_slots[0].persistentId;
}

void iosuAct_init_depr()
{
	if (g_legacyThreadStarted)
		return;
	std::thread(LegacyRequestLoop).detach();
	g_legacyThreadStarted = true;
}

bool iosuAct_isInitialized()
{
	return g_legacyThreadStarted;
}

namespace iosu
{
	namespace act
	{
		uint8 getCurrentAccountSlot()
		{
			return 1;
		}

		bool getPrincipalId(uint8 slot, uint32* principalId)
		{
			SlotData* s = FindSlot(slot);
			*principalId = s ? s->principalId : 0;
			return s != nullptr;
		}

		bool getAccountId(uint8 slot, char* accountId)
		{
			SlotData* s = FindSlot(slot);
			if (!s)
			{
				accountId[0] = '\0';
				return false;
			}
			strcpy(accountId, s->accountId);
			return true;
		}

		std::string getAccountId2(uint8 slot)
		{
			SlotData* s = FindSlot(slot);
			return s ? std::string(s->accountId) : std::string();
		}

		bool getMii(uint8 slot, FFLData_t* fflData)
		{
			SlotData* s = FindSlot(slot);
			if (!s)
				return false;
			memcpy(fflData, &s->mii, sizeof(FFLData_t));
			return true;
		}

		bool getScreenname(uint8 slot, uint16 screenname[ACT_NICKNAME_LENGTH])
		{
			SlotData* s = FindSlot(slot);
			if (!s)
			{
				screenname[0] = 0;
				return false;
			}
			for (sint32 i = 0; i < ACT_NICKNAME_LENGTH; i++)
				screenname[i] = (uint16)s->nickname[i];
			return true;
		}

		bool getCountryIndex(uint8 slot, uint32* countryIndex)
		{
			SlotData* s = FindSlot(slot);
			*countryIndex = s ? s->countryIndex : 0;
			return s != nullptr;
		}

		bool GetPersistentId(uint8 slot, uint32* persistentId)
		{
			SlotData* s = FindSlot(slot);
			*persistentId = s ? s->persistentId : 0;
			return s != nullptr;
		}

		namespace
		{
			// IPC endpoint for the real nn_act.rpl; no commands are implemented yet
			class ActIpcService : public iosu::nn::IPCService
			{
			public:
				ActIpcService() : iosu::nn::IPCService("/dev/act") {}

				nnResult ServiceCall(IPCServiceCall& call) override
				{
					cemuLog_log(LogType::Force, "IOSU_ACT: unsupported call on /dev/act");
					cemu_assert_unimplemented();
					return BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_ACT, 0);
				}
			};

			ActIpcService g_actService;
		}

		void Initialize()
		{
			g_actService.Start();
		}

		void Stop()
		{
			g_actService.Stop();
		}
	}
}

// CRC-16/CCITT (polynomial 0x1021, no reflection) in the bit order used by Mii data;
// a valid Mii block checks to zero
uint16 FFLCalculateCRC16(uint8* input, sint32 length)
{
	uint16 crc = 0;
	for (sint32 i = 0; i < length; i++)
	{
		for (sint32 bit = 0; bit < 8; bit++)
			crc = (uint16)((crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1));
		crc ^= input[i];
	}
	return crc;
}
