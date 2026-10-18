// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_nfp is the amiibo library. A game initialises it, registers events for
// "amiibo arrived"/"amiibo left", starts detection, then mounts the amiibo to
// read its common data (character, series, ...), owner registration and the
// game's own application area (0xD8 bytes, tagged with the game's area id).
//
// The emulated reader holds one amiibo dump touched from the GUI. It is
// decrypted on touch, edited in decrypted form, and re-encrypted and written
// back to its file on Flush and when an application area is created or
// deleted. The figure leaves the reader 1.5 seconds after it was touched.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/CafeSystem.h"
#include "Common/FileStream.h"
#include "nn_nfp.h"
#include "AmiiboCrypto.h"

#include <random>

namespace nn::nfp
{
	namespace
	{
		// library states returned by GetNfpState
		enum : uint32
		{
			kStateNone = 0,
			kStateInit = 1,
			kStateSearch = 2,
			kStateActive = 3,
			kStateDeactive = 4,
			kStateMount = 5,
			kStateUnexpected = 6,
			kStateMountRom = 7,
		};

		// GUI touch errors (same values as nfc's NFC_TOUCH_TAG_ERROR_*)
		constexpr uint32 kTouchOk = 0;
		constexpr uint32 kTouchNoAccess = 1;
		constexpr uint32 kTouchBadFile = 2;

		constexpr uint32 kPresenceMs = 1500;

		// settings flags (plain offset 0x2C)
		constexpr uint8 kFlagRegistered = 0x10;
		constexpr uint8 kFlagHasAppArea = 0x20;

		// plain-data offsets (see AmiiboCrypto.h)
		constexpr size_t kOffWriteCounter = 0x029;
		constexpr size_t kOffFlags = 0x02C;
		constexpr size_t kOffNickname = 0x038; // 10 UTF-16BE characters
		constexpr size_t kOffMii = 0x04C;
		constexpr size_t kOffAppWriteCounter = 0x0B4;
		constexpr size_t kOffAppAreaId = 0x0B6;
		constexpr size_t kOffAppArea = 0x0DC;
		constexpr size_t kNicknameSize = 0x14;
		constexpr size_t kMiiSize = 0x60;
		constexpr size_t kAppAreaSize = 0xD8;

		// raw-dump offsets
		constexpr size_t kRawWriteCounter = 0x011;
		constexpr size_t kRawModelInfo = 0x054;

		inline nnResult Ok()
		{
			return BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0);
		}

		inline nnResult Fail(uint32 code = 0)
		{
			return BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, code);
		}

		struct State
		{
			bool initialized = false;
			bool detecting = false;
			bool mounted = false;
			bool readOnly = false;
			bool appAreaOpen = false;
			MEMPTR<coreinit::OSEvent> activateEvent;
			MEMPTR<coreinit::OSEvent> deactivateEvent;

			// the amiibo on the reader
			bool present = false;
			fs::path path;
			uint32 touchTick = 0;
			amiibo::RawTag raw{};     // as loaded from the file
			amiibo::PlainTag plain{}; // decrypted, edited by the game
			uint8 uid[7]{};
		};

		State g_nfp;
		std::recursive_mutex g_nfpMutex;
		std::mt19937 g_fillRng(std::random_device{}());

		uint8 Flags() { return g_nfp.plain[kOffFlags]; }
		void SetFlags(uint8 f) { g_nfp.plain[kOffFlags] = f; }

		uint16 GetBE16(const uint8* p) { return (uint16)((p[0] << 8) | p[1]); }
		void PutBE16(uint8* p, uint16 v) { p[0] = (uint8)(v >> 8); p[1] = (uint8)v; }
		uint32 GetBE32(const uint8* p) { return ((uint32)GetBE16(p) << 16) | GetBE16(p + 2); }
		void PutBE32(uint8* p, uint32 v) { PutBE16(p, (uint16)(v >> 16)); PutBE16(p + 2, (uint16)v); }

		uint8* AppArea() { return g_nfp.plain.data() + kOffAppArea; }

		void BumpAppWriteCounter()
		{
			uint8* p = g_nfp.plain.data() + kOffAppWriteCounter;
			PutBE16(p, GetBE16(p) + 1);
		}

		// fill the unused rest of the application area like the console does
		void FillAppAreaFrom(size_t used)
		{
			std::uniform_int_distribution<int> byte(0, 255);
			for (size_t i = used; i < kAppAreaSize; i++)
				AppArea()[i] = (uint8)byte(g_fillRng);
		}

		bool SaveToFile()
		{
			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.present)
				return false;
			uint8* wc = g_nfp.plain.data() + kOffWriteCounter;
			PutBE16(wc, GetBE16(wc) + 1);

			std::unique_ptr<FileStream> file(FileStream::openFile2(g_nfp.path, true));
			if (!file)
				return false;
			amiibo::RawTag out;
			amiibo::Encrypt(g_nfp.plain, g_nfp.raw, out);
			file->writeData(out.data(), (sint32)out.size());
			return true;
		}

		void Signal(MEMPTR<coreinit::OSEvent> ev)
		{
			if (ev)
				coreinit::OSSignalEvent(ev.GetPtr());
		}

		// ---- structures shared with the game --------------------------------

		struct TagInfo
		{
			/* +0x00 */ uint8 uidLength;
			/* +0x01 */ uint8 uid[10];
			/* +0x0B */ uint8 reserved[0x49];
		};
		static_assert(sizeof(TagInfo) == 0x54);

		struct Date
		{
			uint16be year;
			uint8 month;
			uint8 day;
		};

		struct CommonInfo
		{
			/* +0x00 */ Date lastWriteDate;
			/* +0x04 */ uint8 writeCount[2];
			/* +0x06 */ uint8 characterId[3];
			/* +0x09 */ uint8 seriesId;
			/* +0x0A */ uint16be modelNumber;
			/* +0x0C */ uint8 figureType;
			/* +0x0D */ uint8 version;
			/* +0x0E */ uint16be applicationAreaSize;
			/* +0x10 */ uint8 reserved[0x30];
		};
		static_assert(sizeof(CommonInfo) == 0x40);
		static_assert(offsetof(CommonInfo, applicationAreaSize) == 0xE);

		struct RomInfo // also used as ReadOnlyInfo
		{
			/* +0x00 */ uint8 characterId[3];
			/* +0x03 */ uint8 seriesId;
			/* +0x04 */ uint16be modelNumber;
			/* +0x06 */ uint8 figureType;
			/* +0x07 */ uint8 reserved[0x2F];
		};
		static_assert(sizeof(RomInfo) == 0x36);

		struct RegisterInfo
		{
			/* +0x00 */ uint8 mii[kMiiSize];
			/* +0x60 */ uint8 nickname[kNicknameSize];
			/* +0x74 */ uint8 fontRegion[2];
			/* +0x76 */ uint8 countryCode;
			/* +0x77 */ uint8 pad;
			/* +0x78 */ Date registerDate;
			/* +0x7C */ uint8 reserved[0x2C];
		};
		static_assert(sizeof(RegisterInfo) == 0xA8);

		struct RegisterInfoSet
		{
			/* +0x00 */ uint8 mii[kMiiSize];
			/* +0x60 */ uint8 nickname[kNicknameSize];
		};
		static_assert(sizeof(RegisterInfoSet) == 0x74);

		struct ApplicationAreaCreateInfo
		{
			uint32be appAreaId;
			MEMPTR<uint8> initialData;
			uint32be initialSize;
		};

		struct AmiiboSettingsArgs
		{
			/* +0x000 */ uint8 reserved0[0x10];
			/* +0x010 */ uint32be mode;
			/* +0x014 */ TagInfo tagInfo;
			/* +0x068 */ uint8 isRegistered;
			/* +0x069 */ uint8 pad[3];
			/* +0x06C */ RegisterInfo registerInfo;
			/* +0x114 */ CommonInfo commonInfo;
			/* +0x154 */ uint8 reserved1[0x20];
		};
		static_assert(sizeof(AmiiboSettingsArgs) == 0x174);
		static_assert(offsetof(AmiiboSettingsArgs, commonInfo) == 0x114);

		struct TagInfoCallbackParam
		{
			uint8 uidLength;
			uint8 uid[10];
			uint8 reserved[3];
		};

		void FillRomInfo(RomInfo* info)
		{
			const uint8* model = g_nfp.raw.data() + kRawModelInfo;
			memset(info, 0, sizeof(RomInfo));
			info->characterId[0] = model[0];
			info->characterId[1] = model[1];
			info->characterId[2] = model[2]; // variation
			info->figureType = model[3];
			info->modelNumber = GetBE16(model + 4);
			info->seriesId = model[6];
		}

		// ---- exports ---------------------------------------------------------

		nnResult Initialize()
		{
			std::lock_guard lock(g_nfpMutex);
			g_nfp.initialized = true;
			g_nfp.detecting = false;
			g_nfp.present = false;
			g_nfp.appAreaOpen = false;
			g_nfp.activateEvent = nullptr;
			g_nfp.deactivateEvent = nullptr;
			return 0;
		}

		nnResult SetActivateEvent(coreinit::OSEvent* ev)
		{
			coreinit::OSInitEvent(ev, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			std::lock_guard lock(g_nfpMutex);
			g_nfp.activateEvent = ev;
			return 0;
		}

		nnResult SetDeactivateEvent(coreinit::OSEvent* ev)
		{
			coreinit::OSInitEvent(ev, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			std::lock_guard lock(g_nfpMutex);
			g_nfp.deactivateEvent = ev;
			return 0;
		}

		nnResult StartDetection()
		{
			std::lock_guard lock(g_nfpMutex);
			g_nfp.detecting = true;
			return Ok();
		}

		nnResult StopDetection()
		{
			std::lock_guard lock(g_nfpMutex);
			g_nfp.detecting = false;
			return Ok();
		}

		nnResult GetTagInfo(TagInfo* info)
		{
			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.present)
				return Fail();
			memset(info, 0, sizeof(TagInfo));
			info->uidLength = sizeof(g_nfp.uid);
			memcpy(info->uid, g_nfp.uid, sizeof(g_nfp.uid));
			return Ok();
		}

		nnResult MountAs(bool readOnly)
		{
			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.present)
				return Fail();
			g_nfp.mounted = true;
			g_nfp.readOnly = readOnly;
			g_nfp.detecting = false;
			return Ok();
		}

		nnResult Mount() { return MountAs(false); }
		nnResult MountRom() { return MountAs(true); }

		nnResult Unmount()
		{
			std::lock_guard lock(g_nfpMutex);
			g_nfp.appAreaOpen = false;
			return Ok();
		}

		nnResult GetNfpRomInfo(RomInfo* info)
		{
			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.present)
				return Fail();
			FillRomInfo(info);
			return Ok();
		}

		nnResult GetNfpReadOnlyInfo(RomInfo* info)
		{
			return GetNfpRomInfo(info);
		}

		nnResult GetNfpCommonInfo(CommonInfo* info)
		{
			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.present)
				return Fail();
			const uint8* model = g_nfp.raw.data() + kRawModelInfo;
			memset(info, 0, sizeof(CommonInfo));
			info->writeCount[0] = g_nfp.raw[kRawWriteCounter];
			info->writeCount[1] = g_nfp.raw[kRawWriteCounter + 1];
			info->characterId[0] = model[0];
			info->characterId[1] = model[1];
			info->characterId[2] = model[2];
			info->figureType = model[3];
			info->modelNumber = GetBE16(model + 4);
			info->seriesId = model[6];
			info->applicationAreaSize = (uint16)kAppAreaSize;
			return Ok();
		}

		nnResult GetNfpRegisterInfo(RegisterInfo* info)
		{
			if (!info)
				return 0xC1B03780; // invalid argument
			std::lock_guard lock(g_nfpMutex);
			memset(info, 0, sizeof(RegisterInfo));
			if (Flags() & kFlagRegistered)
			{
				memcpy(info->mii, g_nfp.plain.data() + kOffMii, kMiiSize);
				memcpy(info->nickname, g_nfp.plain.data() + kOffNickname, kNicknameSize);
			}
			return Ok();
		}

		nnResult InitializeRegisterInfoSet(RegisterInfoSet* set)
		{
			memset(set, 0, sizeof(RegisterInfoSet));
			return Ok();
		}

		nnResult SetNfpRegisterInfo(const RegisterInfoSet* set)
		{
			std::lock_guard lock(g_nfpMutex);
			memcpy(g_nfp.plain.data() + kOffMii, set->mii, kMiiSize);
			memcpy(g_nfp.plain.data() + kOffNickname, set->nickname, kNicknameSize);
			SetFlags(Flags() | kFlagRegistered);
			return Ok();
		}

		bool IsExistApplicationArea()
		{
			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.present || !g_nfp.mounted)
				return false;
			return (Flags() & kFlagHasAppArea) != 0;
		}

		nnResult OpenApplicationArea(uint32 appAreaId)
		{
			std::lock_guard lock(g_nfpMutex);
			if (!(Flags() & kFlagHasAppArea))
				return Fail(NN_RESULT_NFP_CODE_NOAPPAREA);
			if (GetBE32(g_nfp.plain.data() + kOffAppAreaId) != appAreaId)
				return Fail(NN_RESULT_NFP_CODE_APPAREAIDMISMATCH);
			g_nfp.appAreaOpen = true; // opening twice is fine
			return Ok();
		}

		nnResult ReadApplicationArea(void* buffer, uint32 size)
		{
			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.appAreaOpen)
				return Fail();
			memcpy(buffer, AppArea(), std::min<size_t>(size, kAppAreaSize));
			return Ok();
		}

		nnResult WriteApplicationArea(const void* data, uint32 size, const void* tagId)
		{
			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.appAreaOpen || g_nfp.readOnly)
				return Fail();
			const size_t n = std::min<size_t>(size, kAppAreaSize);
			memcpy(AppArea(), data, n);
			FillAppAreaFrom(n);
			BumpAppWriteCounter();
			return Ok(); // written to the figure on Flush
		}

		nnResult CreateApplicationArea(const ApplicationAreaCreateInfo* info)
		{
			std::lock_guard lock(g_nfpMutex);
			if (g_nfp.appAreaOpen || (Flags() & kFlagHasAppArea) || g_nfp.readOnly)
				return Fail();
			const uint32 size = info->initialSize;
			if (size > kAppAreaSize)
				return Fail();
			PutBE32(g_nfp.plain.data() + kOffAppAreaId, info->appAreaId);
			SetFlags(Flags() | kFlagHasAppArea);
			BumpAppWriteCounter();
			g_nfp.appAreaOpen = false;
			if (size)
				memcpy(AppArea(), info->initialData.GetPtr(), size);
			FillAppAreaFrom(size);
			// creating an area writes the figure right away (Smash Bros. relies on it)
			if (!SaveToFile())
			{
				cemuLog_log(LogType::Force, "nn_nfp: could not write the amiibo file after creating the application area");
				return Fail();
			}
			return Ok();
		}

		nnResult DeleteApplicationArea()
		{
			std::lock_guard lock(g_nfpMutex);
			if (g_nfp.readOnly || !(Flags() & kFlagHasAppArea))
				return Fail();
			PutBE32(g_nfp.plain.data() + kOffAppAreaId, 0);
			SetFlags(Flags() & ~kFlagHasAppArea);
			BumpAppWriteCounter();
			if (!SaveToFile())
			{
				cemuLog_log(LogType::Force, "nn_nfp: could not write the amiibo file after deleting the application area");
				return Fail();
			}
			return Ok();
		}

		nnResult Flush()
		{
			std::lock_guard lock(g_nfpMutex);
			if (g_nfp.readOnly)
			{
				cemuLog_log(LogType::Force, "nn_nfp: Flush on an amiibo mounted read-only");
				return Fail();
			}
			if (!SaveToFile())
			{
				cemuLog_log(LogType::Force, "nn_nfp: could not write the amiibo file");
				return Fail();
			}
			return Ok();
		}

		uint32 GetNfpState()
		{
			// Mario Party 10 polls this in a tight loop; give the CPU back
			const uint64 titleId = CafeSystem::GetForegroundTitleId();
			if (titleId == 0x0005000010162d00 || titleId == 0x0005000010162e00)
				coreinit::OSSleepTicks(ESPRESSO_TIMER_CLOCK / 100);

			std::lock_guard lock(g_nfpMutex);
			if (!g_nfp.initialized)
				return kStateNone;
			if (g_nfp.mounted && g_nfp.present)
				return g_nfp.readOnly ? kStateMountRom : kStateMount;
			if (g_nfp.detecting)
				return g_nfp.present ? kStateActive : kStateSearch;
			return kStateInit;
		}

		nnResult GetAmiiboSettingsArgs(AmiiboSettingsArgs* args)
		{
			memset(args, 0, sizeof(AmiiboSettingsArgs));
			args->mode = 0x64; // started normally, not from amiibo settings
			return Ok();
		}

		// error code shown to the user for an nn_nfp result (168-xxxx)
		uint32 GetErrorCode(const uint32be* result)
		{
			const uint32 r = result ? (uint32)*result : 0;
			const uint32 level = (r >> 27) & 3;
			const uint32 moduleMask = level == 3 ? 0x07F00000 : 0x1FF00000;
			if ((r & 0x80000000) && (r & moduleMask) == ((uint32)NN_RESULT_MODULE_NN_NFP << 20))
			{
				const uint32 descMask = level == 3 ? 0x3FF : 0xFFFFF;
				return 1680000 + ((r & descMask) >> 7);
			}
			return 1680000;
		}
	}

	uint32 NFCGetTagInfo(uint32 index, uint32 timeout, MPTR functionPtr, void* userParam)
	{
		cemu_assert(index == 0);
		std::lock_guard lock(g_nfpMutex);
		StackAllocator<TagInfoCallbackParam> param;
		memset(param.GetPointer(), 0, sizeof(TagInfoCallbackParam));
		param->uidLength = sizeof(g_nfp.uid);
		memcpy(param->uid, g_nfp.uid, sizeof(g_nfp.uid));
		PPCCoreCallback(functionPtr, index, (uint32)0, param.GetPointer(), userParam);
		return 0;
	}

	class : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "nn_nfp";
		}

		void RPLMapped() override
		{
			cafeExportRegisterFunc(Initialize, "nn_nfp", "Initialize__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(SetActivateEvent, "nn_nfp", "SetActivateEvent__Q2_2nn3nfpFP7OSEvent", LogType::NN_NFP);
			cafeExportRegisterFunc(SetDeactivateEvent, "nn_nfp", "SetDeactivateEvent__Q2_2nn3nfpFP7OSEvent", LogType::NN_NFP);
			cafeExportRegisterFunc(StartDetection, "nn_nfp", "StartDetection__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(StopDetection, "nn_nfp", "StopDetection__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(GetTagInfo, "nn_nfp", "GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo", LogType::NN_NFP);
			cafeExportRegisterFunc(Mount, "nn_nfp", "Mount__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(MountRom, "nn_nfp", "MountRom__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(Unmount, "nn_nfp", "Unmount__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(GetNfpCommonInfo, "nn_nfp", "GetNfpCommonInfo__Q2_2nn3nfpFPQ3_2nn3nfp10CommonInfo", LogType::NN_NFP);
			cafeExportRegisterFunc(GetNfpRegisterInfo, "nn_nfp", "GetNfpRegisterInfo__Q2_2nn3nfpFPQ3_2nn3nfp12RegisterInfo", LogType::NN_NFP);
			cafeExportRegisterFunc(GetNfpRomInfo, "nn_nfp", "GetNfpRomInfo__Q2_2nn3nfpFPQ3_2nn3nfp7RomInfo", LogType::NN_NFP);
			cafeExportRegisterFunc(GetNfpReadOnlyInfo, "nn_nfp", "GetNfpReadOnlyInfo__Q2_2nn3nfpFPQ3_2nn3nfp12ReadOnlyInfo", LogType::NN_NFP);
			cafeExportRegisterFunc(InitializeRegisterInfoSet, "nn_nfp", "InitializeRegisterInfoSet__Q2_2nn3nfpFPQ3_2nn3nfp15RegisterInfoSet", LogType::NN_NFP);
			cafeExportRegisterFunc(SetNfpRegisterInfo, "nn_nfp", "SetNfpRegisterInfo__Q2_2nn3nfpFRCQ3_2nn3nfp15RegisterInfoSet", LogType::NN_NFP);
			cafeExportRegisterFunc(IsExistApplicationArea, "nn_nfp", "IsExistApplicationArea__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(OpenApplicationArea, "nn_nfp", "OpenApplicationArea__Q2_2nn3nfpFUi", LogType::NN_NFP);
			cafeExportRegisterFunc(CreateApplicationArea, "nn_nfp", "CreateApplicationArea__Q2_2nn3nfpFRCQ3_2nn3nfp25ApplicationAreaCreateInfo", LogType::NN_NFP);
			cafeExportRegisterFunc(DeleteApplicationArea, "nn_nfp", "DeleteApplicationArea__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(ReadApplicationArea, "nn_nfp", "ReadApplicationArea__Q2_2nn3nfpFPvUi", LogType::NN_NFP);
			cafeExportRegisterFunc(WriteApplicationArea, "nn_nfp", "WriteApplicationArea__Q2_2nn3nfpFPCvUiRCQ3_2nn3nfp5TagId", LogType::NN_NFP);
			cafeExportRegisterFunc(Flush, "nn_nfp", "Flush__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(GetNfpState, "nn_nfp", "GetNfpState__Q2_2nn3nfpFv", LogType::NN_NFP);
			cafeExportRegisterFunc(GetAmiiboSettingsArgs, "nn_nfp", "GetAmiiboSettingsArgs__Q2_2nn3nfpFPQ3_2nn3nfp18AmiiboSettingsArgs", LogType::NN_NFP);
			cafeExportRegisterFunc(GetErrorCode, "nn_nfp", "GetErrorCode__Q2_2nn3nfpFRCQ2_2nn6Result", LogType::NN_NFP);
		}
	} s_nfpModule;

	COSModule* GetModule()
	{
		return &s_nfpModule;
	}
}

bool nnNfp_isInitialized()
{
	return nn::nfp::g_nfp.initialized;
}

bool nnNfp_touchNfcTagFromFile(const fs::path& filePath, uint32* nfcError)
{
	using namespace nn::nfp;
	auto file = FileStream::LoadIntoMemory(filePath);
	if (!file)
	{
		*nfcError = kTouchNoAccess;
		return false;
	}
	if (file->size() < amiibo::kRawSize)
	{
		*nfcError = kTouchBadFile;
		return false;
	}
	amiibo::RawTag raw;
	memcpy(raw.data(), file->data(), raw.size());

	auto matches = [&raw](size_t offset, std::initializer_list<uint8> expected) {
		size_t i = offset;
		for (uint8 b : expected)
		{
			if (raw[i++] != b)
				return false;
		}
		return true;
	};
	// dynamic lock bytes: older emulator versions wrote these wrong, so fix
	// them up instead of rejecting the file
	if (!matches(0x208, {0x01, 0x00, 0x0F, 0xBD}))
	{
		const uint8 fixed[4] = {0x01, 0x00, 0x0F, 0xBD};
		memcpy(raw.data() + 0x208, fixed, 4);
	}
	// NTAG215 configuration pages, static lock bytes, capability container
	if (!matches(0x20C, {0x00, 0x00, 0x00, 0x04}) || !matches(0x210, {0x5F, 0x00, 0x00, 0x00}) ||
		!matches(0x00A, {0x0F, 0xE0}) || !matches(0x00C, {0xF1, 0x10, 0xFF, 0xEE}))
	{
		*nfcError = kTouchBadFile;
		return false;
	}

	// 7-byte UID: bytes 0-2 and 4-7 (byte 3 and 8 are check bytes)
	const uint8 uid[7] = {raw[0], raw[1], raw[2], raw[4], raw[5], raw[6], raw[7]};
	const uint8 bcc0 = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
	const uint8 bcc1 = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
	if (raw[3] != bcc0 || raw[8] != bcc1)
		cemuLog_log(LogType::Force, "nn_nfp: amiibo UID check bytes do not match");

	std::lock_guard lock(g_nfpMutex);
	g_nfp.mounted = false;
	g_nfp.raw = raw;
	memcpy(g_nfp.uid, uid, sizeof(uid));
	amiibo::Decrypt(g_nfp.raw, g_nfp.plain);
	g_nfp.path = filePath;
	g_nfp.present = true;
	g_nfp.touchTick = GetTickCount();
	Signal(g_nfp.activateEvent);
	*nfcError = kTouchOk;
	return true;
}

void nnNfp_update()
{
	using namespace nn::nfp;
	if (!g_nfp.present) // cheap check without the lock
		return;
	std::unique_lock lock(g_nfpMutex, std::try_to_lock);
	if (!lock.owns_lock() || !g_nfp.present)
		return;
	if (GetTickCount() - g_nfp.touchTick < kPresenceMs)
		return;
	g_nfp.mounted = false;
	g_nfp.present = false;
	Signal(g_nfp.deactivateEvent);
}
