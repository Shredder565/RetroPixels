// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "prudp.h"
#include "nex.h"
#include "nexThread.h"

namespace
{
	std::mutex s_incomingMutex;
	std::vector<nexService*> s_incoming; // registered, not yet picked up by the thread
	std::atomic<bool> s_started{false};
	std::thread::id s_threadId;

	void ServiceLoop()
	{
		std::vector<nexService*> services; // only touched by this thread
		while (true)
		{
			{
				// also makes sure s_threadId is set before any service runs
				std::lock_guard lock(s_incomingMutex);
				services.insert(services.end(), s_incoming.begin(), s_incoming.end());
				s_incoming.clear();
			}
			if (services.empty())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}
			for (nexService* service : services)
				service->update();
			std::erase_if(services, [](nexService* service) {
				if (!service->isMarkedForDestruction())
					return false;
				service->destroy(); // deletes it since we are the nex thread
				return true;
			});
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

void nexThread_registerService(nexService* service)
{
	std::lock_guard lock(s_incomingMutex);
	if (!s_started)
	{
		std::thread worker(ServiceLoop);
		s_threadId = worker.get_id();
		worker.detach();
		s_started = true;
	}
	s_incoming.push_back(service);
}

bool nexThread_isCurrentThread()
{
	return s_started && std::this_thread::get_id() == s_threadId;
}
