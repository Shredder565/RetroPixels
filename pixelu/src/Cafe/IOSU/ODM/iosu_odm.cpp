// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// /dev/odm: optical disc drive service. The emulated console never has a
// disc inserted, so the drive state query always reports "no disc".
#include <util/helpers/helpers.h>
#include "iosu_odm.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"
#include "util/helpers/Semaphore.h"
#include "../kernel/iosu_kernel.h"

namespace iosu
{
	namespace odm
	{
		using namespace iosu::kernel;

		namespace
		{
			constexpr const char* kDevicePath = "/dev/odm";

			// ioctl request ids
			constexpr uint32 kRequestGetDriveState = 4;
			constexpr uint32 kRequestUnknown5 = 5;

			// drive states as reported by the state query
			enum class DriveState : uint32
			{
				None = 0,
				Initial = 1,
				Authentication = 2,
				WaitForDiscReady = 3,
				CafeDisc = 4,
				RvlDisc = 5,
				CleaningDisc = 6,
				InvalidDisc = 8,
				DirtyDisc = 9,
				NoDisc = 10,
				InvalidDrive = 11,
				Fatal = 12,
				HardFatal = 13,
				Shutdown = 14,
			};

			std::thread g_thread;
			std::atomic_bool g_stopRequested{ false };
			std::atomic_bool g_started{ false };
			std::atomic_bool g_ready{ false };
			IOSMsgQueueId g_queue;
			SysAllocator<iosu::kernel::IOSMessage, 128> g_queueStorage;

			void HandleIoctl(IPCCommandBody* cmd)
			{
				const uint32 request = cmd->args[0];
				if (request == kRequestGetDriveState)
					*MEMPTR<uint32be>(cmd->args[3]).GetPtr() = (uint32)DriveState::NoDisc;
				else if (request != kRequestUnknown5) // request 5 has no visible output
					cemuLog_log(LogType::Force, "/dev/odm: unknown ioctl request {}", request);
				IOS_ResourceReply(cmd, IOS_ERROR_OK);
			}

			void ServiceLoop()
			{
				SetThreadName("ODMService");
				g_queue = IOS_CreateMessageQueue(g_queueStorage.GetPtr(), g_queueStorage.GetCount());
				cemu_assert(!IOS_ResultIsError((IOS_ERROR)g_queue));
				const IOS_ERROR regErr = IOS_RegisterResourceManager(kDevicePath, g_queue);
				cemu_assert(!IOS_ResultIsError(regErr));
				g_ready = true;
				while (true)
				{
					IOSMessage msg;
					const IOS_ERROR err = IOS_ReceiveMessage(g_queue, &msg, 0);
					cemu_assert(!IOS_ResultIsError(err));
					if (msg == 0)
					{
						cemu_assert_debug(g_stopRequested);
						break;
					}
					IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(msg).GetPtr();
					switch ((IPCCommandId)cmd->cmdId)
					{
					case IPCCommandId::IOS_OPEN:
						IOS_ResourceReply(cmd, (IOS_ERROR)1); // one shared client handle
						break;
					case IPCCommandId::IOS_CLOSE:
						IOS_ResourceReply(cmd, IOS_ERROR_OK);
						break;
					case IPCCommandId::IOS_IOCTL:
						HandleIoctl(cmd);
						break;
					case IPCCommandId::IOS_IOCTLV:
						cemuLog_log(LogType::Force, "{}: Received unsupported Ioctlv cmd", kDevicePath);
						IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
						break;
					default:
						cemuLog_log(LogType::Force, "{}: Unsupported cmdId", kDevicePath);
						cemu_assert_unimplemented();
						IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
						break;
					}
				}
				g_ready = false;
			}
		}

		void Initialize()
		{
			if (g_started.exchange(true))
				return;
			g_ready = false;
			g_stopRequested = false;
			g_thread = std::thread(ServiceLoop);
			while (!g_ready)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		void Shutdown()
		{
			if (!g_started.exchange(false))
				return;
			g_stopRequested = true;
			IOS_SendMessage(g_queue, 0, 0);
			g_thread.join();
		}
	}
}
