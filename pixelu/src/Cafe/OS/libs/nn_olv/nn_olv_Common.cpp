// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nn_olv_Common.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Common/StackAllocator.h"
#include "config/ActiveSettings.h"
#include <zlib.h>
#include <future>
#include <charconv>

namespace nn::olv
{
	ParamPackStorage g_ParamPack;
	DiscoveryResultStorage g_DiscoveryResults;
	OliveState g_olv;

	// --- requests ---------------------------------------------------------------

	nnResult RunBlockingOnHost(const std::function<nnResult()>& fn)
	{
		StackAllocator<coreinit::OSEvent> done;
		coreinit::OSInitEvent(done.GetPointer(), coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_MANUAL);
		coreinit::OSEvent* event = done.GetPointer();
		std::future<nnResult> result = std::async(std::launch::async, [&fn, event]() {
			nnResult r = fn();
			coreinit::OSSignalEvent(event);
			return r;
		});
		coreinit::OSWaitEvent(event);
		return result.get();
	}

	void BeginOliveRequest(CurlRequestHelper& req, const std::string& url)
	{
		req.initate(ActiveSettings::GetNetworkService(), url, CurlRequestHelper::SERVER_SSL_CONTEXT::OLIVE);
		req.addHeaderField("X-Nintendo-ServiceToken", g_DiscoveryResults.serviceToken);
		req.addHeaderField("X-Nintendo-ParamPack", g_ParamPack.encodedParamPack);
		curl_easy_setopt(req.getCURL(), CURLOPT_USERAGENT, g_DiscoveryResults.userAgent);
	}

	nnResult FinishOliveRequest(CurlRequestHelper& req, const std::string& url, bool post, pugi::xml_document& reply)
	{
		const bool sent = req.submitRequest(post);
		long status = 0;
		curl_easy_getinfo(req.getCURL(), CURLINFO_RESPONSE_CODE, &status);
		// an HTTP error page still carries an Olive <result> worth decoding
		if (!sent)
		{
			cemuLog_log(LogType::Force, "nn_olv: request to {} failed (HTTP {})", url, status);
			if (status < 400)
				return kOlvRequestFailed;
		}
		const std::vector<uint8>& body = req.getReceivedData();
		if (!reply.load_buffer(body.data(), body.size()))
		{
			cemuLog_log(LogType::Force, "nn_olv: reply from {} is not valid XML", url);
			return kOlvBadXml;
		}
		const nnResult envelope = CheckResultEnvelope(reply);
		if (NN_RESULT_IS_FAILURE(envelope))
			return envelope;
		if (status != 200)
			return OlvStatus((uint32)status + 4000);
		return kOlvSuccess;
	}

	nnResult CheckResultEnvelope(const pugi::xml_document& doc)
	{
		pugi::xml_node result = doc.child("result");
		if (!result)
		{
			cemuLog_log(LogType::Force, "nn_olv: reply has no <result> element");
			return kOlvBadXml;
		}
		if (std::string_view(result.child_value("has_error")) == "1")
		{
			const sint32 detail = StringHelpers::ToInt(result.child_value("error_code"), -1);
			if (detail >= 0)
				return OlvStatus((uint32)detail + 5000);
			return OlvStatus((uint32)(StringHelpers::ToInt(result.child_value("code"), -1) + 4000));
		}
		if (std::string_view(result.child_value("version")) != "1")
			return kOlvBadVersion;
		return kOlvSuccess;
	}

	void AppendQuery(std::string& url, std::string_view query)
	{
		url.push_back(url.find('?') == std::string::npos ? '?' : '&');
		url.append(query);
	}

	// --- wide strings -----------------------------------------------------------

	size_t WideLength(const uint16be* str, size_t maxLength)
	{
		size_t n = 0;
		while (n < maxLength && (uint16)str[n] != 0)
			n++;
		return n;
	}

	void WideCopyPadded(uint16be* dst, const uint16be* src, size_t count)
	{
		size_t i = 0;
		for (; i < count && (uint16)src[i] != 0; i++)
			dst[i] = src[i];
		for (; i < count; i++)
			dst[i] = 0;
	}

	nnResult WideAssignChecked(uint16be* dst, size_t capacity, const uint16be* src)
	{
		if (WideLength(src, capacity) >= capacity)
			return kOlvNotEnoughSize;
		memset(dst, 0, capacity * sizeof(uint16be));
		WideCopyPadded(dst, src, capacity);
		return kOlvSuccess;
	}

	uint32 WideAssignTruncated(uint16be* dst, size_t capacity, const uint16be* src)
	{
		const size_t n = WideLength(src, capacity - 1);
		for (size_t i = 0; i < n; i++)
			dst[i] = src[i];
		dst[n] = 0;
		return (uint32)n;
	}

	uint32 Utf8ToWideTerminated(uint16be* dst, size_t capacity, std::string_view text)
	{
		const std::vector<uint16be> wide = StringHelpers::FromUtf8(text);
		const size_t n = std::min(wide.size(), capacity - 1);
		std::copy_n(wide.begin(), n, dst);
		dst[n] = 0;
		return (uint32)n;
	}

	uint32 Utf8ToWideFixed(uint16be* dst, size_t capacity, std::string_view text)
	{
		const std::vector<uint16be> wide = StringHelpers::FromUtf8(text);
		const size_t n = std::min(wide.size(), capacity);
		std::copy_n(wide.begin(), n, dst);
		return (uint32)n;
	}

	// --- TGA ------------------------------------------------------------------------

	namespace
	{
		// the fields of the 18 byte TGA header we look at (little endian on disk)
		struct TgaInfo
		{
			uint8 idLength, colorMapType, imageType;
			uint16 colorMapFirst, colorMapLength;
			uint8 colorMapDepth;
			uint16 originX, originY, width, height;
			uint8 pixelDepth, descriptor;
		};

		TgaInfo ReadTgaHeader(const uint8* p)
		{
			auto u16 = [p](int at) { return (uint16)(p[at] | (p[at + 1] << 8)); };
			TgaInfo h;
			h.idLength = p[0];
			h.colorMapType = p[1];
			h.imageType = p[2];
			h.colorMapFirst = u16(3);
			h.colorMapLength = u16(5);
			h.colorMapDepth = p[7];
			h.originX = u16(8);
			h.originY = u16(10);
			h.width = u16(12);
			h.height = u16(14);
			h.pixelDepth = p[16];
			h.descriptor = p[17];
			return h;
		}

		const char* TgaProblem(const TgaInfo& h, TgaKind kind)
		{
			switch (kind)
			{
			case TgaKind::Painting:
			{
				const bool plain = h.idLength == 0 && h.colorMapType == 0 && h.imageType == 2 &&
					h.colorMapFirst == 0 && h.colorMapLength == 0 && h.colorMapDepth == 0 &&
					h.originX == 0 && h.originY == 0;
				if (!plain || h.width != 320 || h.height != 120 || h.pixelDepth != 32 || h.descriptor != 8)
					return "painting must be an uncompressed 320x120 32-bit TGA";
				return nullptr;
			}
			case TgaKind::CommunityIcon:
				if (h.width != 128 || h.height != 128 || h.pixelDepth != 32)
					return "community icon must be a 128x128 32-bit TGA";
				return nullptr;
			case TgaKind::Icon100or200:
				if (h.pixelDepth != 32)
					return "icon must be a 32-bit TGA";
				if (!((h.width == 100 && h.height == 100) || (h.width == 200 && h.height == 200)))
					return "icon must be 100x100 or 200x200";
				return nullptr;
			}
			return "unknown TGA check";
		}
	}

	bool IsValidTga(const uint8* data, uint32 size, TgaKind kind)
	{
		if (!data || size < 18)
		{
			cemuLog_log(LogType::Force, "nn_olv: TGA data is too short");
			return false;
		}
		if (const char* problem = TgaProblem(ReadTgaHeader(data), kind))
		{
			cemuLog_log(LogType::Force, "nn_olv: {}", problem);
			return false;
		}
		return true;
	}

	sint32 InflateTga(const uint8* in, uint32 inSize, uint8* out, uint32 outCapacity, TgaKind kind)
	{
		if (!in || !out || inSize == 0)
			return -1;
		uLongf produced = outCapacity;
		const int r = uncompress(out, &produced, in, inSize);
		if (r != Z_OK)
		{
			cemuLog_log(LogType::Force, "nn_olv: inflating image failed (zlib {})", r);
			return -1;
		}
		return IsValidTga(out, (uint32)produced, kind) ? (sint32)produced : -2;
	}

	sint32 DeflateTga(const uint8* in, uint32 inSize, uint8* out, uint32 outCapacity, TgaKind kind)
	{
		if (inSize != outCapacity)
		{
			cemuLog_log(LogType::Force, "nn_olv: image buffer sizes differ ({} vs {})", inSize, outCapacity);
			return -1;
		}
		if (!IsValidTga(in, inSize, kind))
			return -1;
		if (!out || inSize == 0)
			return -1;
		uLongf produced = outCapacity;
		const int r = compress(out, &produced, in, inSize);
		if (r != Z_OK)
		{
			cemuLog_log(LogType::Force, "nn_olv: compressing image failed (zlib {})", r);
			return -1;
		}
		return (sint32)produced;
	}

	// --- community codes --------------------------------------------------------------
	// The code is a scrambled form of the id followed by a check byte. The scramble is
	// a fixed sequence of xor-shift steps, each of which can be undone.

	namespace
	{
		constexpr uint32 kCodeSalt = 0x20121002;

		constexpr uint32 SwapNibbleMix(uint32 v) { return v ^ ((v & 0x0F0F0F0Fu) << 4); }
		constexpr uint32 SpreadUp(uint32 v) { return v ^ (v << 18) ^ (v << 24) ^ (v << 30); }
		constexpr uint32 SpreadDown(uint32 v) { return v ^ (v >> 17) ^ (v >> 23) ^ (v >> 29); }

		constexpr uint32 ScrambleId(uint32 id)
		{
			return SpreadDown(SwapNibbleMix(SpreadUp(id))) ^ kCodeSalt;
		}

		constexpr uint32 UnscrambleCode(uint32 code)
		{
			// each step is its own inverse for these shift distances
			return SpreadUp(SwapNibbleMix(SpreadDown(code ^ kCodeSalt)));
		}

		// 40-bit value printed as the code: check byte above the scrambled id
		constexpr uint64 WithCheckByte(uint32 scrambled)
		{
			const uint32 check = ((scrambled >> 24) ^ (scrambled >> 16) ^ (scrambled >> 8) ^ scrambled ^ 0xFF) & 0xFF;
			uint64 value = ((uint64)check << 32) | scrambled;
			// large values would not fit into 12 decimal digits; their top bit is dropped
			if (check > 0xE8 || (check == 0xE8 && scrambled > 0xD4A50FFFu))
				value &= 0x7FFFFFFFFFull;
			return value;
		}

		static_assert(UnscrambleCode(ScrambleId(140500)) == 140500);
		static_assert(WithCheckByte(ScrambleId(140500)) == 717651734336ull);

		bool ParseCode(const char* text, uint64& value)
		{
			while (*text == ' ' || *text == '\t' || *text == '\n')
				text++;
			size_t len = 0;
			while (len < 12 && text[len] >= '0' && text[len] <= '9')
				len++;
			if (len == 0)
				return false;
			return std::from_chars(text, text + len, value).ec == std::errc();
		}

		bool IsWellFormedCode(const char* text, uint64& value)
		{
			return ParseCode(text, value) && WithCheckByte((uint32)value) == value;
		}
	}

	bool CommunityIdFromCode(uint32* outId, const char* code)
	{
		uint64 value;
		if (!code || !IsWellFormedCode(code, value))
			return false;
		*outId = UnscrambleCode((uint32)value);
		return true;
	}

	bool CommunityCodeFromId(char* outCode, uint32* outLength, uint32 communityId)
	{
		if (communityId == 0 || communityId == 0xFFFFFFFF)
			return false;
		const std::string text = fmt::format("{:012}", WithCheckByte(ScrambleId(communityId)));
		memcpy(outCode, text.c_str(), text.size() + 1);
		*outLength = (uint32)std::min<size_t>(text.size(), 12);
		uint64 value;
		return IsWellFormedCode(outCode, value);
	}

	// --- community records ---------------------------------------------------------

	nnResult StoreCommunityReply(CommunityRecord& rec, const CommunityReply& reply)
	{
		if (!reply.appData.empty())
		{
			const std::vector<uint8> bytes = NCrypto::base64Decode(reply.appData);
			if (bytes.empty())
				return kOlvBadTextField;
			const size_t n = std::min(bytes.size(), sizeof(rec.appData));
			memcpy(rec.appData, bytes.data(), n);
			rec.appDataSize = (uint32)n;
			rec.flags = rec.flags | CommunityFlag::AppData;
		}

		const sint64 id = StringHelpers::ToInt64(reply.communityId, -1);
		if (id == -1)
			return kOlvBadIntegerField;
		rec.communityId = (uint32)id;

		if (!reply.name.empty())
		{
			const uint32 n = Utf8ToWideFixed(rec.title, std::size(rec.title), reply.name);
			if (n == 0)
				return kOlvBadTextField;
			rec.titleLength = n;
			rec.flags = rec.flags | CommunityFlag::Title;
		}

		if (!reply.description.empty())
		{
			const uint32 n = Utf8ToWideFixed(rec.description, std::size(rec.description), reply.description);
			if (n == 0)
				return kOlvBadTextField;
			rec.descriptionLength = n;
			rec.flags = rec.flags | CommunityFlag::Description;
		}

		const sint64 pid = StringHelpers::ToInt64(reply.pid, -1);
		if (pid == -1)
			return kOlvBadIntegerField;
		rec.ownerPid = (uint32)pid;

		if (!reply.icon.empty())
		{
			const std::vector<uint8> bytes = NCrypto::base64Decode(reply.icon);
			if (bytes.empty())
				return kOlvBadTextField;
			const size_t n = std::min(bytes.size(), sizeof(rec.icon));
			memcpy(rec.icon, bytes.data(), n);
			rec.iconSize = (uint32)n;
			rec.flags = rec.flags | CommunityFlag::Icon;
		}
		return kOlvSuccess;
	}
}
