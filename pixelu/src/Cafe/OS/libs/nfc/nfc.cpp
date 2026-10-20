// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nfc.rpl drives the GamePad's NFC reader. Games (and our ntag library) start
// one operation at a time per channel - read, write, raw command or abort -
// and poll NFCProc, which finishes the operation once a tag is on the reader,
// the discovery timeout ran out, or it was aborted, and then calls the game's
// callback.
//
// The emulated reader holds a Type 1 tag image loaded from a file the user
// "touches" from the GUI. The payload games care about is the first NDEF
// record with the "unknown" type name format; the tag's locked blocks are
// handed over as well. Writes replace that NDEF message and save the file.
// A touched tag stays on the reader for two seconds.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/libs/nn_nfp/nn_nfp.h"
#include "Common/FileStream.h"

#include "TagV0.h"
#include "ndef.h"

namespace nfc
{
	namespace
	{
		constexpr uint32 kChannels = 2;
		constexpr sint32 kModeInvalid = -1;
		constexpr sint32 kModeIdle = 0;
		constexpr sint32 kModeActive = 1;
		constexpr uint32 kMaxPayload = 0x200;
		constexpr auto kTagPresence = std::chrono::seconds(2);
		constexpr sint32 kStatusWriteFailed = 0x22;

		using Clock = std::chrono::system_clock;

		enum class Op
		{
			None,
			Read,
			Write,
			Raw,
			Abort,
		};

		struct Channel
		{
			bool initialized = false;
			bool ready = false; // NFCProc ran since init; operations may start
			Op op = Op::None;
			sint32 mode = kModeIdle;

			// the tag on the reader
			std::unique_ptr<Type1Tag> tag;
			fs::path tagPath;
			Clock::time_point touchTime{};
			bool tagReported = false; // the game was told a tag arrived

			// the running operation
			Clock::time_point deadline{};
			NFCUid filterUid{};
			NFCUid filterMask{};
			MPTR callback = MPTR_NULL;
			void* callbackContext = nullptr;
			std::vector<uint8> writeMessage;

			MPTR detectCallback = MPTR_NULL;
			void* detectContext = nullptr;

			MPTR tagInfoCallback = MPTR_NULL;
		};

		Channel g_channels[kChannels];
		SysAllocator<NFCTagInfo> g_tagInfo[kChannels];

		// a tag touched from the GUI thread, picked up by the next NFCProc(0)
		struct PendingTouch
		{
			std::unique_ptr<Type1Tag> tag;
			fs::path path;
			Clock::time_point when;
		};
		std::mutex g_touchMutex;
		std::optional<PendingTouch> g_pendingTouch;

		void ResetChannel(uint32 chan)
		{
			g_channels[chan] = Channel{};
			if (chan == 0)
			{
				std::lock_guard lock(g_touchMutex);
				g_pendingTouch.reset();
			}
		}

		Clock::time_point DeadlineFromTimeout(uint32 timeoutMs)
		{
			if (timeoutMs == 0)
				return Clock::time_point::max(); // wait forever
			return Clock::now() + std::chrono::milliseconds(timeoutMs);
		}

		bool UidMatches(const NFCUid& uid, const NFCUid& want, const NFCUid& mask)
		{
			for (size_t i = 0; i < sizeof(uid.uid); i++)
			{
				if ((uid.uid[i] & mask.uid[i]) != want.uid[i])
					return false;
			}
			return true;
		}

		// common checks of NFCRead/NFCWrite/NFCSendRawData
		sint32 CanStart(uint32 chan, sint32 base)
		{
			if (!NFCIsInit(chan))
				return NFC_MAKE_RESULT(base, NFC_RESULT_UNINITIALIZED);
			if (NFCGetMode(chan) == kModeActive && NFCSetMode(chan, kModeIdle) < 0)
				return NFC_MAKE_RESULT(base, NFC_RESULT_INVALID_MODE);
			const Channel& ch = g_channels[chan];
			if (!ch.ready || ch.op != Op::None)
				return NFC_MAKE_RESULT(base, NFC_RESULT_INVALID_STATE);
			return NFC_RESULT_SUCCESS;
		}

		void Start(uint32 chan, Op op, uint32 timeoutMs, MPTR callback, void* context)
		{
			Channel& ch = g_channels[chan];
			ch.op = op;
			ch.callback = callback;
			ch.callbackContext = context;
			ch.deadline = DeadlineFromTimeout(timeoutMs);
		}

		void SetFilter(Channel& ch, const NFCUid* uid, const NFCUid* mask)
		{
			ch.filterUid = *uid;
			ch.filterMask = *mask;
		}

		void FinishRead(uint32 chan)
		{
			Channel& ch = g_channels[chan];
			StackAllocator<NFCUid> uid;
			StackAllocator<uint8, kMaxPayload> data;
			StackAllocator<uint8, kMaxPayload> locked;
			memset(uid.GetPointer(), 0, sizeof(NFCUid));
			uint32 dataSize = 0;
			uint32 lockedSize = 0;
			sint32 result;

			if (!ch.tag)
				result = NFC_MAKE_RESULT(NFC_RESULT_BASE_READ, NFC_RESULT_NO_TAG);
			else
			{
				memcpy(uid.GetPointer(), ch.tag->Uid(), sizeof(NFCUid));
				std::vector<ndef::Record> records;
				const ndef::Record* rec = nullptr;
				if (!UidMatches(*uid.GetPointer(), ch.filterUid, ch.filterMask))
					result = NFC_MAKE_RESULT(NFC_RESULT_BASE_READ, NFC_RESULT_UID_MISMATCH);
				else if (!ndef::ParseMessage(ch.tag->Ndef(), records) ||
						 (rec = ndef::FindByTnf(records, ndef::kTnfUnknown)) == nullptr ||
						 rec->payload.empty() || rec->payload.size() >= kMaxPayload)
					result = NFC_MAKE_RESULT(NFC_RESULT_BASE_TAG_PARSE, NFC_RESULT_INVALID_TAG);
				else
				{
					dataSize = (uint32)rec->payload.size();
					memcpy(data.GetPointer(), rec->payload.data(), dataSize);
					const std::vector<uint8>& lockedArea = ch.tag->LockedArea();
					lockedSize = (uint32)std::min<size_t>(lockedArea.size(), kMaxPayload);
					memcpy(locked.GetPointer(), lockedArea.data(), lockedSize);
					result = NFC_RESULT_SUCCESS;
				}
			}
			const uint32 readOnly = 0;
			PPCCoreCallback(ch.callback, chan, result, uid.GetPointer(), readOnly, dataSize, data.GetPointer(), lockedSize, locked.GetPointer(), ch.callbackContext);
		}

		void FinishWrite(uint32 chan)
		{
			Channel& ch = g_channels[chan];
			sint32 result;
			NFCUid uid{};
			if (ch.tag)
				memcpy(uid.uid, ch.tag->Uid(), sizeof(uid.uid));
			if (!ch.tag)
				result = NFC_MAKE_RESULT(NFC_RESULT_BASE_WRITE, NFC_RESULT_NO_TAG);
			else if (!UidMatches(uid, ch.filterUid, ch.filterMask))
				result = NFC_MAKE_RESULT(NFC_RESULT_BASE_WRITE, NFC_RESULT_UID_MISMATCH);
			else
			{
				ch.tag->SetNdef(std::move(ch.writeMessage));
				std::vector<uint8> image;
				result = NFC_MAKE_RESULT(NFC_RESULT_BASE_WRITE, kStatusWriteFailed);
				if (ch.tag->Save(image))
				{
					std::unique_ptr<FileStream> file(FileStream::openFile2(ch.tagPath, true));
					if (file && file->writeData(image.data(), (sint32)image.size()) == (sint32)image.size())
						result = NFC_RESULT_SUCCESS;
					else
						cemuLog_log(LogType::Force, "nfc: could not write tag file {}", _pathToUtf8(ch.tagPath));
				}
			}
			ch.writeMessage.clear();
			PPCCoreCallback(ch.callback, chan, result, ch.callbackContext);
		}

		void FinishRaw(uint32 chan)
		{
			Channel& ch = g_channels[chan];
			// no command is really sent; this only reports whether a tag is there
			const sint32 result = ch.tag ? NFC_RESULT_SUCCESS : NFC_MAKE_RESULT(NFC_RESULT_BASE_SEND_RAW_DATA, NFC_RESULT_NO_TAG);
			const uint32 responseSize = 0;
			const MPTR response = MPTR_NULL;
			PPCCoreCallback(ch.callback, chan, result, responseSize, response, ch.callbackContext);
		}

		void FinishAbort(uint32 chan)
		{
			Channel& ch = g_channels[chan];
			PPCCoreCallback(ch.callback, chan, (sint32)0, ch.callbackContext);
		}

		void PickUpTouchedTag(Channel& ch)
		{
			std::lock_guard lock(g_touchMutex);
			if (!g_pendingTouch)
				return;
			ch.tag = std::move(g_pendingTouch->tag);
			ch.tagPath = std::move(g_pendingTouch->path);
			ch.touchTime = g_pendingTouch->when;
			g_pendingTouch.reset();
		}

		sint32 TagInfoResult(sint32 rawResult)
		{
			if (rawResult == NFC_MAKE_RESULT(NFC_RESULT_BASE_SEND_RAW_DATA, NFC_RESULT_NO_TAG))
				return NFC_MAKE_RESULT(NFC_RESULT_BASE_GET_TAG_INFO, NFC_RESULT_TAG_INFO_TIMEOUT);
			return rawResult;
		}

		// raw-data callback used by NFCGetTagInfo
		void OnTagInfoRawDone(uint32 chan, sint32 error, uint32 responseSize, void* response, void* context)
		{
			if (chan >= kChannels)
				return;
			Channel& ch = g_channels[chan];
			NFCTagInfo* info = g_tagInfo[chan].GetPtr();
			error = TagInfoResult(error);
			if (error == NFC_RESULT_SUCCESS && ch.tag)
			{
				memset(info, 0, sizeof(NFCTagInfo));
				info->uidSize = sizeof(NFCUid);
				memcpy(info->uid, ch.tag->Uid(), sizeof(NFCUid));
				info->technology = NFC_TECHNOLOGY_A;
				info->protocol = NFC_PROTOCOL_T1T;
			}
			PPCCoreCallback(ch.tagInfoCallback, chan, error, info, context);
		}
	}

	sint32 NFCInit(uint32 chan)
	{
		return NFCInitEx(chan, 0);
	}

	sint32 NFCInitEx(uint32 chan, uint32 powerMode)
	{
		cemu_assert(chan < kChannels);
		ResetChannel(chan);
		g_channels[chan].initialized = true;
		return NFC_RESULT_SUCCESS;
	}

	sint32 NFCShutdown(uint32 chan)
	{
		cemu_assert(chan < kChannels);
		ResetChannel(chan);
		return NFC_RESULT_SUCCESS;
	}

	bool NFCIsInit(uint32 chan)
	{
		cemu_assert(chan < kChannels);
		return g_channels[chan].initialized;
	}

	void NFCProc(uint32 chan)
	{
		cemu_assert(chan < kChannels);
		Channel& ch = g_channels[chan];
		if (!ch.initialized)
			return;
		ch.ready = true;
		if (chan == 0)
			PickUpTouchedTag(ch);

		const bool busy = ch.op == Op::Read || ch.op == Op::Write || ch.op == Op::Raw;
		if (ch.tag)
		{
			if (!ch.tagReported && busy)
			{
				ch.tagReported = true;
				if (ch.detectCallback)
					PPCCoreCallback(ch.detectCallback, chan, (uint32)1, ch.detectContext);
			}
			if (Clock::now() > ch.touchTime + kTagPresence)
			{
				// the tag was lifted off the reader
				ch.tag.reset();
				ch.tagPath.clear();
			}
		}
		else if (ch.tagReported && ch.op == Op::None)
		{
			ch.tagReported = false;
			if (ch.detectCallback)
				PPCCoreCallback(ch.detectCallback, chan, (uint32)0, ch.detectContext);
		}

		const bool canFinish = ch.op == Op::Abort || ch.tag || Clock::now() > ch.deadline;
		if (!canFinish)
			return;
		const Op op = ch.op;
		ch.op = Op::None;
		switch (op)
		{
		case Op::Read: FinishRead(chan); break;
		case Op::Write: FinishWrite(chan); break;
		case Op::Raw: FinishRaw(chan); break;
		case Op::Abort: FinishAbort(chan); break;
		case Op::None: break;
		}
		ch.mode = kModeIdle;
	}

	sint32 NFCGetMode(uint32 chan)
	{
		cemu_assert(chan < kChannels);
		if (!g_channels[chan].initialized)
			return kModeInvalid;
		return g_channels[chan].mode;
	}

	sint32 NFCSetMode(uint32 chan, sint32 mode)
	{
		cemu_assert(chan < kChannels);
		if (!g_channels[chan].initialized)
			return NFC_MAKE_RESULT(NFC_RESULT_BASE_SET_MODE, NFC_RESULT_UNINITIALIZED);
		g_channels[chan].mode = mode;
		return NFC_RESULT_SUCCESS;
	}

	void NFCSetTagDetectCallback(uint32 chan, MPTR callback, void* context)
	{
		cemu_assert(chan < kChannels);
		g_channels[chan].detectCallback = callback;
		g_channels[chan].detectContext = context;
	}

	sint32 NFCAbort(uint32 chan, MPTR callback, void* context)
	{
		cemu_assert(chan < kChannels);
		if (!NFCIsInit(chan))
			return NFC_MAKE_RESULT(NFC_RESULT_BASE_ABORT, NFC_RESULT_UNINITIALIZED);
		if (g_channels[chan].op == Op::None)
			return NFC_MAKE_RESULT(NFC_RESULT_BASE_ABORT, NFC_RESULT_INVALID_STATE);
		// the aborted operation's own callback is not called
		Start(chan, Op::Abort, 0, callback, context);
		return NFC_RESULT_SUCCESS;
	}

	sint32 NFCGetTagInfo(uint32 chan, uint32 discoveryTimeout, MPTR callback, void* context)
	{
		cemu_assert(chan < kChannels);
		// a game using nn_nfp has its amiibo there
		if (nnNfp_isInitialized())
			return nn::nfp::NFCGetTagInfo(chan, discoveryTimeout, callback, context);

		g_channels[chan].tagInfoCallback = callback;
		const sint32 r = NFCSendRawData(chan, true, discoveryTimeout, 1000, 0, 0, nullptr, makeCallableExport<OnTagInfoRawDone>(), context);
		return TagInfoResult(r);
	}

	sint32 NFCSendRawData(uint32 chan, bool startDiscovery, uint32 discoveryTimeout, uint32 commandTimeout, uint32 commandSize, uint32 responseSize, void* commandData, MPTR callback, void* context)
	{
		cemu_assert(chan < kChannels);
		if (!NFCIsInit(chan))
			return NFC_MAKE_RESULT(NFC_RESULT_BASE_SEND_RAW_DATA, NFC_RESULT_UNINITIALIZED);
		if (!startDiscovery) // commands to an already selected tag are not emulated
			return NFC_MAKE_RESULT(NFC_RESULT_BASE_SEND_RAW_DATA, NFC_RESULT_INVALID_MODE);
		if (sint32 r = CanStart(chan, NFC_RESULT_BASE_SEND_RAW_DATA); r != NFC_RESULT_SUCCESS)
			return r;
		Start(chan, Op::Raw, discoveryTimeout, callback, context);
		return NFC_RESULT_SUCCESS;
	}

	sint32 NFCRead(uint32 chan, uint32 discoveryTimeout, NFCUid* uid, NFCUid* uidMask, MPTR callback, void* context)
	{
		cemu_assert(chan < kChannels);
		if (sint32 r = CanStart(chan, NFC_RESULT_BASE_READ); r != NFC_RESULT_SUCCESS)
			return r;
		Start(chan, Op::Read, discoveryTimeout, callback, context);
		SetFilter(g_channels[chan], uid, uidMask);
		return NFC_RESULT_SUCCESS;
	}

	sint32 NFCWrite(uint32 chan, uint32 discoveryTimeout, NFCUid* uid, NFCUid* uidMask, uint32 size, void* data, MPTR callback, void* context)
	{
		cemu_assert(chan < kChannels);
		if (sint32 r = CanStart(chan, NFC_RESULT_BASE_WRITE); r != NFC_RESULT_SUCCESS)
			return r;
		// the game's data becomes the single "unknown" record of the NDEF message
		ndef::Record rec;
		rec.tnf = ndef::kTnfUnknown;
		const uint8* bytes = (const uint8*)data;
		rec.payload.assign(bytes, bytes + size);
		g_channels[chan].writeMessage = ndef::BuildMessage({rec});
		Start(chan, Op::Write, discoveryTimeout, callback, context);
		SetFilter(g_channels[chan], uid, uidMask);
		return NFC_RESULT_SUCCESS;
	}

	bool TouchTagFromFile(const fs::path& filePath, uint32* nfcError)
	{
		if (nnNfp_isInitialized())
			return nnNfp_touchNfcTagFromFile(filePath, nfcError);

		auto file = FileStream::LoadIntoMemory(filePath);
		if (!file)
		{
			*nfcError = NFC_TOUCH_TAG_ERROR_NO_ACCESS;
			return false;
		}
		auto tag = Type1Tag::Load(*file);
		if (!tag)
		{
			*nfcError = NFC_TOUCH_TAG_ERROR_INVALID_FILE_FORMAT;
			return false;
		}
		{
			std::lock_guard lock(g_touchMutex);
			g_pendingTouch = PendingTouch{std::move(tag), filePath, Clock::now()};
		}
		*nfcError = NFC_TOUCH_TAG_ERROR_NONE;
		return true;
	}

	class : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "nfc";
		}

		void RPLMapped() override
		{
			// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("nfc", fn, LogType::NFC)
			EXPORT_FN(NFCInit); EXPORT_FN(NFCInitEx); EXPORT_FN(NFCShutdown);
			EXPORT_FN(NFCIsInit); EXPORT_FN(NFCProc); EXPORT_FN(NFCGetMode);
			EXPORT_FN(NFCSetMode); EXPORT_FN(NFCSetTagDetectCallback); EXPORT_FN(NFCGetTagInfo);
			EXPORT_FN(NFCSendRawData); EXPORT_FN(NFCAbort); EXPORT_FN(NFCRead);
			EXPORT_FN(NFCWrite);
#undef EXPORT_FN
		}
	} s_nfcModule;

	COSModule* GetModule()
	{
		return &s_nfcModule;
	}
}
