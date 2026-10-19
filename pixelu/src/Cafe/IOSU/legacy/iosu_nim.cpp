// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// /dev/nim: network install manager. Offers pending game updates (compared
// against the online version list) and icon database (IDBE) entries.
#include "iosu_nim.h"
#include "iosu_ioctl.h"
#include "iosu_act.h"
#include "iosu_mcp.h"
#include "util/crypto/aes128.h"
#include "curl/curl.h"
#include "openssl/bn.h"
#include "openssl/x509.h"
#include "openssl/ssl.h"
#include "util/helpers/helpers.h"
#include "Cemu/napi/napi.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Account/Account.h"
#include "config/ActiveSettings.h"

namespace iosu
{
	namespace nim
	{
		namespace
		{
			constexpr uint8 kPackageTypeUpdate = 1;

			struct PendingPackage
			{
				uint64 titleId;
				uint16 version;
				uint8 type;
			};

			struct CachedIcon
			{
				uint64 titleId;
				bool valid;
				NAPI::IDBEIconDataV0 data;
			};

			bool g_initialized = false;
			// the package list is built once; titles expect its size to stay stable
			std::vector<PendingPackage> g_packages;
			std::atomic_bool g_packagesReady{ false };
			bool g_scanStarted = false;
			std::unordered_map<uint64, uint32> g_latestVersions; // update title id -> version
			std::vector<CachedIcon> g_iconCache;

			// the Wii U menu skips update checks to keep its boot fast
			bool UpdatesDisabledForForegroundTitle()
			{
				switch (CafeSystem::GetForegroundTitleId())
				{
				case 0x0005001010040000:
				case 0x0005001010040100:
				case 0x0005001010040200:
					return true;
				default:
					return false;
				}
			}

			NAPI::AuthInfo MakeAuthInfo()
			{
				NAPI::AuthInfo auth;
				auth.country = NCrypto::GetCountryAsString(Account::GetCurrentAccount().GetCountry());
				auth.region = NCrypto::SEEPROM_GetRegion();
				return auth;
			}

			bool QueryVersionListVersion()
			{
				NAPI::AuthInfo auth = MakeAuthInfo();
				const auto result = NAPI::TAG_GetVersionListVersion(auth);
				if (!result.isValid)
					return false;
				if (result.fqdnURL.size() >= 256)
				{
					cemuLog_log(LogType::Force, "NIM: fqdn URL too long");
					return false;
				}
				return true;
			}

			bool DownloadVersionList()
			{
				g_latestVersions.clear();
				NAPI::AuthInfo auth = MakeAuthInfo();
				const auto listVersion = NAPI::TAG_GetVersionListVersion(auth);
				if (!listVersion.isValid)
					return false;
				const auto list = TAG_GetVersionList(auth, listVersion.fqdnURL, listVersion.version);
				if (!list.isValid)
					return false;
				for (const auto& [titleId, version] : list.titleVersionList)
					g_latestVersions.try_emplace(titleId, version);
				return true;
			}

			void CollectAvailableUpdates()
			{
				if (UpdatesDisabledForForegroundTitle())
				{
					cemuLog_logDebug(LogType::Force, "NIM: update check skipped for this title");
					g_packages.clear();
					return;
				}
				const sint32 count = mcpGetTitleCount();
				std::vector<MCPTitleInfo> titles(count);
				if (count > 0)
					memset(titles.data(), 0, count * sizeof(MCPTitleInfo));
				uint32be listed = count;
				if (mcpGetTitleList(titles.data(), count * sizeof(MCPTitleInfo), &listed) != 0)
				{
					cemuLog_log(LogType::Force, "IOSU: nim failed to acquire title list");
					return;
				}
				const uint32 numListed = listed;
				for (uint32 i = 0; i < numListed; i++)
				{
					const MCPTitleInfo& t = titles[i];
					if (t.titleIdHigh != 0x00050000)
						continue; // only base games receive updates
					const uint64 updateId = (0x0005000EULL << 32) | (uint32)t.titleIdLow;
					const auto it = g_latestVersions.find(updateId);
					if (it == g_latestVersions.end() || it->second <= (uint32)t.titleVersion)
						continue;
					g_packages.push_back(PendingPackage{ updateId, 0, kPackageTypeUpdate });
				}
			}

			void PackageScanThread()
			{
				while (!iosuAct_isAccountDataLoaded())
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				if (QueryVersionListVersion() && DownloadVersionList())
					CollectAvailableUpdates();
				g_packagesReady = true;
			}

			void WaitForPackageList()
			{
				if (!g_scanStarted)
				{
					cemuLog_log(LogType::Force, "IOSU: Starting nim background thread");
					std::thread(PackageScanThread).detach();
					g_scanStarted = true;
				}
				while (!g_packagesReady)
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
			}

			void FillPackageInfo(const uint64* titleIds, sint32 count, titlePackageInfo_t* out)
			{
				memset(out, 0, sizeof(titlePackageInfo_t) * count);
				if (UpdatesDisabledForForegroundTitle())
					return;
				for (sint32 i = 0; i < count; i++)
				{
					titlePackageInfo_t& info = out[i];
					// every entry reports the first requested title id, in host byte order
					info.titleId = _swapEndianU64(titleIds[0]);
					info.ukn0C = 1; // package is an update
					info.ukn0F = 1;
					info.ukn48 = 0; // 0 hides the progress bar
					info.ukn40 = (5u << 20) | (1u << 31) | 0x30000u;
				}
			}

			sint32 GetIcon(uint64 titleId, void* out)
			{
				// updates share the icon of their base title
				if ((uint32)(titleId >> 32) == 0x0005000E)
					titleId &= ~0xF00000000ULL;
				for (const CachedIcon& c : g_iconCache)
				{
					if (c.titleId != titleId)
						continue;
					if (c.valid)
						memcpy(out, &c.data, sizeof(NAPI::IDBEIconDataV0));
					else
						memset(out, 0, sizeof(NAPI::IDBEIconDataV0));
					return 0;
				}
				auto icon = NAPI::IDBE_Request(ActiveSettings::GetNetworkService(), titleId);
				if (!icon)
				{
					// failures are not cached so the download is retried next time
					memset(out, 0, sizeof(NAPI::IDBEIconDataV0));
					cemuLog_log(LogType::Force, "NIM: Unable to download IDBE icon");
					return 0;
				}
				g_iconCache.push_back(CachedIcon{ titleId, true, *icon });
				memcpy(out, &*icon, sizeof(NAPI::IDBEIconDataV0));
				return 0;
			}

			void HandleEmulatorRequest(iosuNimCemuRequest_t& req)
			{
				switch (req.requestCode)
				{
				case IOSU_NIM_GET_ICON_DATABASE_ENTRY:
					req.returnCode = GetIcon(req.titleId, req.ptr.GetPtr());
					break;
				case IOSU_NIM_GET_PACKAGE_COUNT:
					WaitForPackageList();
					req.resultU32.u32 = (uint32)g_packages.size();
					req.returnCode = 0;
					break;
				case IOSU_NIM_GET_PACKAGES_TITLEID:
				{
					WaitForPackageList();
					const uint32 maxCount = (uint32)req.maxCount;
					uint64* ids = (uint64*)req.ptr.GetPtr();
					memset(ids, 0, sizeof(uint64) * maxCount);
					uint32 n = 0;
					for (const PendingPackage& p : g_packages)
					{
						ids[n++] = _swapEndianU64(p.titleId);
						if (n >= maxCount)
							break;
					}
					req.returnCode = 0;
					break;
				}
				case IOSU_NIM_GET_PACKAGES_INFO:
					WaitForPackageList();
					FillPackageInfo((const uint64*)req.ptr.GetPtr(), req.maxCount, (titlePackageInfo_t*)req.ptr2.GetPtr());
					req.returnCode = 0;
					break;
				default:
					cemu_assert_unimplemented();
					break;
				}
			}

			void RequestThread()
			{
				SetThreadName("iosuNim_thread");
				while (true)
				{
					ioQueueEntry_t* entry = iosuIoctl_getNextWithWait(IOS_DEVICE_NIM);
					if (entry->request == IOSU_NIM_REQUEST_CEMU)
						HandleEmulatorRequest(*(iosuNimCemuRequest_t*)entry->bufferVectors[0].buffer.GetPtr());
					else
						cemu_assert_unimplemented();
					iosuIoctl_completeRequest(entry, 0);
				}
			}
		}

		void Initialize()
		{
			if (g_initialized)
				return;
			g_latestVersions.clear();
			g_packages.clear();
			g_packagesReady = false;
			g_scanStarted = false;
			std::thread(RequestThread).detach();
			g_initialized = true;
		}
	}
}
