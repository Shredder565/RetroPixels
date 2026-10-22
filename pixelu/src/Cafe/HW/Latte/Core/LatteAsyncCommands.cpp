// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include <variant>

// CPU-side code (GX2 HLE) sometimes needs the GPU thread to do something outside of
// the command buffer. Requests are queued here and run by the GPU thread whenever the
// command processor polls LatteAsyncCommands_checkAndExecute.

void LatteThread_Exit();

namespace
{
	// copy a texture back to guest memory right away
	struct ReadbackRequest
	{
		MPTR physAddr;
		MPTR mipAddr;
		sint32 format;
		Latte::E_HWTILEMODE tilemode;
		sint32 width;
		sint32 height;
		sint32 depth;
		uint32 pitch;
		uint32 slice;
		sint32 level;
	};

	struct DeleteShaderRequest
	{
		uint64 baseHash;
		uint64 auxHash;
		LatteConst::ShaderType type;
	};

	struct CopyRequest
	{
		LatteSurfaceCopyParam src;
		LatteSurfaceCopyParam dst;
		LatteSurfaceCopyRect rect;
	};

	using Request = std::variant<ReadbackRequest, DeleteShaderRequest, CopyRequest>;

	std::mutex s_queueMutex;
	std::deque<Request> s_queue;
	std::atomic<uint32> s_unfinished{0}; // queued or currently running

	void Enqueue(Request&& request)
	{
		std::lock_guard lock(s_queueMutex);
		s_queue.emplace_back(std::move(request));
		s_unfinished++;
	}

	void Run(const ReadbackRequest& r)
	{
		cemu_assert_debug(r.level == 0); // mip levels would need their own swizzle
		// the swizzle is not used for the lookup
		LatteTextureView* view = LatteTC_GetTextureSliceViewOrTryCreate(r.physAddr, r.mipAddr, (Latte::E_GX2SURFFMT)r.format, r.tilemode, r.width, r.height, r.depth, r.pitch, 0, r.slice, r.level);
		if (!view)
		{
			cemuLog_logDebug(LogType::Force, "Forced readback: no texture at {:08x}", r.physAddr);
			return;
		}
		LatteTexture_UpdateDataToLatest(view->baseTexture);
		LatteTextureReadback_StartTransfer(view);
		LatteTextureReadback_UpdateFinishedTransfers(true); // blocks until the data is written back
	}

	void Run(const DeleteShaderRequest& r)
	{
		LatteSHRC_RemoveFromCacheByHash(r.baseHash, r.auxHash, r.type);
	}

	void Run(const CopyRequest& r)
	{
		LatteSurfaceCopy_copySurfaceNew(r.src, r.dst, r.rect);
	}
}

void LatteAsyncCommands_queueForceTextureReadback(MPTR physAddr, MPTR mipAddr, uint32 swizzle, sint32 format, sint32 width, sint32 height, sint32 depth, uint32 pitch, uint32 slice, sint32 dim, Latte::E_HWTILEMODE tilemode, sint32 aa, sint32 level)
{
	// swizzle, dim and aa are accepted for the GX2 side but not needed
	Enqueue(ReadbackRequest{ physAddr, mipAddr, format, tilemode, width, height, depth, pitch, slice, level });
}

void LatteAsyncCommands_queueDeleteShader(uint64 shaderBaseHash, uint64 shaderAuxHash, LatteConst::ShaderType shaderType)
{
	Enqueue(DeleteShaderRequest{ shaderBaseHash, shaderAuxHash, shaderType });
}

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	Enqueue(CopyRequest{ src, dst, rect });
}

void LatteAsyncCommands_waitUntilAllProcessed()
{
	while (s_unfinished.load() != 0)
		_mm_pause();
}

// GPU thread, polled by the command processor. Also where the thread notices a stop request.
void LatteAsyncCommands_checkAndExecute()
{
	if (Latte_GetStopSignal())
		LatteThread_Exit();
	while (s_unfinished.load() != 0)
	{
		Request request;
		{
			std::lock_guard lock(s_queueMutex);
			if (s_queue.empty())
				return;
			request = std::move(s_queue.front());
			s_queue.pop_front();
		}
		std::visit([](const auto& r) { Run(r); }, request);
		s_unfinished--;
	}
}
