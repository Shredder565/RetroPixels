// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "iosu_nn_service.h"
#include "../kernel/iosu_kernel.h"
#include "util/helpers/helpers.h"

using namespace iosu::kernel;

namespace iosu
{
	namespace nn
	{
		namespace
		{
			// reserved queue messages, request pointers are never this small
			constexpr IOSMessage kMsgQuit = 0;
			constexpr IOSMessage kMsgTimer = 1;

			template<typename TService>
			void LaunchThread(TService* service, std::atomic_bool& running, std::atomic_bool& requestStop, std::atomic_bool& initialized, std::thread& thread, void (TService::*entry)())
			{
				initialized = false;
				requestStop = false;
				thread = std::thread(entry, service);
				while (!initialized)
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			IOSMsgQueueId OpenServiceQueue(const std::string& devicePath, IOSMessage* storage, uint32 count)
			{
				const IOSMsgQueueId queue = IOS_CreateMessageQueue(storage, count);
				cemu_assert(!IOS_ResultIsError((IOS_ERROR)queue));
				const IOS_ERROR err = IOS_RegisterResourceManager(devicePath.c_str(), queue);
				cemu_assert(!IOS_ResultIsError(err));
				return queue;
			}
		}

		/* ---- IPCSimpleService ---- */

		void IPCSimpleService::Start()
		{
			if (m_isRunning.exchange(true))
				return;
			LaunchThread(this, m_isRunning, m_requestStop, m_threadInitialized, m_serviceThread, &IPCSimpleService::ServiceThread);
			StartService();
		}

		void IPCSimpleService::Stop()
		{
			if (!m_isRunning.exchange(false))
				return;
			m_requestStop = true;
			StopService();
			if (m_timerId != IOSInvalidTimerId)
				IOS_DestroyTimer(m_timerId);
			m_timerId = IOSInvalidTimerId;
			IOS_SendMessage(m_msgQueueId, kMsgQuit, 0);
			m_serviceThread.join();
		}

		IOSDevHandle IPCSimpleService::AllocateHandle()
		{
			// handles are positive, non zero and not in use
			while (m_nextHandle == 0 || m_clientObjects.count(m_nextHandle))
				m_nextHandle = (m_nextHandle + 1) & 0x7FFFFFFF;
			const IOSDevHandle handle = m_nextHandle;
			m_nextHandle = (m_nextHandle + 1) & 0x7FFFFFFF;
			return handle;
		}

		void* IPCSimpleService::LookupClient(IOSDevHandle handle) const
		{
			const auto it = m_clientObjects.find(handle);
			return it != m_clientObjects.end() ? it->second : nullptr;
		}

		void IPCSimpleService::ServiceThread()
		{
			const std::string threadName = GetThreadName();
			if (!threadName.empty())
				SetThreadName(threadName.c_str());
			m_msgQueueId = OpenServiceQueue(m_devicePath, m_queueStorage.GetPtr(), m_queueStorage.GetCount());
			m_threadInitialized = true;
			while (true)
			{
				IOSMessage msg;
				const IOS_ERROR err = IOS_ReceiveMessage(m_msgQueueId, &msg, 0);
				cemu_assert(!IOS_ResultIsError(err));
				if (msg == kMsgQuit)
				{
					cemu_assert_debug(m_requestStop);
					break;
				}
				if (msg == kMsgTimer)
				{
					TimerUpdate();
					continue;
				}
				IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(msg).GetPtr();
				switch ((IPCCommandId)cmd->cmdId)
				{
				case IPCCommandId::IOS_OPEN:
				{
					void* client = CreateClientObject();
					if (!client)
					{
						cemuLog_log(LogType::Force, "IPCSimpleService[{}]: Maximum handle count reached or handle rejected", m_devicePath);
						IOS_ResourceReply(cmd, IOS_ERROR_MAXIMUM_REACHED);
						break;
					}
					const IOSDevHandle handle = AllocateHandle();
					m_clientObjects[handle] = client;
					IOS_ResourceReply(cmd, (IOS_ERROR)handle);
					break;
				}
				case IPCCommandId::IOS_CLOSE:
				{
					const auto it = m_clientObjects.find(cmd->devHandle);
					if (it != m_clientObjects.end())
					{
						DestroyClientObject(it->second);
						m_clientObjects.erase(it);
					}
					IOS_ResourceReply(cmd, IOS_ERROR_OK);
					break;
				}
				case IPCCommandId::IOS_IOCTLV:
				{
					void* client = LookupClient(cmd->devHandle);
					if (!client)
					{
						cemuLog_log(LogType::Force, "IPCSimpleService[{}]: Invalid IPC handle", m_devicePath);
						IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
						break;
					}
					const uint32 request = cmd->args[0];
					const uint32 numIn = cmd->args[1];
					const uint32 numOut = cmd->args[2];
					IPCIoctlVector* vec = MEMPTR<IPCIoctlVector>(cmd->args[3]).GetPtr();
					m_delayResponse = false;
					m_activeCmd = cmd;
					const uint32 result = ServiceCall(client, request, vec, numIn, vec + numIn, numOut);
					if (!m_delayResponse)
						IOS_ResourceReply(cmd, (IOS_ERROR)result);
					m_activeCmd = nullptr;
					break;
				}
				default:
					cemuLog_log(LogType::Force, "IPCSimpleService[{}]: Unsupported IPC cmdId {}", m_devicePath, (uint32)cmd->cmdId.value());
					cemu_assert_unimplemented();
					IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
					break;
				}
			}
			IOS_DestroyMessageQueue(m_msgQueueId);
			m_threadInitialized = false;
		}

		void IPCSimpleService::SetTimerUpdate(uint32 milliseconds)
		{
			if (m_timerId != IOSInvalidTimerId)
				IOS_DestroyTimer(m_timerId);
			const uint32 us = milliseconds * 1000;
			m_timerId = IOS_CreateTimer(us, us, m_msgQueueId, kMsgTimer);
		}

		IPCCommandBody* IPCSimpleService::ServiceCallDelayCurrentResponse()
		{
			cemu_assert_debug(m_activeCmd);
			m_delayResponse = true;
			return m_activeCmd;
		}

		void IPCSimpleService::ServiceCallAsyncRespond(IPCCommandBody* response, uint32 r)
		{
			IOS_ResourceReply(response, (IOS_ERROR)r);
		}

		/* ---- IPCService ---- */

		void IPCService::Start()
		{
			if (m_isRunning.exchange(true))
				return;
			LaunchThread(this, m_isRunning, m_requestStop, m_threadInitialized, m_serviceThread, &IPCService::ServiceThread);
		}

		void IPCService::Stop()
		{
			if (!m_isRunning.exchange(false))
				return;
			m_requestStop = true;
			IOS_SendMessage(m_msgQueueId, kMsgQuit, 0);
			m_serviceThread.join();
		}

		void IPCService::HandleServiceRequest(IPCCommandBody* cmd)
		{
			// the nn client counts its reply vectors as "in" and lists them first
			const uint32 numOut = cmd->args[1];
			const uint32 numIn = cmd->args[2];
			IPCIoctlVector* vec = MEMPTR<IPCIoctlVector>(cmd->args[3]).GetPtr();
			IPCIoctlVector* outVec = vec;
			IPCIoctlVector* inVec = vec + numOut;
			cemu_assert(numIn > 0 && numOut > 0);
			cemu_assert(inVec[0].size >= 80 && !inVec[0].basePhys.IsNull());

			uint8* request = MEMPTR<uint8>(inVec[0].basePhys).GetPtr();
			uint8* reply = MEMPTR<uint8>(outVec[0].basePhys).GetPtr();
			const IPCServiceRequestHeader* header = (const IPCServiceRequestHeader*)request;

			IPCServiceCall call(0, header->serviceId, header->commandId);
			// the first buffer of each direction carries a header in front of the payload
			call.AddInputStream(request + sizeof(IPCServiceRequestHeader), inVec[0].size - sizeof(IPCServiceRequestHeader));
			call.AddOutputStream(reply + sizeof(IPCServiceResponseHeader), outVec[0].size - sizeof(IPCServiceResponseHeader));
			for (uint32 i = 1; i < numIn; i++)
				call.AddInputStream(MEMPTR<uint8>(inVec[i].basePhys).GetPtr(), inVec[i].size);
			for (uint32 i = 1; i < numOut; i++)
				call.AddOutputStream(MEMPTR<uint8>(outVec[i].basePhys).GetPtr(), outVec[i].size);

			((IPCServiceResponseHeader*)reply)->nnResultCode = (uint32)ServiceCall(call);
			IOS_ResourceReply(cmd, IOS_ERROR_OK);
		}

		void IPCService::ServiceThread()
		{
			const size_t slash = m_devicePath.find_last_of('/');
			const std::string threadName = "NNsvc_" + m_devicePath.substr(slash == std::string::npos ? 0 : slash + 1);
			SetThreadName(threadName.c_str());
			m_msgQueueId = OpenServiceQueue(m_devicePath, m_queueStorage.GetPtr(), m_queueStorage.GetCount());
			m_threadInitialized = true;
			while (true)
			{
				IOSMessage msg;
				const IOS_ERROR err = IOS_ReceiveMessage(m_msgQueueId, &msg, 0);
				cemu_assert(!IOS_ResultIsError(err));
				if (msg == kMsgQuit)
				{
					cemu_assert_debug(m_requestStop);
					break;
				}
				IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(msg).GetPtr();
				switch ((IPCCommandId)cmd->cmdId)
				{
				case IPCCommandId::IOS_OPEN:
					IOS_ResourceReply(cmd, (IOS_ERROR)CreateClientHandle());
					break;
				case IPCCommandId::IOS_CLOSE:
					CloseClientHandle((IOSDevHandle)(uint32)cmd->devHandle);
					IOS_ResourceReply(cmd, IOS_ERROR_OK);
					break;
				case IPCCommandId::IOS_IOCTLV:
					HandleServiceRequest(cmd);
					break;
				default:
					cemuLog_log(LogType::Force, "{}: Unsupported cmdId", m_devicePath);
					cemu_assert_unimplemented();
					IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
					break;
				}
			}
			IOS_DestroyMessageQueue(m_msgQueueId);
			m_threadInitialized = false;
		}
	};
};
