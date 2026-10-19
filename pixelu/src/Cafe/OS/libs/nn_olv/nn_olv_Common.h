// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cemu/napi/napi_helper.h"
#include "util/helpers/StringHelpers.h"
#include "pugixml.hpp"

#include <functional>

// nn_olv (Miiverse / "Olive") shared pieces: result codes, service state, the
// request helper and small string, TGA and community-code utilities used by
// every class file of the library.

namespace nn::olv
{
	// --- result codes -------------------------------------------------------------
	// nn::Result values of the olv module; the detail field is (code << 7)

	constexpr nnResult OlvResult(uint32 level, uint32 code)
	{
		return BUILD_NN_RESULT(level, NN_RESULT_MODULE_NN_OLV, code << 7);
	}
	constexpr nnResult OlvStatus(uint32 code) { return OlvResult(NN_RESULT_LEVEL_STATUS, code); }
	constexpr nnResult OlvUsage(uint32 code) { return OlvResult(NN_RESULT_LEVEL_LVL6, code); }
	constexpr nnResult OlvFatal(uint32 code) { return OlvResult(NN_RESULT_LEVEL_FATAL, code); }

	constexpr nnResult kOlvSuccess = OlvResult(0, 1);

	constexpr nnResult kOlvInvalidParameter = OlvUsage(201);
	constexpr nnResult kOlvInvalidData = OlvUsage(202);
	constexpr nnResult kOlvNotEnoughSize = OlvUsage(203);
	constexpr nnResult kOlvInvalidPointer = OlvUsage(204);
	constexpr nnResult kOlvNotInitialized = OlvUsage(205);
	constexpr nnResult kOlvAlreadyInitialized = OlvUsage(206);
	constexpr nnResult kOlvOfflineMode = OlvUsage(207);
	constexpr nnResult kOlvMissingData = OlvUsage(208);
	constexpr nnResult kOlvInvalidSize = OlvUsage(209);

	constexpr nnResult kOlvBadVersion = OlvStatus(2001);
	constexpr nnResult kOlvRequestFailed = OlvStatus(2003);
	constexpr nnResult kOlvBadXml = OlvStatus(2004);
	constexpr nnResult kOlvBadTextField = OlvStatus(2006);
	constexpr nnResult kOlvBadIntegerField = OlvStatus(2007);

	// --- host-side service state ------------------------------------------------

	// values sent to the Olive servers with every request
	struct ParamPackStorage
	{
		uint64 titleId;
		uint32 accessKey;
		uint32 platformId;
		uint8 regionId;
		uint8 languageId;
		uint8 countryId;
		uint8 areaId;
		uint8 networkRestriction;
		uint8 friendRestriction;
		uint32 ratingRestriction;
		uint8 ratingOrganization;
		uint64 transferableId;
		char tzName[72];
		sint64 utcOffset;
		char encodedParamPack[512]; // base64 of the backslash separated list
	};

	// what Initialize() learned from the discovery server
	struct DiscoveryResultStorage
	{
		sint32 hasError;
		char serviceToken[512];
		char userAgent[64];
		char apiEndpoint[256];
		char portalEndpoint[256];
	};

	struct OliveState
	{
		uint32 reportTypes = 0;
		bool initialized = false;
		bool online = false;
		bool useOfflineDB = false; // serve posts from the bundled archive instead of a server
	};

	extern ParamPackStorage g_ParamPack;
	extern DiscoveryResultStorage g_DiscoveryResults;
	extern OliveState g_olv;

	// --- requests ---------------------------------------------------------------

	// run fn on a host thread while the calling guest thread waits on an OSEvent
	nnResult RunBlockingOnHost(const std::function<nnResult()>& fn);

	// set up a request to an Olive endpoint (service token, param pack, user agent)
	void BeginOliveRequest(CurlRequestHelper& req, const std::string& url);

	// submit a prepared request and validate the <result> envelope of the reply
	nnResult FinishOliveRequest(CurlRequestHelper& req, const std::string& url, bool post, pugi::xml_document& reply);

	// <result><has_error>/<version> check shared by every Olive reply
	nnResult CheckResultEnvelope(const pugi::xml_document& doc);

	// append "key=value" to a URL, choosing '?' or '&'
	void AppendQuery(std::string& url, std::string_view query);

	// --- wide strings (big-endian UCS-2 in guest memory) ---------------------------

	size_t WideLength(const uint16be* str, size_t maxLength);
	// strncpy-like: copy up to count characters, stop at a terminator, pad the rest with zeros
	void WideCopyPadded(uint16be* dst, const uint16be* src, size_t count);
	// store src into a dst[capacity] field; fails if src does not fit with its terminator
	nnResult WideAssignChecked(uint16be* dst, size_t capacity, const uint16be* src);
	// copy at most capacity-1 characters and terminate; returns the copied length
	uint32 WideAssignTruncated(uint16be* dst, size_t capacity, const uint16be* src);
	// UTF-8 -> wide, at most capacity-1 characters plus terminator; returns the length
	uint32 Utf8ToWideTerminated(uint16be* dst, size_t capacity, std::string_view text);
	// UTF-8 -> wide, at most capacity characters, no terminator added; returns the length
	uint32 Utf8ToWideFixed(uint16be* dst, size_t capacity, std::string_view text);

	// --- TGA images (paintings and icons travel zlib-compressed) --------------------

	enum class TgaKind
	{
		Painting,      // 320x120 32bpp uncompressed true colour
		CommunityIcon, // 128x128 32bpp
		Icon100or200,  // 100x100 or 200x200 32bpp
	};

	bool IsValidTga(const uint8* data, uint32 size, TgaKind kind);
	// inflate and validate; returns the inflated size, -1 if inflating failed, -2 if it is not a valid TGA
	sint32 InflateTga(const uint8* in, uint32 inSize, uint8* out, uint32 outCapacity, TgaKind kind);
	// validate and deflate (out must be as large as in); returns the compressed size or -1
	sint32 DeflateTga(const uint8* in, uint32 inSize, uint8* out, uint32 outCapacity, TgaKind kind);

	// --- community codes (the 12 digit code shown to users) ------------------------

	bool CommunityIdFromCode(uint32* outId, const char* code);
	bool CommunityCodeFromId(char* outCode, uint32* outLength, uint32 communityId);

	// --- community records ----------------------------------------------------------
	// Downloaded/Uploaded community data and favourites all start with this block.

	namespace CommunityFlag
	{
		constexpr uint32 Title = 1 << 0;
		constexpr uint32 Description = 1 << 1;
		constexpr uint32 AppData = 1 << 2;
		constexpr uint32 Icon = 1 << 3;
		constexpr uint32 Mii = 1 << 4;
	}

	struct CommunityRecord
	{
		/* +0x00000 */ uint32be flags;
		/* +0x00004 */ uint32be communityId;
		/* +0x00008 */ uint32be ownerPid;
		/* +0x0000C */ uint16be title[128];
		/* +0x0010C */ uint32be titleLength;
		/* +0x00110 */ uint16be description[256];
		/* +0x00310 */ uint32be descriptionLength;
		/* +0x00314 */ uint8 appData[1024];
		/* +0x00714 */ uint32be appDataSize;
		/* +0x00718 */ uint8 icon[0x1002C]; // zlib-compressed TGA
		/* +0x10744 */ uint32be iconSize;
	};
	static_assert(sizeof(CommunityRecord) == 0x10748);
	static_assert(offsetof(CommunityRecord, titleLength) == 0x10C);
	static_assert(offsetof(CommunityRecord, descriptionLength) == 0x310);
	static_assert(offsetof(CommunityRecord, appDataSize) == 0x714);
	static_assert(offsetof(CommunityRecord, iconSize) == 0x10744);

	// raw response values for the shared record fields
	struct CommunityReply
	{
		std::string_view appData, communityId, name, description, pid, icon;
	};

	// fill the shared fields from a server reply (fields absent from the reply are skipped,
	// community_id and pid are required)
	nnResult StoreCommunityReply(CommunityRecord& rec, const CommunityReply& reply);

	// guest-callable accessors shared by the community data classes; T holds a CommunityRecord named rec
	template<typename T>
	struct CommunityRecordApi
	{
		static void ResetCounts(T* self)
		{
			CommunityRecord& r = self->rec;
			r.titleLength = 0;
			r.appDataSize = 0;
			r.descriptionLength = 0;
			r.ownerPid = 0;
			r.communityId = 0;
			r.flags = 0;
			r.iconSize = 0;
			if constexpr (requires { &T::ownerNickname; })
				self->ownerNickname[0] = 0;
		}

		static T* Construct(T* self)
		{
			if (!self)
				return nullptr;
			ResetCounts(self);
			return self;
		}

		static void Clear(T* self)
		{
			ResetCounts(self);
			self->rec.title[0] = 0;
			self->rec.description[0] = 0;
			self->rec.appData[0] = 0;
			self->rec.icon[0] = 0;
		}

		static bool TestFlags(T* self, uint32 mask)
		{
			return ((uint32)self->rec.flags & mask) != 0;
		}

		static uint32 GetCommunityId(T* self)
		{
			return self->rec.communityId;
		}

		static nnResult GetCommunityCode(T* self, char* out, uint32 outSize)
		{
			if (!out)
				return kOlvInvalidPointer;
			if (outSize < 13)
				return kOlvNotEnoughSize;
			uint32 length = 0;
			return CommunityCodeFromId(out, &length, self->rec.communityId) ? kOlvSuccess : kOlvInvalidParameter;
		}

		static uint32 GetOwnerPid(T* self)
		{
			return self->rec.ownerPid;
		}

		static nnResult GetTitleText(T* self, uint16be* out, uint32 maxChars)
		{
			if (!out)
				return kOlvInvalidPointer;
			if (maxChars == 0)
				return kOlvNotEnoughSize;
			if (!TestFlags(self, CommunityFlag::Title))
				return kOlvMissingData;
			memset(out, 0, maxChars * sizeof(uint16be));
			WideCopyPadded(out, self->rec.title, std::min<uint32>(maxChars, self->rec.titleLength));
			return kOlvSuccess;
		}

		static nnResult GetDescriptionText(T* self, uint16be* out, uint32 maxChars)
		{
			if (!out)
				return kOlvInvalidPointer;
			if (maxChars == 0)
				return kOlvNotEnoughSize;
			if (!TestFlags(self, CommunityFlag::Description))
				return kOlvMissingData;
			memset(out, 0, maxChars * sizeof(uint16be));
			WideCopyPadded(out, self->rec.description, std::min<uint32>(maxChars, std::size(self->rec.description)));
			return kOlvSuccess;
		}

		static nnResult GetAppData(T* self, uint8* out, uint32be* outSize, uint32 maxSize)
		{
			if (!out)
				return kOlvInvalidPointer;
			if (maxSize == 0)
				return kOlvNotEnoughSize;
			if (!TestFlags(self, CommunityFlag::AppData))
				return kOlvMissingData;
			const uint32 n = std::min<uint32>(maxSize, self->rec.appDataSize);
			memcpy(out, self->rec.appData, n);
			if (outSize)
				*outSize = n;
			return kOlvSuccess;
		}

		static uint32 GetAppDataSize(T* self)
		{
			return TestFlags(self, CommunityFlag::AppData) ? (uint32)self->rec.appDataSize : 0;
		}

		static nnResult GetIconData(T* self, uint8* out, uint32be* outSize, uint32 maxSize)
		{
			if (!out)
				return kOlvInvalidPointer;
			if (maxSize < sizeof(self->rec.icon))
				return kOlvNotEnoughSize;
			if (!TestFlags(self, CommunityFlag::Icon))
				return kOlvMissingData;
			const sint32 size = InflateTga(self->rec.icon, self->rec.iconSize, out, maxSize, TgaKind::CommunityIcon);
			if (outSize)
				*outSize = size >= 0 ? (uint32)size : 0;
			if (size >= 0)
				return kOlvSuccess;
			cemuLog_log(LogType::Force, "nn_olv: community icon could not be {}", size == -1 ? "inflated" : "read as TGA");
			return kOlvBadTextField;
		}

	};

	// registers the shared accessors; MANGLED is the length-prefixed class name, e.g. "21UploadedCommunityData"
#define OLV_REGISTER_COMMUNITY_RECORD(TYPE, MANGLED) \
	{ \
		using RecordApi = ::nn::olv::CommunityRecordApi<TYPE>; \
		cafeExportRegisterFunc(RecordApi::Construct, "nn_olv", "__ct__Q3_2nn3olv" MANGLED "Fv", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::TestFlags, "nn_olv", "TestFlags__Q3_2nn3olv" MANGLED "CFUi", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::GetCommunityId, "nn_olv", "GetCommunityId__Q3_2nn3olv" MANGLED "CFv", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::GetCommunityCode, "nn_olv", "GetCommunityCode__Q3_2nn3olv" MANGLED "CFPcUi", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::GetOwnerPid, "nn_olv", "GetOwnerPid__Q3_2nn3olv" MANGLED "CFv", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::GetTitleText, "nn_olv", "GetTitleText__Q3_2nn3olv" MANGLED "CFPwUi", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::GetDescriptionText, "nn_olv", "GetDescriptionText__Q3_2nn3olv" MANGLED "CFPwUi", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::GetAppData, "nn_olv", "GetAppData__Q3_2nn3olv" MANGLED "CFPUcPUiUi", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::GetAppDataSize, "nn_olv", "GetAppDataSize__Q3_2nn3olv" MANGLED "CFv", LogType::NN_OLV); \
		cafeExportRegisterFunc(RecordApi::GetIconData, "nn_olv", "GetIconData__Q3_2nn3olv" MANGLED "CFPUcPUiUi", LogType::NN_OLV); \
	}

	// per-file export tables (called when nn_olv is mapped)
	void RegisterInitializeExports();
	void RegisterDownloadCommunityExports();
	void RegisterUploadCommunityExports();
	void RegisterUploadFavoriteExports();
	void RegisterPostExports();
}
