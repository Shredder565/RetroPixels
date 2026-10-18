// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_idbe fetches a title's icon database entry (IDBE) from the online
// service and decrypts it. Only system apps (Daily Log, Download Manager) use
// it, to draw icons of titles that are not installed.
//
// Encrypted IDBE file layout (0x12082 bytes):
//   +0x00  u8   format version (must be 0)
//   +0x01  u8   key index (0..3)
//   +0x02  AES-128-CBC encrypted { SHA-256 of icon data (0x20), icon data (0x12060) }
// The decrypted icon data holds, among other things, a 256x256 TGA image at
// offset 0x2030.
//
// DecryptIconFile always reports success; if the file cannot be decrypted or
// fails its hash check the caller gets a blank 256x256 TGA header instead, so
// the app shows an empty icon rather than an error.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cemu/napi/napi.h"
#include "config/ActiveSettings.h"
#include "util/crypto/aes128.h"
#include "openssl/sha.h"
#include "nn_idbe.h"

namespace nn::idbe
{
	namespace
	{
		constexpr size_t kIconDataSize = 0x12060;
		constexpr size_t kHashSize = 0x20;
		constexpr size_t kHeaderSize = 2;
		constexpr size_t kEncryptedFileSize = kHeaderSize + kHashSize + kIconDataSize;
		constexpr size_t kTgaOffset = 0x2030;
		static_assert(kEncryptedFileSize == 0x12082);

		constexpr uint8 kIdbeKeys[4][16] = {
			{ 0x4A, 0xB9, 0xA4, 0x0E, 0x14, 0x69, 0x75, 0xA8, 0x4B, 0xB1, 0xB4, 0xF3, 0xEC, 0xEF, 0xC4, 0x7B },
			{ 0x90, 0xA0, 0xBB, 0x1E, 0x0E, 0x86, 0x4A, 0xE8, 0x7D, 0x13, 0xA6, 0xA0, 0x3D, 0x28, 0xC9, 0xB8 },
			{ 0xFF, 0xBB, 0x57, 0xC1, 0x4E, 0x98, 0xEC, 0x69, 0x75, 0xB3, 0x84, 0xFC, 0xF4, 0x07, 0x86, 0xB5 },
			{ 0x80, 0x92, 0x37, 0x99, 0xB4, 0x1F, 0x36, 0xA6, 0xA7, 0x5F, 0xB8, 0xB4, 0x8C, 0x95, 0xF6, 0x6F },
		};
		constexpr uint8 kIdbeIv[16] = { 0xA4, 0x69, 0x87, 0xAE, 0x47, 0xD8, 0x2B, 0xB4, 0xFA, 0x8A, 0xBC, 0x04, 0x50, 0x28, 0x5F, 0xA4 };

		// Fetches the encrypted IDBE of titleId into out (kEncryptedFileSize
		// bytes); zero-fills out when the server has no usable file.
		void FetchEncryptedIcon(uint64 titleId, uint8* out)
		{
			std::vector<uint8> file = NAPI::IDBE_RequestRawEncrypted(ActiveSettings::GetNetworkService(), titleId);
			if (file.size() == kEncryptedFileSize)
			{
				memcpy(out, file.data(), kEncryptedFileSize);
				return;
			}
			cemuLog_log(LogType::Force, "nn_idbe: no icon available for title {:016x}", titleId);
			memset(out, 0, kEncryptedFileSize);
		}

		// bool DownloadIconFile(void* out, u64 titleId, u16, bool)
		// Blocks the calling game thread until the download is done, without
		// blocking the emulated CPU: the network request runs on a host thread
		// and wakes the game thread through an OS event.
		uint32 DownloadIconFile(uint8* out, uint64 titleId, uint32 arg2, uint32 arg3)
		{
			StackAllocator<coreinit::OSEvent> done;
			coreinit::OSInitEvent(&done, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			coreinit::OSEvent* doneEvent = &done;
			std::thread worker([titleId, out, doneEvent]() {
				FetchEncryptedIcon(titleId, out);
				coreinit::OSSignalEvent(doneEvent);
			});
			coreinit::OSWaitEvent(doneEvent);
			worker.join();
			return 1;
		}

		bool TryDecrypt(const uint8* encrypted, uint8* iconOut)
		{
			const uint8 version = encrypted[0];
			const uint8 keyIndex = encrypted[1];
			if (version != 0)
			{
				cemuLog_log(LogType::Force, "nn_idbe: unsupported IDBE format version {}", version);
				return false;
			}
			if (keyIndex >= std::size(kIdbeKeys))
			{
				cemuLog_log(LogType::Force, "nn_idbe: IDBE key index {} out of range", keyIndex);
				return false;
			}
			// hash and icon data are one CBC stream; decrypt both together
			std::vector<uint8> cipher(encrypted + kHeaderSize, encrypted + kEncryptedFileSize);
			std::vector<uint8> plain(cipher.size());
			AES128_CBC_decrypt(plain.data(), cipher.data(), (uint32)cipher.size(), kIdbeKeys[keyIndex], kIdbeIv);

			// the decrypted data is handed out even if the hash check fails
			memcpy(iconOut, plain.data() + kHashSize, kIconDataSize);

			uint8 digest[SHA256_DIGEST_LENGTH];
			SHA256(iconOut, kIconDataSize, digest);
			if (memcmp(digest, plain.data(), kHashSize) != 0)
			{
				cemuLog_logDebug(LogType::Force, "nn_idbe: icon hash mismatch");
				return false;
			}
			return true;
		}

		// Blank, uncompressed 32-bit 256x256 TGA header (little endian fields).
		void WriteBlankTgaHeader(uint8* tga)
		{
			uint8 header[18] = {};
			header[2] = 2;                       // uncompressed true-colour
			header[12] = 0x00; header[13] = 0x01; // width 256
			header[14] = 0x00; header[15] = 0x01; // height 256
			header[16] = 32;                     // bits per pixel
			header[17] = 8;                      // 8 alpha bits
			memcpy(tga, header, sizeof(header));
		}

		// bool DecryptIconFile(void* out, const void* in)
		uint32 DecryptIconFile(uint8* iconOut, const uint8* encrypted)
		{
			if (!TryDecrypt(encrypted, iconOut))
				WriteBlankTgaHeader(iconOut + kTgaOffset);
			return 1;
		}

		class IdbeLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_idbe"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(DownloadIconFile, "nn_idbe", "DownloadIconFile__Q2_2nn4idbeFPvULUsb", LogType::Placeholder);
				cafeExportRegisterFunc(DecryptIconFile, "nn_idbe", "DecryptIconFile__Q2_2nn4idbeFPvPCv", LogType::Placeholder);
			}
		};

		IdbeLibrary g_idbeLibrary;
	}

	COSModule* GetModule()
	{
		return &g_idbeLibrary;
	}
}
