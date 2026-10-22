// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// camera drives the GamePad's front camera. A game:
//   1. CAMInit(instance, setupInfo, &err)  -> camera handle (> 0)
//   2. CAMOpen(handle)                     -> start streaming
//   3. CAMSubmitTargetSurface(handle, s)   -> queue buffers to fill
//   4. receives "frame decoded" events on the handler from setupInfo
//   5. CAMClose / CAMExit
//
// No real camera is attached. While a camera is open, each 1/60 s tick
// returns one queued buffer to the game through its event handler, contents
// unchanged, so the game sees a steady stream of (blank) frames and never
// stalls waiting for one.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "camera.h"

namespace camera
{
	namespace
	{
		constexpr sint32 kCamOk = 0;
		constexpr sint32 kCamBadHandle = -8;
		constexpr sint32 kWorkMemoryNeeded = 1024;
		constexpr uint32 kEventFrameDecoded = 0;

		// CAMSetupInfo (only the fields we read are named)
		struct SetupInfo
		{
			uint32be streamType;   // +0x00
			uint32be width;        // +0x04
			uint32be height;       // +0x08
			uint32be workMemSize;  // +0x0C
			MEMPTR<void> workMem;  // +0x10
			uint32be eventHandler; // +0x14
			uint32be modeFlags;    // +0x18
			uint32be fps;          // +0x1C
			uint32be affinity;     // +0x20
		};
		static_assert(offsetof(SetupInfo, eventHandler) == 0x14);

		// CAMSurface: a buffer the game wants filled with a frame
		struct Surface
		{
			uint32be size;         // +0x00
			MEMPTR<void> buffer;   // +0x04
			uint32be height;
			uint32be width;
			uint32be pitch;
			uint32be alignment;
			uint32be tileMode;
			uint32be pixelFormat;
		};
		static_assert(sizeof(Surface) == 0x20);

		// CAMEventData passed to the game's handler
		struct EventData
		{
			uint32be type;
			uint32be buffer;
			uint32be size;
			uint32be extra;
		};
		static_assert(sizeof(EventData) == 0x10);

		struct Camera
		{
			uint32 width = 0;
			uint32 height = 0;
			MPTR handler = MPTR_NULL;
			bool streaming = false;
			std::deque<Surface> pending;
		};

		std::mutex g_lock;
		std::vector<std::unique_ptr<Camera>> g_cameras; // handle N lives in slot N-1
		uint32 g_liveCameras = 0;

		SysAllocator<coreinit::OSAlarm_t> g_tickAlarm;
		SysAllocator<EventData> g_eventData;

		// caller holds g_lock
		Camera* Lookup(sint32 handle)
		{
			if (handle < 1 || (size_t)handle > g_cameras.size())
				return nullptr;
			return g_cameras[handle - 1].get();
		}

		// Alarm handler, runs on the emulated CPU 60 times per second.
		void OnTick(void* alarm, void* context)
		{
			size_t slot = 0;
			while (true)
			{
				MPTR handler = MPTR_NULL;
				Surface frame;
				{
					std::lock_guard guard(g_lock);
					// next open camera with a queued buffer
					while (slot < g_cameras.size())
					{
						Camera* cam = g_cameras[slot].get();
						slot++;
						if (cam && cam->streaming && !cam->pending.empty())
						{
							frame = cam->pending.front();
							cam->pending.pop_front();
							handler = cam->handler;
							break;
						}
					}
				}
				if (handler == MPTR_NULL)
					break; // no more cameras with work this tick
				g_eventData->type = kEventFrameDecoded;
				g_eventData->buffer = frame.buffer.GetMPTR();
				g_eventData->size = frame.size;
				g_eventData->extra = 0;
				PPCCoreCallback(handler, g_eventData.GetPtr());
			}
		}

		void StartTicking()
		{
			coreinit::OSCreateAlarm(g_tickAlarm.GetPtr());
			coreinit::OSSetPeriodicAlarm(g_tickAlarm.GetPtr(), coreinit::OSGetTime(), (uint64)ESPRESSO_TIMER_CLOCK / 60ull, makeCallableExport<&OnTick>());
		}

		sint32 CAMGetMemReq(void* streamInfo)
		{
			return kWorkMemoryNeeded;
		}

		sint32 CAMCheckMemSegmentation(void* memory, uint32 size)
		{
			return kCamOk;
		}

		sint32 CAMInit(uint32 instance, SetupInfo* setup, uint32be* errorOut)
		{
			// Some games (Hunter's Trophy 2) refuse to start unless err is cleared.
			*errorOut = 0;
			auto cam = std::make_unique<Camera>();
			cam->width = setup->width;
			cam->height = setup->height;
			cam->handler = setup->eventHandler;

			bool firstCamera;
			sint32 handle;
			{
				std::lock_guard guard(g_lock);
				auto freeSlot = std::find(g_cameras.begin(), g_cameras.end(), nullptr);
				if (freeSlot == g_cameras.end())
					freeSlot = g_cameras.insert(g_cameras.end(), nullptr);
				*freeSlot = std::move(cam);
				handle = (sint32)(freeSlot - g_cameras.begin()) + 1;
				firstCamera = (g_liveCameras == 0);
				g_liveCameras++;
			}
			if (firstCamera)
				StartTicking();
			return handle;
		}

		sint32 CAMExit(sint32 handle)
		{
			bool lastCamera;
			{
				std::lock_guard guard(g_lock);
				if (!Lookup(handle))
					return kCamBadHandle;
				g_cameras[handle - 1].reset();
				g_liveCameras--;
				lastCamera = (g_liveCameras == 0);
			}
			if (lastCamera)
				coreinit::OSCancelAlarm(g_tickAlarm.GetPtr());
			return kCamOk;
		}

		sint32 CAMSubmitTargetSurface(sint32 handle, Surface* surface)
		{
			std::lock_guard guard(g_lock);
			Camera* cam = Lookup(handle);
			if (!cam)
				return kCamBadHandle;
			cam->pending.push_back(*surface);
			return kCamOk;
		}

		void ResetAll()
		{
			std::lock_guard guard(g_lock);
			g_cameras.clear();
			g_liveCameras = 0;
		}

		class CameraLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "camera"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(CAMGetMemReq, "camera", "CAMGetMemReq", LogType::Placeholder);
				cafeExportRegisterFunc(CAMCheckMemSegmentation, "camera", "CAMCheckMemSegmentation", LogType::Placeholder);
				cafeExportRegisterFunc(CAMInit, "camera", "CAMInit", LogType::Placeholder);
				cafeExportRegisterFunc(CAMExit, "camera", "CAMExit", LogType::Placeholder);
				cafeExportRegisterFunc(camera::CAMOpen, "camera", "CAMOpen", LogType::Placeholder);
				cafeExportRegisterFunc(camera::CAMClose, "camera", "CAMClose", LogType::Placeholder);
				cafeExportRegisterFunc(CAMSubmitTargetSurface, "camera", "CAMSubmitTargetSurface", LogType::Placeholder);
			}

			void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
			{
				if (reason == coreinit::RplEntryReason::Loaded)
					ResetAll();
			}
		};

		CameraLibrary g_cameraLibrary;
	}

	sint32 CAMOpen(sint32 handle)
	{
		std::lock_guard guard(g_lock);
		Camera* cam = Lookup(handle);
		if (!cam)
			return kCamBadHandle;
		cam->streaming = true;
		return kCamOk;
	}

	sint32 CAMClose(sint32 handle)
	{
		std::lock_guard guard(g_lock);
		Camera* cam = Lookup(handle);
		if (!cam)
			return kCamBadHandle;
		cam->streaming = false;
		return kCamOk;
	}

	COSModule* GetModule()
	{
		return &g_cameraLibrary;
	}
}
