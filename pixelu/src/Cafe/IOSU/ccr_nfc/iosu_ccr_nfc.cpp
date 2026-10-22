// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// /dev/ccr_nfc: encrypts and decrypts NTAG data areas (locked secret and
// unfixed infos). Per tag keys are derived with an HMAC-SHA256 counter KDF
// over a salt stored on the tag; the areas are AES-128-CTR encrypted and
// authenticated with HMAC-SHA256.
#include "iosu_ccr_nfc.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "util/crypto/aes128.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace iosu
{
	namespace ccr_nfc
	{
		namespace
		{
			// fixed console keys
			constexpr uint8 kSaltKey[16] = { 0xC1, 0x2B, 0x07, 0x10, 0xD7, 0x2C, 0xEB, 0x5D, 0x43, 0x49, 0xB7, 0x43, 0xE3, 0xCA, 0xD2, 0x24 };
			constexpr uint8 kSaltIV[16] = { 0x4F, 0xD3, 0x9A, 0x6E, 0x79, 0xFC, 0xEA, 0xAD, 0x99, 0x90, 0x4D, 0xB8, 0xEE, 0x38, 0xE9, 0xDB };

			// input seeds of the two key derivations
			constexpr uint8 kUnfixedInfosSeed[16] = { 0x00, 0x00, 0xDB, 0x4B, 0x9E, 0x3F, 0x45, 0x27, 0x8F, 0x39, 0x7E, 0xFF, 0x9B, 0x4F, 0xB9, 0x93 };
			constexpr uint8 kLockedSecretSeed[16] = { 0xFD, 0xC8, 0xA0, 0x76, 0x94, 0xB8, 0x9E, 0x4C, 0x47, 0xD3, 0x7D, 0xE8, 0xCE, 0x5C, 0x74, 0xC1 };
			constexpr uint8 kUnfixedInfosHmacKey[16] = { 0x1D, 0x16, 0x4B, 0x37, 0x5B, 0x72, 0xA5, 0x57, 0x28, 0xB9, 0x1D, 0x64, 0xB6, 0xA3, 0xC2, 0x05 };
			constexpr uint8 kLockedSecretHmacKey[16] = { 0x7F, 0x75, 0x2D, 0x28, 0x73, 0xA2, 0x00, 0x17, 0xFE, 0xF8, 0x5C, 0x05, 0x75, 0x90, 0x4B, 0x6D };

			// KDF labels, 14 bytes each (text plus padding zeros)
			constexpr size_t kLabelSize = 14;
			constexpr char kUnfixedInfosLabel[kLabelSize + 1] = "unfixed infos\0";
			constexpr char kLockedSecretLabel[kLabelSize + 1] = "locked secret\0";

			constexpr uint32 kHmacSize = 0x20;

			struct AreaKeys
			{
				uint8 aesKey[16];
				uint8 ctrNonce[16];
				uint8 hmacKey[16];
			};

			struct TagKeys
			{
				AreaKeys lockedSecret;
				AreaKeys unfixedInfos;
			};

			bool Hmac(const uint8* key, size_t keySize, const uint8* data, size_t size, uint8* out)
			{
				unsigned int outLen = kHmacSize;
				return HMAC(EVP_sha256(), key, (int)keySize, data, size, out, &outLen) != nullptr;
			}

			// copies 'size' bytes from src to dst and applies AES-128-CTR in place
			void CtrCopy(const uint8* key, const uint8* nonce, const uint8* src, uint8* dst, uint32 size)
			{
				uint8 counter[16];
				memcpy(counter, nonce, sizeof(counter));
				memcpy(dst, src, size);
				AES128CTR_transform(dst, (sint32)size, const_cast<uint8*>(key), counter);
			}

			// Counter mode HMAC KDF. Each 32 byte output block is
			// HMAC(key, be16 blockIndex || label || seed[0x40]).
			bool DeriveAreaKeys(const uint8* hmacKey, const char* label, const uint8 (&seed)[0x40], AreaKeys& out)
			{
				uint8 message[2 + kLabelSize + sizeof(seed)];
				memcpy(message + 2, label, kLabelSize);
				memcpy(message + 2 + kLabelSize, seed, sizeof(seed));
				uint8 material[0x40];
				for (uint32 block = 0; block < sizeof(material) / kHmacSize; block++)
				{
					message[0] = (uint8)(block >> 8);
					message[1] = (uint8)block;
					if (!Hmac(hmacKey, 16, message, sizeof(message), material + block * kHmacSize))
						return false;
				}
				memcpy(out.aesKey, material + 0x00, 16);
				memcpy(out.ctrNonce, material + 0x10, 16);
				memcpy(out.hmacKey, material + 0x20, 16);
				return true;
			}

			// the tag identity part of the KDF seed (16 bytes at seed+0x10)
			bool FillTagIdentity(const CCRNFCCryptData& tag, uint8* dst)
			{
				const uint8* uid = tag.data + tag.uuidOffset;
				if (tag.version == 0)
				{
					memcpy(dst, uid, 16); // format info
					return true;
				}
				if (tag.version == 2)
				{
					memcpy(dst, uid, 8); // 7 byte UID plus check byte, repeated
					memcpy(dst + 8, uid, 8);
					return true;
				}
				return false;
			}

			sint32 DeriveTagKeys(const CCRNFCCryptData& tag, const uint8* salt, TagKeys& keys)
			{
				uint8 seed[0x40] = {};
				memcpy(seed, kLockedSecretSeed, 16);
				if (!FillTagIdentity(tag, seed + 0x10))
					return CCR_NFC_ERROR;
				memcpy(seed + 0x20, salt, 0x20);
				if (!DeriveAreaKeys(kLockedSecretHmacKey, kLockedSecretLabel, seed, keys.lockedSecret))
					return CCR_NFC_ERROR;

				memset(seed, 0, sizeof(seed));
				memcpy(seed, tag.data + tag.seedOffset, 2); // first two bytes come from the tag
				memcpy(seed + 2, kUnfixedInfosSeed + 2, 14);
				if (!FillTagIdentity(tag, seed + 0x10))
					return CCR_NFC_ERROR;
				memcpy(seed + 0x20, salt, 0x20);
				if (!DeriveAreaKeys(kUnfixedInfosHmacKey, kUnfixedInfosLabel, seed, keys.unfixedInfos))
					return CCR_NFC_ERROR;
				return 0;
			}

			// HMAC over the plaintext that follows a stored HMAC. 'skip' is the
			// distance from the HMAC position to the start of the covered data.
			bool AreaHmac(const AreaKeys& keys, const CCRNFCCryptData& in, const CCRNFCCryptData& plain, uint32 hmacOffset, uint32 skip, uint8* out)
			{
				return Hmac(keys.hmacKey, sizeof(keys.hmacKey), plain.data + hmacOffset + skip, (in.dataSize - hmacOffset) - skip, out);
			}

			// The unfixed infos HMAC of version 2 tags starts one byte later
			uint32 UnfixedInfosSkip(const CCRNFCCryptData& tag)
			{
				return tag.version == 0 ? 0x20 : 0x21;
			}

			// 'out' already holds a copy of 'in'
			sint32 Decrypt(const CCRNFCCryptData& in, CCRNFCCryptData& out, const TagKeys& keys)
			{
				// version 2 tags keep the locked secret in plain text
				if (in.version == 0)
					CtrCopy(keys.lockedSecret.aesKey, keys.lockedSecret.ctrNonce, in.data + in.lockedSecretOffset, out.data + in.lockedSecretOffset, in.lockedSecretSize);
				CtrCopy(keys.unfixedInfos.aesKey, keys.unfixedInfos.ctrNonce, in.data + in.unfixedInfosOffset, out.data + in.unfixedInfosOffset, in.unfixedInfosSize);

				uint8 mac[kHmacSize];
				if (!AreaHmac(keys.lockedSecret, in, out, in.lockedSecretHmacOffset, 0x20, mac))
					return CCR_NFC_ERROR;
				if (memcmp(in.data + in.lockedSecretHmacOffset, mac, kHmacSize) != 0)
					return CCR_NFC_INVALID_LOCKED_SECRET;

				if (!AreaHmac(keys.unfixedInfos, in, out, in.unfixedInfosHmacOffset, UnfixedInfosSkip(in), mac))
					return CCR_NFC_ERROR;
				if (memcmp(in.data + in.unfixedInfosHmacOffset, mac, kHmacSize) != 0)
					return CCR_NFC_INVALID_UNFIXED_INFOS;
				return 0;
			}

			// 'out' already holds a copy of 'in' (plain text)
			sint32 Encrypt(const CCRNFCCryptData& in, CCRNFCCryptData& out, const TagKeys& keys)
			{
				uint8 mac[kHmacSize];
				if (!AreaHmac(keys.lockedSecret, in, out, in.lockedSecretHmacOffset, 0x20, mac))
					return CCR_NFC_ERROR;
				if (memcmp(in.data + in.lockedSecretHmacOffset, mac, kHmacSize) != 0)
					return CCR_NFC_INVALID_LOCKED_SECRET;

				// refresh the unfixed infos HMAC, it is written straight into the output
				if (!AreaHmac(keys.unfixedInfos, in, out, in.unfixedInfosHmacOffset, UnfixedInfosSkip(in), out.data + in.unfixedInfosHmacOffset))
					return CCR_NFC_ERROR;

				if (in.version == 0)
					CtrCopy(keys.lockedSecret.aesKey, keys.lockedSecret.ctrNonce, in.data + in.lockedSecretOffset, out.data + in.lockedSecretOffset, in.lockedSecretSize);
				CtrCopy(keys.unfixedInfos.aesKey, keys.unfixedInfos.ctrNonce, in.data + in.unfixedInfosOffset, out.data + in.unfixedInfosOffset, in.unfixedInfosSize);
				return 0;
			}

			sint32 Transform(const CCRNFCCryptData& in, CCRNFCCryptData& out, bool decrypt)
			{
				uint8 salt[0x20];
				CtrCopy(kSaltKey, kSaltIV, in.data + in.keyGenSaltOffset, salt, sizeof(salt));
				TagKeys keys;
				if (sint32 r = DeriveTagKeys(in, salt, keys); r != 0)
					return r;
				return decrypt ? Decrypt(in, out, keys) : Encrypt(in, out, keys);
			}

			// size must match exactly; offsets are only bounds checked for the input block
			sint32 CheckBlock(const CCRNFCCryptData* block, uint32 size, bool checkLayout)
			{
				if (!block || size != sizeof(CCRNFCCryptData))
					return CCR_NFC_ERROR;
				if (!checkLayout)
					return 0;
				uint32 limit;
				if (block->version == 0)
					limit = 0x1C9;
				else if (block->version == 2)
					limit = 0x21D;
				else
					return CCR_NFC_ERROR;
				const uint32 fields[] = { block->unfixedInfosHmacOffset, block->unfixedInfosOffset, block->lockedSecretHmacOffset,
										  block->lockedSecretOffset, block->lockedSecretSize, block->unfixedInfosSize };
				for (uint32 v : fields)
					if (v >= limit)
						return CCR_NFC_ERROR;
				return 0;
			}

			constexpr iosu::kernel::IOSMessage kQuitMessage = 0xF00DD00D;

			IOSMsgQueueId g_queue;
			SysAllocator<iosu::kernel::IOSMessage, 0x20> g_queueStorage;
			std::thread g_thread;
			bool g_running = false;

			sint32 HandleIoctl(const IPCCommandBody& cmd)
			{
				const uint32 request = cmd.args[0];
				auto* in = MEMPTR<CCRNFCCryptData>(cmd.args[1]).GetPtr();
				const uint32 inSize = cmd.args[2];
				auto* out = MEMPTR<CCRNFCCryptData>(cmd.args[3]).GetPtr();
				const uint32 outSize = cmd.args[4];
				if (sint32 r = CheckBlock(in, inSize, true); r != 0)
					return r;
				if (sint32 r = CheckBlock(out, outSize, false); r != 0)
					return r;
				memcpy(out, in, inSize);
				switch (request)
				{
				case 1:
					return Transform(*in, *out, false);
				case 2:
					return Transform(*in, *out, true);
				default:
					cemuLog_log(LogType::Force, "/dev/ccr_nfc: Unsupported IOCTL requestId");
					cemu_assert_suspicious();
					return IOS_ERROR_INVALID;
				}
			}

			void ServiceLoop()
			{
				while (true)
				{
					iosu::kernel::IOSMessage msg;
					const IOS_ERROR err = iosu::kernel::IOS_ReceiveMessage(g_queue, &msg, 0);
					cemu_assert(!IOS_ResultIsError(err));
					if (msg == kQuitMessage)
						return;
					IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(msg).GetPtr();
					switch ((IPCCommandId)cmd->cmdId)
					{
					case IPCCommandId::IOS_OPEN:
					case IPCCommandId::IOS_CLOSE:
						iosu::kernel::IOS_ResourceReply(cmd, IOS_ERROR_OK);
						break;
					case IPCCommandId::IOS_IOCTL:
						iosu::kernel::IOS_ResourceReply(cmd, (IOS_ERROR)HandleIoctl(*cmd));
						break;
					default:
						cemuLog_log(LogType::Force, "/dev/ccr_nfc: Unsupported IPC cmdId");
						cemu_assert_suspicious();
						iosu::kernel::IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
						break;
					}
				}
			}

			class CcrNfcModule : public ::IOSUModule
			{
			public:
				void SystemLaunch() override
				{
					g_queue = iosu::kernel::IOS_CreateMessageQueue(g_queueStorage.GetPtr(), g_queueStorage.GetCount());
					cemu_assert(!IOS_ResultIsError((IOS_ERROR)g_queue));
					const IOS_ERROR err = iosu::kernel::IOS_RegisterResourceManager("/dev/ccr_nfc", g_queue);
					cemu_assert(!IOS_ResultIsError(err));
					g_thread = std::thread(ServiceLoop);
					g_running = true;
				}

				void SystemExit() override
				{
					if (!g_running)
						return;
					iosu::kernel::IOS_SendMessage(g_queue, kQuitMessage, 0);
					g_thread.join();
					iosu::kernel::IOS_DestroyMessageQueue(g_queue);
					g_running = false;
				}
			};

			CcrNfcModule g_module;
		}

		IOSUModule* GetModule()
		{
			return &g_module;
		}
	}
}
