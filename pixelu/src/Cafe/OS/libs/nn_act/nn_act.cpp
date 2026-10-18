// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_act is the account library: which user profile is active, its ids
// (persistent id, principal id, NNID, uuid, ...), its Mii and the tokens
// for online services. The accounts themselves are held by PixelU's
// /dev/act service (iosu_act.cpp); each query here is one request to it.
//
// Slot numbers start at 1; 0xFE means "the account that is signed in".
// PixelU always runs with exactly one signed-in account in slot 1.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_IOS.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/IOSU/legacy/iosu_act.h"
#include "Cafe/IOSU/legacy/iosu_ioctl.h"
#include "Cafe/CafeSystem.h"
#include "Common/CafeString.h"
#include "nn_act.h"

namespace
{
	constexpr uint8 kCurrentSlot = iosu::act::ACT_SLOT_CURRENT;
	constexpr uint8 kFirstSlot = 1;
	constexpr uint8 kSlotCount = 12;
	constexpr uint32 kUuidSize = 16;

	// client id the eShop service token is requested with
	constexpr const char* kEShopClientId = "71a6f5d6430ea0183e3917787d717c46";

	// One request to /dev/act. The request block must be in game memory.
	class ActCall
	{
	public:
		explicit ActCall(uint32 code, uint8 slot = kCurrentSlot)
		{
			memset(m_request.GetPointer(), 0, sizeof(iosuActCemuRequest_t));
			memset(m_vector.GetPointer(), 0, sizeof(ioBufferVector_t));
			m_request->requestCode = code;
			m_request->accountSlot = slot;
			m_vector->buffer = (uint8*)m_request.GetPointer();
		}

		iosuActCemuRequest_t* operator->() { return m_request.GetPointer(); }

		void SetClientId(const char* text)
		{
			const size_t room = sizeof(m_request->clientId);
			const size_t len = text ? strnlen(text, room - 1) : 0;
			memcpy(m_request->clientId, text ? text : "", len);
			m_request->clientId[len] = '\0';
		}

		// Returns the IOS error if the request couldn't be delivered, else the service's answer.
		uint32 Send()
		{
			const uint32 ios = (uint32)__depr__IOS_Ioctlv(IOS_DEVICE_ACT, IOSU_ACT_REQUEST_CEMU, 1, 1, m_vector.GetPointer());
			if (ios & 0x80000000)
				return ios;
			return m_request->returnCode;
		}

		uint32 U32() { return m_request->resultU32.u32; }
		const char* Text() { return m_request->resultString.strBuffer; }
		const uint8* Bytes() { return m_request->resultBinary.binBuffer; }

	private:
		StackAllocator<iosuActCemuRequest_t> m_request;
		StackAllocator<ioBufferVector_t> m_vector;
	};

	// ACTNexAuthenticationResult
	struct NexToken
	{
		/* +0x000 */ char token[0x201];
		/* +0x201 */ uint8 pad201[3];
		/* +0x204 */ char password[0x41];
		/* +0x245 */ uint8 pad245[3];
		/* +0x248 */ char host[0x10];
		/* +0x258 */ uint16be port;
		/* +0x25A */ uint16be pad25A;
	};
	static_assert(sizeof(NexToken) == 0x25C);

	uint32 g_initCount = 0;
	bool g_parentalCheckEnabled = false;

	uint32 GetMiiEx(void* mii, uint8 slot)
	{
		ActCall call(IOSU_ARC_MIIDATA, slot);
		const uint32 r = call.Send();
		memcpy(mii, call.Bytes(), MII_FFL_STORAGE_SIZE);
		return r;
	}

	uint32 GetAccountIdEx(char* accountId, uint8 slot)
	{
		ActCall call(IOSU_ARC_ACCOUNT_ID, slot);
		const uint32 r = call.Send();
		strcpy(accountId, call.Text());
		return r;
	}

	uint32 GetPrincipalIdEx(uint32be* principalId, uint8 slot)
	{
		ActCall call(IOSU_ARC_PRINCIPALID, slot);
		const uint32 r = call.Send();
		*principalId = call.U32();
		return r;
	}

	uint32 GetCountryEx(char* country, uint8 slot)
	{
		ActCall call(IOSU_ARC_COUNTRY, slot);
		const uint32 r = call.Send();
		strcpy(country, call.Text());
		return r;
	}

	uint32 IsNetworkAccountEx(uint8 slot)
	{
		ActCall call(IOSU_ARC_ISNETWORKACCOUNT, slot);
		call.Send();
		return call.U32() != 0 ? 1 : 0;
	}

	// Copies the account's Mii name (up to 10 characters) plus a terminator.
	void CopyMiiName(uint16be* out, uint8 slot)
	{
		StackAllocator<FFLData_t> mii;
		GetMiiEx(mii.GetPointer(), slot);
		uint32 len = 0;
		while (len < MII_FFL_NAME_LENGTH && (uint16)mii->miiName[len] != 0)
		{
			out[len] = (uint16)mii->miiName[len];
			len++;
		}
		out[len] = 0;
	}

	// ---- exports -------------------------------------------------------------

	uint32 InitializeExport()
	{
		nn::act::Initialize();
		return 0;
	}

	uint32 CreateConsoleAccount()
	{
		return 0;
	}

	uint32 GetNumOfAccounts()
	{
		return 1;
	}

	uint32 IsSlotOccupied(uint8 slot)
	{
		return nn::act::GetPersistentIdEx(slot) != 0 ? 1 : 0;
	}

	uint32 GetSlotNo()
	{
		return kFirstSlot;
	}

	// Slot of the account with this uuid, 0 if there is none.
	uint32 GetSlotNoEx(uint8* uuid)
	{
		for (uint8 slot = kFirstSlot; slot < kSlotCount; slot++)
		{
			uint8 slotUuid[kUuidSize]{};
			nn::act::GetUuidEx(slotUuid, slot);
			if (memcmp(uuid, slotUuid, kUuidSize) == 0)
				return slot;
		}
		return 0;
	}

	uint32 GetDefaultAccount()
	{
		return kFirstSlot;
	}

	uint32 IsNetworkAccount()
	{
		return IsNetworkAccountEx(kCurrentSlot);
	}

	uint32 GetAccountId(char* accountId)
	{
		GetAccountIdEx(accountId, kCurrentSlot);
		return 0;
	}

	uint32 GetAccountIdExExport(char* accountId, uint8 slot)
	{
		GetAccountIdEx(accountId, slot);
		return 0;
	}

	uint32 GetSimpleAddressId()
	{
		StackAllocator<uint32be> id;
		*id.GetPointer() = 0;
		nn::act::GetSimpleAddressIdEx(id.GetPointer(), kCurrentSlot);
		return *id.GetPointer();
	}

	uint32 GetSimpleAddressIdExExport(uint32be* id, uint8 slot)
	{
		nn::act::GetSimpleAddressIdEx(id, slot);
		return 0;
	}

	uint32 GetPrincipalId()
	{
		uint32be pid = 0;
		GetPrincipalIdEx(&pid, kCurrentSlot);
		return pid;
	}

	uint32 GetPrincipalIdExExport(uint32be* pid, uint8 slot)
	{
		GetPrincipalIdEx(pid, slot);
		return 0;
	}

	uint64 GetTransferableId(uint32 unique)
	{
		uint64 stored = 0; // big-endian bytes, as GetTransferableIdEx writes them
		if (NN_RESULT_IS_FAILURE(nn::act::GetTransferableIdEx(&stored, unique, kCurrentSlot)))
			return 0;
		return _swapEndianU64(stored);
	}

	uint32 GetTransferableIdExExport(uint64* id, uint32 unique, uint8 slot)
	{
		nn::act::GetTransferableIdEx(id, unique, slot);
		return 0;
	}

	uint32 GetPersistentId()
	{
		return nn::act::GetPersistentIdEx(kCurrentSlot);
	}

	uint32 GetPersistentIdExExport(uint8 slot)
	{
		return nn::act::GetPersistentIdEx(slot);
	}

	uint32 GetCountry(char* country)
	{
		return GetCountryEx(country, kCurrentSlot);
	}

	// Accounts carry no time zone yet; report London.
	nnResult GetTimeZoneId(CafeString<65>* zone)
	{
		zone->assign("Europe/London");
		return 0;
	}

	uint32 EnableParentalControlCheck(uint8 enable)
	{
		g_parentalCheckEnabled = enable != 0;
		return 0;
	}

	uint32 IsParentalControlCheckEnabled()
	{
		return g_parentalCheckEnabled ? 1 : 0;
	}

	// Parental controls belong to the account itself.
	uint32 GetParentalControlSlotNoEx(uint8* slotOut, uint8 slot)
	{
		*slotOut = kFirstSlot;
		return 0;
	}

	uint32 GetMii(void* mii)
	{
		return GetMiiEx(mii, kCurrentSlot);
	}

	uint32 GetMiiExExport(void* mii, uint8 slot)
	{
		return GetMiiEx(mii, slot);
	}

	uint32 GetMiiImageEx(uint32be* sizeOut, void* buffer, uint32 bufferSize, uint32 imageType, uint8 slot)
	{
		return 0; // Mii portraits are not rendered
	}

	uint32 GetMiiName(uint16be* name)
	{
		CopyMiiName(name, kCurrentSlot);
		return 0;
	}

	uint32 GetMiiNameEx(uint16be* name, uint8 slot)
	{
		CopyMiiName(name, slot);
		return 0;
	}

	uint32 UpdateMii(uint8 slot, FFLData_t* mii, uint16be* name)
	{
		cemuLog_log(LogType::Force, "nn_act: UpdateMii is not supported, the account's Mii stays as it is");
		return 0;
	}

	uint32 GetUuid(uint8* uuid)
	{
		nn::act::GetUuidEx(uuid, kCurrentSlot);
		return 0;
	}

	uint32 GetUuidExExport(uint8* uuid, uint8 slot)
	{
		nn::act::GetUuidEx(uuid, slot);
		return 0;
	}

	uint32 GetUuidExNamed(uint8* uuid, uint8 slot, sint32 name)
	{
		nn::act::GetUuidEx(uuid, slot, name);
		return 0;
	}

	uint32 AcquireEcServiceToken(independentServiceToken_t* token)
	{
		return nn::act::AcquireIndependentServiceToken(token, kEShopClientId, 0);
	}

	uint32 AcquireNexServiceToken(NexToken* token, uint32 serverId)
	{
		memset(token, 0, sizeof(NexToken));
		ActCall call(IOSU_ARC_ACQUIRENEXTOKEN);
		call->titleId = CafeSystem::GetForegroundTitleId();
		call->titleVersion = CafeSystem::GetForegroundTitleVersion();
		call->serverId = serverId;
		const uint32 r = call.Send();
		memcpy(token, call.Bytes(), sizeof(NexToken));
		return r;
	}

	uint32 AcquireIndependentServiceTokenNoCache(independentServiceToken_t* token, const char* clientId)
	{
		return nn::act::AcquireIndependentServiceToken(token, clientId, 0);
	}

	uint32 AcquireIndependentServiceTokenCached(independentServiceToken_t* token, const char* clientId, uint32 cacheSeconds)
	{
		return nn::act::AcquireIndependentServiceToken(token, clientId, cacheSeconds);
	}

	uint32 AcquirePrincipalIdByAccountId(uint32be* pidOut, const char* accountId, uint32 option)
	{
		ActCall call(IOSU_ARC_ACQUIREPIDBYNNID, 0);
		call.SetClientId(accountId);
		const uint32 r = call.Send();
		*pidOut = call.U32();
		return r;
	}

	NN_ERROR_CODE GetErrorCode(betype<nnResult>* result)
	{
		return nn::NNResultToErrorCode(*result, NN_RESULT_MODULE_NN_ACT);
	}

	// Splatoon tries to register a friend account when this is false.
	uint32 HasNfsAccount()
	{
		return 1;
	}

	uint32 GetHostServerSettings(char* first, char* second, uint8 slot)
	{
		second[0] = '\0';
		return 0;
	}
}

namespace nn::act
{
	uint32 Initialize()
	{
		if (g_initCount++ == 0)
		{
			ActCall call(IOSU_ARC_INIT);
			call.Send();
		}
		return 0;
	}

	uint32 GetPersistentIdEx(uint8 slot)
	{
		ActCall call(IOSU_ARC_PERSISTENTID, slot);
		call.Send();
		return call.U32();
	}

	uint32 GetUuidEx(uint8* uuid, uint8 slot, sint32 name)
	{
		ActCall call(IOSU_ARC_UUID, slot);
		call->uuidName = name;
		const uint32 r = call.Send();
		memcpy(uuid, call.Bytes(), kUuidSize);
		return r;
	}

	uint32 GetSimpleAddressIdEx(uint32be* simpleAddressId, uint8 slot)
	{
		ActCall call(IOSU_ARC_SIMPLEADDRESS, slot);
		const uint32 r = call.Send();
		*simpleAddressId = call.U32();
		return r;
	}

	// Writes the id as big-endian bytes, the way it sits in game memory.
	uint32 GetTransferableIdEx(uint64* transferableId, uint32 unique, uint8 slot)
	{
		ActCall call(IOSU_ARC_TRANSFERABLEID, slot);
		call->unique = unique;
		const uint32 r = call.Send();
		*transferableId = _swapEndianU64(call->resultU64.u64);
		return r;
	}

	// Local time minus UTC, in microseconds.
	sint64 GetUtcOffset()
	{
		const sint64 localSeconds = (sint64)(ppcCyclesSince2000 / ESPRESSO_CORE_CLOCK);
		const sint64 utcSeconds = (sint64)(ppcCyclesSince2000_UTC / ESPRESSO_CORE_CLOCK);
		return (localSeconds - utcSeconds) * 1'000'000;
	}

	sint32 GetUtcOffsetEx(sint64be* offsetOut, uint8 slot)
	{
		if (!offsetOut)
			return (sint32)0xC0712C80;
		*offsetOut = GetUtcOffset();
		return 0;
	}

	uint32 AcquireIndependentServiceToken(independentServiceToken_t* token, const char* clientId, uint32 cacheDurationInSeconds)
	{
		memset(token, 0, sizeof(independentServiceToken_t));
		ActCall call(IOSU_ARC_ACQUIREINDEPENDENTTOKEN);
		call->titleId = CafeSystem::GetForegroundTitleId();
		call->titleVersion = CafeSystem::GetForegroundTitleVersion();
		call->expiresIn = cacheDurationInSeconds;
		call.SetClientId(clientId);
		const uint32 r = call.Send();
		memcpy(token, call.Bytes(), sizeof(independentServiceToken_t));
		return r;
	}

	namespace
	{
		class ActLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_act"; }

			void RPLMapped() override
			{
				constexpr auto lt = LogType::Placeholder;
				cafeExportRegisterFunc(InitializeExport, "nn_act", "Initialize__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(CreateConsoleAccount, "nn_act", "CreateConsoleAccount__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(GetNumOfAccounts, "nn_act", "GetNumOfAccounts__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(IsSlotOccupied, "nn_act", "IsSlotOccupied__Q2_2nn3actFUc", lt);
				cafeExportRegisterFunc(GetSlotNo, "nn_act", "GetSlotNo__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(GetSlotNoEx, "nn_act", "GetSlotNoEx__Q2_2nn3actFRC7ACTUuid", lt);
				cafeExportRegisterFunc(IsNetworkAccount, "nn_act", "IsNetworkAccount__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(IsNetworkAccountEx, "nn_act", "IsNetworkAccountEx__Q2_2nn3actFUc", lt);
				cafeExportRegisterFunc(GetAccountId, "nn_act", "GetAccountId__Q2_2nn3actFPc", lt);
				cafeExportRegisterFunc(GetAccountIdExExport, "nn_act", "GetAccountIdEx__Q2_2nn3actFPcUc", lt);
				cafeExportRegisterFunc(GetSimpleAddressId, "nn_act", "GetSimpleAddressId__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(GetSimpleAddressIdExExport, "nn_act", "GetSimpleAddressIdEx__Q2_2nn3actFPUiUc", lt);
				cafeExportRegisterFunc(GetPrincipalId, "nn_act", "GetPrincipalId__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(GetPrincipalIdExExport, "nn_act", "GetPrincipalIdEx__Q2_2nn3actFPUiUc", lt);
				cafeExportRegisterFunc(GetTransferableId, "nn_act", "GetTransferableId__Q2_2nn3actFUi", lt);
				cafeExportRegisterFunc(GetTransferableIdExExport, "nn_act", "GetTransferableIdEx__Q2_2nn3actFPULUiUc", lt);
				cafeExportRegisterFunc(GetPersistentId, "nn_act", "GetPersistentId__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(GetPersistentIdExExport, "nn_act", "GetPersistentIdEx__Q2_2nn3actFUc", lt);
				cafeExportRegisterFunc(GetCountry, "nn_act", "GetCountry__Q2_2nn3actFPc", lt);
				cafeExportRegisterFunc(GetTimeZoneId, "nn_act", "GetTimeZoneId__Q2_2nn3actFPc", lt);
				cafeExportRegisterFunc(EnableParentalControlCheck, "nn_act", "EnableParentalControlCheck__Q2_2nn3actFb", lt);
				cafeExportRegisterFunc(IsParentalControlCheckEnabled, "nn_act", "IsParentalControlCheckEnabled__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(GetParentalControlSlotNoEx, "nn_act", "GetParentalControlSlotNoEx__Q2_2nn3actFPUcUc", lt);
				cafeExportRegisterFunc(GetMii, "nn_act", "GetMii__Q2_2nn3actFP12FFLStoreData", lt);
				cafeExportRegisterFunc(GetMiiExExport, "nn_act", "GetMiiEx__Q2_2nn3actFP12FFLStoreDataUc", lt);
				cafeExportRegisterFunc(GetMiiImageEx, "nn_act", "GetMiiImageEx__Q2_2nn3actFPUiPvUi15ACTMiiImageTypeUc", lt);
				cafeExportRegisterFunc(GetMiiName, "nn_act", "GetMiiName__Q2_2nn3actFPw", lt);
				cafeExportRegisterFunc(GetMiiNameEx, "nn_act", "GetMiiNameEx__Q2_2nn3actFPwUc", lt);
				cafeExportRegisterFunc(UpdateMii, "nn_act", "UpdateMii__Q2_2nn3actFUcRC12FFLStoreDataPCwPCvUiT4T5T4T5T4T5T4T5T4T5T4T5T4T5T4T5", lt);
				cafeExportRegisterFunc(GetUuid, "nn_act", "GetUuid__Q2_2nn3actFP7ACTUuid", lt);
				cafeExportRegisterFunc(GetUuidExExport, "nn_act", "GetUuidEx__Q2_2nn3actFP7ACTUuidUc", lt);
				cafeExportRegisterFunc(GetUuidExNamed, "nn_act", "GetUuidEx__Q2_2nn3actFP7ACTUuidUcUi", lt);
				cafeExportRegisterFunc(GetDefaultAccount, "nn_act", "GetDefaultAccount__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(AcquireEcServiceToken, "nn_act", "AcquireEcServiceToken__Q2_2nn3actFPc", lt);
				cafeExportRegisterFunc(AcquireNexServiceToken, "nn_act", "AcquireNexServiceToken__Q2_2nn3actFP26ACTNexAuthenticationResultUi", lt);
				cafeExportRegisterFunc(AcquireIndependentServiceTokenNoCache, "nn_act", "AcquireIndependentServiceToken__Q2_2nn3actFPcPCc", lt);
				cafeExportRegisterFunc(AcquireIndependentServiceTokenCached, "nn_act", "AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4", lt);
				cafeExportRegisterFunc(AcquireIndependentServiceTokenCached, "nn_act", "AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUi", lt);
				cafeExportRegisterFunc(AcquirePrincipalIdByAccountId, "nn_act", "AcquirePrincipalIdByAccountId__Q2_2nn3actFPUiPA17_CcUi", lt);
				cafeExportRegisterFunc(GetErrorCode, "nn_act", "GetErrorCode__Q2_2nn3actFRCQ2_2nn6Result", lt);
				cafeExportRegisterFunc(HasNfsAccount, "nn_act", "HasNfsAccount__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(GetHostServerSettings, "nn_act", "GetHostServerSettings__Q2_2nn3actFPcT1Uc", lt);
				cafeExportRegisterFunc(GetUtcOffset, "nn_act", "GetUtcOffset__Q2_2nn3actFv", lt);
				cafeExportRegisterFunc(GetUtcOffsetEx, "nn_act", "GetUtcOffsetEx__Q2_2nn3actFPLUc", lt);
			}
		};

		ActLibrary g_actLibrary;
	}

	COSModule* GetModule()
	{
		return &g_actLibrary;
	}
}
