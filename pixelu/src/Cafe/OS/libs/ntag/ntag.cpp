// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// ntag reads and writes NFC tags in the "NTAG" application format some games
// use for their own figures and cards (Pokemon Rumble U, ...). It sits on top
// of the raw NFC library (nfc.cpp) and the tag crypto service /dev/ccr_nfc.
//
// Tag image (0x1C8 bytes = the tag's data area followed by its locked area),
// after decryption:
//   0x020  "NOFT" header: magic, version 1, write counter (u16), one byte
//   0x198  info header: offsets/sizes of the RW and RO area headers, the tag
//          uid and the format version
//   each area header: magic "RW"/"RO", data offset, data size, the maker
//          code and identify code of the game that formatted the tag
// A read decrypts the image, checks the headers against the game's format
// settings and hands the game both areas. A write reads the tag first, puts
// the new RW data (padded with random bytes) into the decrypted image, bumps
// the write counter, encrypts and writes back the NOFT header and RW area.
// All multi-byte fields on the tag are big-endian.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPC.h"
#include "Cafe/IOSU/ccr_nfc/iosu_ccr_nfc.h"
#include "ntag.h"

namespace ntag
{
	namespace
	{
		using iosu::ccr_nfc::CCRNFCCryptData;

		constexpr uint32 kChannels = 2;
		constexpr uint32 kImageSize = 0x1C8;
		constexpr uint32 kNoftOffset = 0x20;
		constexpr uint32 kInfoOffset = 0x198;
		constexpr uint32 kWriteBackPrefix = 0x28; // bytes in front of the RW area that are written back too
		constexpr uint32 kAreasTotal = 0x130;     // RW size + RO size
		constexpr uint32 kLockedExtra = 0x70;     // locked area size = RO size + this
		constexpr uint32 kWriteTimeout = 200;

		// errors reported to the game for tags that don't fit the format
		constexpr sint32 kErrFormatVersion = -0x2710;
		constexpr sint32 kErrMagic = -0x270F;
		constexpr sint32 kErrMakerCode = -0x270E;
		constexpr sint32 kErrSize = -0x270D;
		constexpr sint32 kErrLockedSize = -0x270C;
		constexpr sint32 kErrUid = -0x270B;
		constexpr sint32 kErrIdentifyCode = -0x2709;
		constexpr sint32 kErrUnfixedInfos = -0x2708;
		constexpr sint32 kErrLockedSecret = -0x2707;
		constexpr sint32 kErrWriteFailed = -0x3E3;

		// /dev/ccr_nfc ioctls
		constexpr uint32 kIoctlEncrypt = 1;
		constexpr uint32 kIoctlDecrypt = 2;

		struct NoftHeader
		{
			uint8 magic[4]; // "NOFT"
			uint8 version;
			uint8 writeCount[2]; // big-endian, unaligned
			uint8 unk;
		};
		static_assert(sizeof(NoftHeader) == 8);

		struct InfoHeader
		{
			uint16be rwHeaderOffset;
			uint16be rwSize;
			uint16be roHeaderOffset;
			uint16be roSize;
			nfc::NFCUid uid;
			uint8 formatVersion;
		};
		static_assert(sizeof(InfoHeader) == 0x10);

		struct AreaHeader
		{
			uint16be magic;
			uint16be offset;
			uint16be size;
			uint16be pad;
			uint32be makerCode;
			uint32be identifyCode;
		};
		static_assert(sizeof(AreaHeader) == 0x10);

		// NTAGData handed to the game's read callback
		struct GameArea
		{
			MEMPTR<uint8> data;
			uint16be size;
			uint16be pad;
			uint32be makerCode;
			uint32be identifyCode;
			uint8 reserved[0x20];
		};
		static_assert(sizeof(GameArea) == 0x30);

		struct GameTagData
		{
			nfc::NFCUid uid;
			uint8 readOnly;
			uint8 formatVersion;
			uint8 pad[3];
			GameArea rw;
			GameArea ro;
			uint8 reserved[0x20];
		};
		static_assert(sizeof(GameTagData) == 0x8C);

		struct FormatSettings
		{
			uint8 version = 0;
			uint32 makerCode = 0;
			uint32 identifyCode = 0;
		};

		struct PendingWrite
		{
			uint32 size = 0;
			uint8 data[kImageSize]{};
			nfc::NFCUid uid{};
			nfc::NFCUid uidMask{};
		};

		struct Channel
		{
			MPTR detectCallback = MPTR_NULL;
			MPTR abortCallback = MPTR_NULL;
			MPTR readCallback = MPTR_NULL;
			MPTR writeCallback = MPTR_NULL;
			PendingWrite write;
		};

		Channel g_channels[kChannels];
		FormatSettings g_format;
		bool g_cryptOpen = false;
		IOSDevHandle g_cryptHandle = 0;

		// The decoded headers of a tag image.
		struct ParsedTag
		{
			NoftHeader noft;
			InfoHeader info;
			AreaHeader rw;
			AreaHeader ro;
		};

		sint32 FromNfcResult(sint32 result)
		{
			if (result == NFC_RESULT_SUCCESS)
				return NTAG_RESULT_SUCCESS;
			switch (result & NFC_RESULT_MASK)
			{
			case NFC_RESULT_UNINITIALIZED: return NTAG_RESULT_UNINITIALIZED;
			case NFC_RESULT_INVALID_STATE: return NTAG_RESULT_INVALID_STATE;
			case NFC_RESULT_NO_TAG: return NTAG_RESULT_NO_TAG;
			case NFC_RESULT_UID_MISMATCH: return NTAG_RESULT_UID_MISMATCH;
			default: return NTAG_RESULT_INVALID;
			}
		}

		uint16 ReadU16(const uint8* p)
		{
			return (uint16)((p[0] << 8) | p[1]);
		}

		void WriteU16(uint8* p, uint16 v)
		{
			p[0] = (uint8)(v >> 8);
			p[1] = (uint8)v;
		}

		// ---- crypto through /dev/ccr_nfc ----------------------------------------

		IOSDevHandle CryptDevice()
		{
			if (!g_cryptOpen)
			{
				g_cryptHandle = coreinit::IOS_Open("/dev/ccr_nfc", 0);
				g_cryptOpen = true;
			}
			return g_cryptHandle;
		}

		// The service wants the image with the layout of a format 0 tag described.
		void DescribeFormat0(CCRNFCCryptData& d)
		{
			d.version = 0;
			d.dataSize = kImageSize;
			d.seedOffset = 0x25;
			d.keyGenSaltOffset = 0x1A8;
			d.uuidOffset = 0x198;
			d.unfixedInfosOffset = 0x28;
			d.unfixedInfosSize = 0x120;
			d.lockedSecretOffset = 0x168;
			d.lockedSecretSize = 0x30;
			d.unfixedInfosHmacOffset = 0;
			d.lockedSecretHmacOffset = 0x148;
		}

		sint32 RunCrypt(uint32 ioctl, const uint8* in, uint8* out)
		{
			StackAllocator<CCRNFCCryptData> request;
			StackAllocator<CCRNFCCryptData> reply;
			memset(request.GetPointer(), 0, sizeof(CCRNFCCryptData));
			memset(reply.GetPointer(), 0, sizeof(CCRNFCCryptData));
			DescribeFormat0(*request.GetPointer());
			memcpy(request->data, in, kImageSize);
			const sint32 r = (sint32)coreinit::IOS_Ioctl(CryptDevice(), ioctl, request.GetPointer(), sizeof(CCRNFCCryptData), reply.GetPointer(), sizeof(CCRNFCCryptData));
			memcpy(out, reply->data, kImageSize);
			return r;
		}

		sint32 Decrypt(const uint8* in, uint8* out)
		{
			const sint32 r = RunCrypt(kIoctlDecrypt, in, out);
			if (r == CCR_NFC_INVALID_UNFIXED_INFOS)
				return kErrUnfixedInfos;
			if (r == CCR_NFC_INVALID_LOCKED_SECRET)
				return kErrLockedSecret;
			return r;
		}

		sint32 Encrypt(const uint8* in, uint8* out)
		{
			return RunCrypt(kIoctlEncrypt, in, out);
		}

		// ---- parsing ---------------------------------------------------------------

		bool AreaFits(uint32 offset, uint32 size)
		{
			return offset + size <= kImageSize;
		}

		sint32 CheckHeaders(const ParsedTag& tag)
		{
			if (tag.info.formatVersion != g_format.version || tag.noft.version != 1)
			{
				cemuLog_log(LogType::Force, "ntag: tag has a different format version");
				return kErrFormatVersion;
			}
			if (memcmp(tag.noft.magic, "NOFT", 4) != 0 || tag.rw.magic != 0x5257 /* RW */ || tag.ro.magic != 0x524F /* RO */)
			{
				cemuLog_log(LogType::Force, "ntag: tag headers are damaged");
				return kErrMagic;
			}
			if (tag.rw.makerCode != g_format.makerCode || tag.ro.makerCode != g_format.makerCode)
			{
				cemuLog_log(LogType::Force, "ntag: tag belongs to another maker");
				return kErrMakerCode;
			}
			if (tag.info.formatVersion != 0 && (tag.rw.identifyCode != g_format.identifyCode || tag.ro.identifyCode != g_format.identifyCode))
			{
				cemuLog_log(LogType::Force, "ntag: tag belongs to another game");
				return kErrIdentifyCode;
			}
			if ((uint32)tag.rw.size + (uint32)tag.ro.size != kAreasTotal)
			{
				cemuLog_log(LogType::Force, "ntag: tag areas have the wrong size");
				return kErrSize;
			}
			return 0;
		}

		sint32 ReadHeaders(const uint8* image, ParsedTag& tag)
		{
			memcpy(&tag.noft, image + kNoftOffset, sizeof(NoftHeader));
			memcpy(&tag.info, image + kInfoOffset, sizeof(InfoHeader));
			const uint32 rwAt = tag.info.rwHeaderOffset;
			const uint32 roAt = tag.info.roHeaderOffset;
			if (!AreaFits(rwAt, sizeof(AreaHeader)) || !AreaFits(roAt, sizeof(AreaHeader)))
			{
				cemuLog_log(LogType::Force, "ntag: tag header offsets are out of range");
				return kErrSize;
			}
			memcpy(&tag.rw, image + rwAt, sizeof(AreaHeader));
			memcpy(&tag.ro, image + roAt, sizeof(AreaHeader));
			return CheckHeaders(tag);
		}

		// Decrypts and checks a tag image read from uid. On success plain holds
		// the decrypted image.
		sint32 OpenTag(const uint8* image, const nfc::NFCUid& uid, uint32 lockedSize, uint8* plain, ParsedTag& tag)
		{
			sint32 r = Decrypt(image, plain);
			if (r < 0)
				return r;
			r = ReadHeaders(plain, tag);
			if (r < 0)
				return r;
			if ((uint32)tag.ro.size + kLockedExtra != lockedSize)
			{
				cemuLog_log(LogType::Force, "ntag: tag locked area has the wrong size");
				return kErrLockedSize;
			}
			if (memcmp(tag.info.uid.uid, uid.uid, sizeof(uid.uid)) != 0)
			{
				cemuLog_log(LogType::Force, "ntag: tag uid doesn't match its contents");
				return kErrUid;
			}
			if (!AreaFits(tag.rw.offset, tag.rw.size) || !AreaFits(tag.ro.offset, tag.ro.size))
			{
				cemuLog_log(LogType::Force, "ntag: tag areas are out of range");
				return kErrSize;
			}
			return 0;
		}

		// Joins the tag's data and locked areas into one image.
		bool JoinImage(uint8* image, const void* data, uint32 dataSize, const void* locked, uint32 lockedSize)
		{
			if (dataSize > kImageSize || lockedSize > kImageSize - dataSize)
				return false;
			memset(image, 0, kImageSize);
			memcpy(image, data, dataSize);
			memcpy(image + dataSize, locked, lockedSize);
			return true;
		}

		// ---- NFC callbacks (run on the game's NFC thread) --------------------------

		void OnTagDetected(uint32 chan, uint32 hasTag, void* context)
		{
			cemuLog_log(LogType::NTAG, "ntag: tag {} on channel {}", hasTag ? "arrived" : "left", chan);
			if (chan < kChannels && g_channels[chan].detectCallback)
				PPCCoreCallback(g_channels[chan].detectCallback, chan, hasTag, context);
		}

		void OnAborted(uint32 chan, sint32 error, void* context)
		{
			if (chan < kChannels && g_channels[chan].abortCallback)
				PPCCoreCallback(g_channels[chan].abortCallback, chan, FromNfcResult(error), context);
		}

		void OnRead(uint32 chan, sint32 error, nfc::NFCUid* uid, uint32 readOnly, uint32 dataSize, void* data, uint32 lockedSize, void* locked, void* context)
		{
			if (chan >= kChannels)
				return;
			StackAllocator<GameTagData> result;
			StackAllocator<uint8, kImageSize> rwArea;
			StackAllocator<uint8, kImageSize> roArea;
			memset(result.GetPointer(), 0, sizeof(GameTagData));
			memset(rwArea.GetPointer(), 0, kImageSize);
			memset(roArea.GetPointer(), 0, kImageSize);
			result->readOnly = (uint8)readOnly;
			if (uid)
				result->uid = *uid;

			sint32 r = FromNfcResult(error);
			if (r == 0 && !uid)
				r = kErrUid;
			if (r == 0)
			{
				uint8 image[kImageSize];
				uint8 plain[kImageSize];
				ParsedTag tag;
				if (!JoinImage(image, data, dataSize, locked, lockedSize))
					r = kErrSize;
				else
					r = OpenTag(image, *uid, lockedSize, plain, tag);
				if (r == 0)
				{
					memcpy(rwArea.GetPointer(), plain + (uint32)tag.rw.offset, tag.rw.size);
					memcpy(roArea.GetPointer(), plain + (uint32)tag.ro.offset, tag.ro.size);
					result->formatVersion = tag.info.formatVersion;
					result->rw.data = rwArea.GetPointer();
					result->rw.size = tag.rw.size;
					result->rw.makerCode = tag.rw.makerCode;
					result->rw.identifyCode = tag.rw.identifyCode;
					result->ro.data = roArea.GetPointer();
					result->ro.size = tag.ro.size;
					result->ro.makerCode = tag.ro.makerCode;
					result->ro.identifyCode = tag.ro.identifyCode;
				}
			}
			cemuLog_log(LogType::NTAG, "ntag: read on channel {} finished with {}", chan, r);
			PPCCoreCallback(g_channels[chan].readCallback, chan, r, result.GetPointer(), context);
		}

		void OnWritten(uint32 chan, sint32 error, void* context)
		{
			if (chan < kChannels)
				PPCCoreCallback(g_channels[chan].writeCallback, chan, FromNfcResult(error), context);
		}

		// Builds the bytes to write back: NOFT header with the next write count
		// and the new RW area, encrypted.
		sint32 BuildWriteBack(uint8* plain, ParsedTag& tag, const PendingWrite& pending, uint8* out, uint32& outSize)
		{
			const uint32 rwOffset = tag.rw.offset;
			const uint32 rwSize = tag.rw.size;
			if (pending.size > rwSize)
				return kErrSize;
			// unused space in the RW area is filled with noise, as the system does
			for (uint32 i = pending.size; i < rwSize; i++)
				plain[rwOffset + i] = (uint8)(rand() & 0xFF);
			memcpy(plain + rwOffset, pending.data, pending.size);

			// the counter's top bit is kept, the low 15 bits wrap to 0
			uint16 count = ReadU16(tag.noft.writeCount);
			count = (count & 0x7FFF) == 0x7FFF ? (uint16)(count & 0x8000) : (uint16)(count + 1);
			WriteU16(tag.noft.writeCount, count);
			memcpy(plain + kNoftOffset, &tag.noft, sizeof(NoftHeader));

			uint8 encrypted[kImageSize];
			const sint32 r = Encrypt(plain, encrypted);
			if (r < 0)
				return r;
			outSize = std::min(rwSize + kWriteBackPrefix, kImageSize);
			memcpy(out, encrypted, outSize);
			return 0;
		}

		void OnReadBeforeWrite(uint32 chan, sint32 error, nfc::NFCUid* uid, uint32 readOnly, uint32 dataSize, void* data, uint32 lockedSize, void* locked, void* context)
		{
			if (chan >= kChannels)
				return;
			Channel& ch = g_channels[chan];
			sint32 r = FromNfcResult(error);
			if (r == 0)
			{
				uint8 image[kImageSize];
				uint8 plain[kImageSize];
				ParsedTag tag;
				if (!uid || !JoinImage(image, data, dataSize, locked, lockedSize) || OpenTag(image, *uid, lockedSize, plain, tag) < 0)
				{
					cemuLog_log(LogType::Force, "ntag: the tag to be written could not be read");
					PPCCoreCallback(ch.writeCallback, chan, kErrWriteFailed, context);
					return;
				}
				uint8 writeBack[kImageSize];
				uint32 writeSize = 0;
				if (BuildWriteBack(plain, tag, ch.write, writeBack, writeSize) < 0)
				{
					cemuLog_log(LogType::Force, "ntag: could not prepare the data to write");
					PPCCoreCallback(ch.writeCallback, chan, kErrWriteFailed, context);
					return;
				}
				const MPTR onWritten = makeCallableExport<OnWritten>();
				r = nfc::NFCWrite(chan, kWriteTimeout, &ch.write.uid, &ch.write.uidMask, writeSize, writeBack, onWritten, context);
				if (r >= 0)
					return; // OnWritten reports the result
				r = FromNfcResult(r);
			}
			PPCCoreCallback(ch.writeCallback, chan, r, context);
		}
	}

	// ---- exports -------------------------------------------------------------------

	sint32 NTAGInit(uint32 chan)
	{
		return NTAGInitEx(chan);
	}

	sint32 NTAGInitEx(uint32 chan)
	{
		return FromNfcResult(nfc::NFCInitEx(chan, 1));
	}

	sint32 NTAGShutdown(uint32 chan)
	{
		const sint32 r = nfc::NFCShutdown(chan);
		if (g_cryptOpen)
		{
			coreinit::IOS_Close(g_cryptHandle);
			g_cryptOpen = false;
		}
		if (chan < kChannels)
		{
			Channel& ch = g_channels[chan];
			ch.detectCallback = ch.abortCallback = ch.readCallback = ch.writeCallback = MPTR_NULL;
		}
		return FromNfcResult(r);
	}

	bool NTAGIsInit(uint32 chan)
	{
		return nfc::NFCIsInit(chan);
	}

	void NTAGProc(uint32 chan)
	{
		nfc::NFCProc(chan);
	}

	void NTAGSetFormatSettings(NTAGFormatSettings* settings)
	{
		g_format.version = settings->version;
		g_format.makerCode = settings->makerCode;
		g_format.identifyCode = settings->identifyCode;
	}

	void NTAGSetTagDetectCallback(uint32 chan, MPTR callback, void* context)
	{
		if (chan >= kChannels)
			return;
		g_channels[chan].detectCallback = callback;
		nfc::NFCSetTagDetectCallback(chan, makeCallableExport<OnTagDetected>(), context);
	}

	sint32 NTAGAbort(uint32 chan, MPTR callback, void* context)
	{
		if (chan >= kChannels)
			return NTAG_RESULT_INVALID;
		g_channels[chan].abortCallback = callback;
		return FromNfcResult(nfc::NFCAbort(chan, makeCallableExport<OnAborted>(), context));
	}

	sint32 NTAGRead(uint32 chan, uint32 timeout, nfc::NFCUid* uid, nfc::NFCUid* uidMask, MPTR callback, void* context)
	{
		if (chan >= kChannels)
			return NTAG_RESULT_INVALID;
		g_channels[chan].readCallback = callback;
		// no uid given: accept any tag (mask 0)
		nfc::NFCUid wanted{};
		nfc::NFCUid mask{};
		if (uid && uidMask)
		{
			wanted = *uid;
			mask = *uidMask;
		}
		return FromNfcResult(nfc::NFCRead(chan, timeout, &wanted, &mask, makeCallableExport<OnRead>(), context));
	}

	sint32 NTAGWrite(uint32 chan, uint32 timeout, nfc::NFCUid* uid, uint32 rwSize, void* rwData, MPTR callback, void* context)
	{
		if (chan >= kChannels || rwSize >= kImageSize)
			return NTAG_RESULT_INVALID;
		Channel& ch = g_channels[chan];
		ch.writeCallback = callback;
		if (uid)
			ch.write.uid = *uid;
		memset(ch.write.uidMask.uid, 0xFF, sizeof(ch.write.uidMask.uid));
		ch.write.size = rwSize;
		memcpy(ch.write.data, rwData, rwSize);
		// read the tag first; OnReadBeforeWrite does the actual write
		return FromNfcResult(nfc::NFCRead(chan, timeout, &ch.write.uid, &ch.write.uidMask, makeCallableExport<OnReadBeforeWrite>(), context));
	}

	sint32 NTAGFormat(uint32 chan, uint32 timeout, nfc::NFCUid* uid, uint32 rwSize, void* rwData, MPTR callback, void* context)
	{
		cemuLog_log(LogType::Force, "ntag: formatting tags is not supported");
		return NTAG_RESULT_INVALID;
	}

	namespace
	{
		class NtagLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "ntag"; }

			void RPLMapped() override
			{
				cafeExportRegister("ntag", NTAGInit, LogType::NTAG);
				cafeExportRegister("ntag", NTAGInitEx, LogType::NTAG);
				cafeExportRegister("ntag", NTAGShutdown, LogType::NTAG);
				cafeExportRegister("ntag", NTAGIsInit, LogType::Placeholder); // polled every frame
				cafeExportRegister("ntag", NTAGProc, LogType::Placeholder);   // polled every frame
				cafeExportRegister("ntag", NTAGSetFormatSettings, LogType::NTAG);
				cafeExportRegister("ntag", NTAGSetTagDetectCallback, LogType::NTAG);
				cafeExportRegister("ntag", NTAGAbort, LogType::NTAG);
				cafeExportRegister("ntag", NTAGRead, LogType::NTAG);
				cafeExportRegister("ntag", NTAGWrite, LogType::NTAG);
				cafeExportRegister("ntag", NTAGFormat, LogType::NTAG);
			}
		};

		NtagLibrary g_ntagLibrary;
	}

	COSModule* GetModule()
	{
		return &g_ntagLibrary;
	}
}
