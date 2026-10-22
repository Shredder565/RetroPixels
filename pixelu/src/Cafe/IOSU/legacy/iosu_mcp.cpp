// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// /dev/mcp: title management. Title enumeration is answered from the
// emulator's title list; the IPC device itself only manages client handles.
#include "iosu_ioctl.h"
#include "iosu_mcp.h"
#include "Cafe/OS/libs/nn_common.h"
#include "util/tinyxml2/tinyxml2.h"
#include "config/CemuConfig.h"
#include "Cafe/TitleList/GameInfo.h"
#include "util/helpers/helpers.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/IOSU/iosu_ipc_common.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"

using namespace iosu::kernel;

namespace iosu
{
	namespace
	{
		std::recursive_mutex g_titleListLock;
		bool g_storageMounted = false;
		bool g_legacyThreadStarted = false;

		void FillTitleInfo(MCPTitleInfo& out, const TitleInfo& title)
		{
			const uint64 titleId = title.GetAppTitleId();
			out.titleIdHigh = (uint32)(titleId >> 32);
			out.titleIdLow = (uint32)titleId;
			out.titleVersion = title.GetAppTitleVersion();
			out.appGroup = title.GetAppGroup();
			out.appType = title.GetAppType();
			strcpy(out.appPath, CafeSystem::GetMlcStoragePath(titleId).c_str());
			strcpy((char*)out.deviceName, "mlc");
			out.osVersion = 0; // not tracked
			out.sdkVersion = title.GetAppSDKVersion();
		}

		// Walks the title list once and reports each title id at most once.
		// 'out' may be null to only count. Returns the number of matching titles (up to 'limit').
		uint32 CollectTitles(MCPTitleInfo* out, uint32 limit, const std::function<bool(const TitleInfo&)>& accept)
		{
			std::lock_guard lock(g_titleListLock);
			CafeTitleList::WaitForMandatoryScan();
			std::set<TitleId> seen;
			uint32 count = 0;
			for (TitleInfo* title : CafeTitleList::AcquireInternalList())
			{
				if (count == limit)
					break;
				const uint64 titleId = title->GetAppTitleId();
				if (seen.count(titleId) || !accept(*title))
					continue;
				seen.insert(titleId);
				if (out)
					FillTitleInfo(out[count], *title);
				count++;
			}
			CafeTitleList::ReleaseInternalList();
			// titles become reachable through the file system after the first listing
			if (!g_storageMounted)
			{
				CafeSystem::MlcStorageMountAllTitles();
				g_storageMounted = true;
			}
			return count;
		}

		uint32 EntriesThatFit(uint32 bufferSize)
		{
			return bufferSize / sizeof(MCPTitleInfo);
		}

		void HandleEmulatorRequest(iosuMcpCemuRequest_t& req)
		{
			auto& list = req.titleListRequest;
			MCPTitleInfo* out = list.titleList.GetPtr();
			const uint32 limit = EntriesThatFit(list.titleListBufferSize);
			switch (req.requestCode)
			{
			case IOSU_MCP_GET_TITLE_LIST:
				list.titleCount = CollectTitles(out, limit, [](const TitleInfo&) { return true; });
				req.returnCode = 0;
				break;
			case IOSU_MCP_GET_TITLE_LIST_BY_APP_TYPE:
			{
				const uint32 appType = list.appType;
				list.titleCount = CollectTitles(out, limit, [appType](const TitleInfo& t) { return t.GetAppType() == appType; });
				req.returnCode = 0;
				break;
			}
			case IOSU_MCP_GET_TITLE_INFO:
			{
				const uint64 titleId = list.titleId;
				list.titleCount = CollectTitles(out, limit, [titleId](const TitleInfo& t) { return t.GetAppTitleId() == titleId; });
				req.returnCode = 0;
				break;
			}
			case IOSU_MCP_GET_TITLE_COUNT:
			{
				const uint32 count = CollectTitles(nullptr, 0xFFFFFFFF, [](const TitleInfo&) { return true; });
				req.returnCode = count;
				list.titleCount = count;
				break;
			}
			default:
				cemu_assert(false);
				break;
			}
		}

		void LegacyRequestThread()
		{
			SetThreadName("iosuMcp_thread");
			while (true)
			{
				ioQueueEntry_t* entry = iosuIoctl_getNextWithWait(IOS_DEVICE_MCP);
				cemu_assert(entry->request == IOSU_MCP_REQUEST_CEMU);
				HandleEmulatorRequest(*(iosuMcpCemuRequest_t*)entry->bufferVectors[0].buffer.GetPtr());
				iosuIoctl_completeRequest(entry, 0);
			}
		}
	}

	sint32 mcpGetTitleList(MCPTitleInfo* titleList, uint32 titleListBufferSize, uint32be* titleCount)
	{
		*titleCount = CollectTitles(titleList, EntriesThatFit(titleListBufferSize), [](const TitleInfo&) { return true; });
		return 0;
	}

	sint32 mcpGetTitleCount()
	{
		return CollectTitles(nullptr, 0xFFFFFFFF, [](const TitleInfo&) { return true; });
	}

	void iosuMcp_init()
	{
		if (g_legacyThreadStarted)
			return;
		std::thread(LegacyRequestThread).detach();
		g_legacyThreadStarted = true;
	}

	namespace mcp
	{
		namespace
		{
			IOSMsgQueueId g_queue;
			SysAllocator<iosu::kernel::IOSMessage, 352> g_queueStorage;
			std::thread g_thread;
			std::array<bool, 256> g_clientOpen{};

			void ServiceLoop()
			{
				SetThreadName("IOSU-MCP");
				while (true)
				{
					IOSMessage msg;
					const IOS_ERROR err = IOS_ReceiveMessage(g_queue, &msg, 0);
					cemu_assert(!IOS_ResultIsError(err));
					if (msg == 0)
						return; // shutdown request
					IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(msg).GetPtr();
					const uint32 handle = cmd->devHandle;
					switch ((IPCCommandId)cmd->cmdId)
					{
					case IPCCommandId::IOS_OPEN:
					{
						IOS_ERROR reply = IOS_ERROR_MAXIMUM_REACHED;
						for (size_t i = 0; i < g_clientOpen.size(); i++)
						{
							if (g_clientOpen[i])
								continue;
							g_clientOpen[i] = true;
							reply = (IOS_ERROR)i;
							break;
						}
						IOS_ResourceReply(cmd, reply);
						break;
					}
					case IPCCommandId::IOS_CLOSE:
						cemu_assert(handle < g_clientOpen.size());
						g_clientOpen[handle] = false;
						IOS_ResourceReply(cmd, IOS_ERROR_OK);
						break;
					case IPCCommandId::IOS_IOCTL:
						// no ioctl requests are implemented
						cemu_assert(handle < g_clientOpen.size());
						cemu_assert(g_clientOpen[handle]);
						IOS_ResourceReply(cmd, (IOS_ERROR)0x80000000);
						break;
					case IPCCommandId::IOS_IOCTLV:
						cemu_assert_unimplemented();
						IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
						break;
					default:
						cemuLog_log(LogType::Force, "/dev/mcp: Unsupported IPC cmdId");
						cemu_assert_suspicious();
						IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
						break;
					}
				}
			}
		}

		void Init()
		{
			g_clientOpen.fill(false);
			g_queue = IOS_CreateMessageQueue(g_queueStorage.GetPtr(), g_queueStorage.GetCount());
			const IOS_ERROR err = IOS_RegisterResourceManager("/dev/mcp", g_queue);
			IOS_DeviceAssociateId("/dev/mcp", 11);
			cemu_assert(!IOS_ResultIsError(err));
			g_thread = std::thread(ServiceLoop);
		}

		void Shutdown()
		{
			IOS_SendMessage(g_queue, 0, 0);
			g_thread.join();
		}
	};
}
