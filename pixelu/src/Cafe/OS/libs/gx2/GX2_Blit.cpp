// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "GX2_Blit.h"
#include "GX2_Command.h"
#include "GX2_Surface.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/common/OSCommon.h"
#include "GX2_Resource.h"

// Clears. Instead of drawing a full screen quad like the console driver, clears are sent to
// the GPU emulation as a single HLE packet that names the target surfaces directly.

namespace GX2
{
	static bool _HasFlag(GX2ClearFlags flags, GX2ClearFlags f)
	{
		return (flags & f) != 0;
	}

	// writes DB_STENCIL_CLEAR and/or DB_DEPTH_CLEAR (they are adjacent, stencil first)
	static void _WriteClearValueRegs(bool writeDepth, bool writeStencil, float depthValue, uint8 stencilValue)
	{
		Latte::LATTE_DB_STENCIL_CLEAR stencilReg;
		stencilReg.set_clearValue(stencilValue);
		Latte::LATTE_DB_DEPTH_CLEAR depthReg;
		depthReg.set_clearValue(depthValue);
		if (writeDepth && writeStencil)
		{
			GX2ReserveCmdSpace(4);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 3), Latte::REGADDR::DB_STENCIL_CLEAR - 0xA000, stencilReg, depthReg);
		}
		else if (writeDepth)
		{
			GX2ReserveCmdSpace(3);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), Latte::REGADDR::DB_DEPTH_CLEAR - 0xA000, depthReg);
		}
		else if (writeStencil)
		{
			GX2ReserveCmdSpace(3);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), Latte::REGADDR::DB_STENCIL_CLEAR - 0xA000, stencilReg);
		}
	}

	void GX2SetClearDepthStencil(GX2DepthBuffer* depthBuffer, float depthClearValue, uint8 stencilClearValue)
	{
		depthBuffer->clearDepth = depthClearValue;
		depthBuffer->clearStencil = stencilClearValue;
		_WriteClearValueRegs(true, true, depthClearValue, stencilClearValue);
	}

	void GX2SetClearDepth(GX2DepthBuffer* depthBuffer, float depthClearValue)
	{
		depthBuffer->clearDepth = depthClearValue;
		_WriteClearValueRegs(true, false, depthClearValue, 0);
	}

	void GX2SetClearStencil(GX2DepthBuffer* depthBuffer, uint8 stencilClearValue)
	{
		depthBuffer->clearStencil = stencilClearValue;
		_WriteClearValueRegs(false, true, 0.0f, stencilClearValue);
	}

	// color channels are sent as 0-255 integers. Out of range values used to be undefined
	// behaviour in the conversion; they now give the result x86 builds always produced.
	static uint32 _ColorChannelToInt(float c)
	{
		const float v = c * 255.0f;
		if (!(v > -9.0e18f && v < 9.0e18f))
			return 0;
		return (uint32)(sint64)v;
	}

	struct ClearTarget
	{
		MPTR physAddr{ MPTR_NULL };
		uint32 format{};
		uint32 tileMode{};
		uint32 width{};
		uint32 height{};
		uint32 pitch{};
		uint32 firstSlice{};
		uint32 numSlices{};

		ClearTarget() = default;

		ClearTarget(const GX2Surface& s, uint32 viewFirstSlice, uint32 viewNumSlices)
		{
			physAddr = memory_virtualToPhysical(s.imagePtr);
			format = (uint32)s.format.value();
			tileMode = (uint32)s.tileMode.value();
			width = s.width;
			height = s.height;
			pitch = s.pitch;
			firstSlice = viewFirstSlice;
			numSlices = viewNumSlices;
		}
	};

	// IT_HLE_CLEAR_COLOR_DEPTH_STENCIL: flags, color target (8 words), depth target (8 words),
	// RGBA as integers, depth as float bits, stencil
	static void _SubmitClear(GX2ColorBuffer* colorBuffer, const float rgba[4], GX2DepthBuffer* depthBuffer, float depthValue, uint8 stencilValue, bool clearColor, bool clearDepth, bool clearStencil)
	{
		GX2ReserveCmdSpace(50);
		const uint32 flags = (clearColor ? 1 : 0) | (clearDepth ? 2 : 0) | (clearStencil ? 4 : 0);
		const ClearTarget color = colorBuffer ? ClearTarget(colorBuffer->surface, colorBuffer->viewFirstSlice, colorBuffer->viewNumSlices) : ClearTarget();
		const ClearTarget depth = depthBuffer ? ClearTarget(depthBuffer->surface, depthBuffer->viewFirstSlice, depthBuffer->viewNumSlices) : ClearTarget();
		gx2WriteGather_submit(pm4HeaderType3(IT_HLE_CLEAR_COLOR_DEPTH_STENCIL, 23), flags,
			color.physAddr, color.format, color.tileMode, color.width, color.height, color.pitch, color.firstSlice, color.numSlices,
			depth.physAddr, depth.format, depth.tileMode, depth.width, depth.height, depth.pitch, depth.firstSlice, depth.numSlices,
			_ColorChannelToInt(rgba[0]), _ColorChannelToInt(rgba[1]), _ColorChannelToInt(rgba[2]), _ColorChannelToInt(rgba[3]),
			depthValue, (uint32)stencilValue);
	}

	void GX2ClearColor(GX2ColorBuffer* colorBuffer, float r, float g, float b, float a)
	{
		if ((colorBuffer->surface.resFlag & GX2_RESFLAG_USAGE_COLOR_BUFFER) == 0)
		{
			debug_printf("GX2ClearColor(): surface is not a color buffer\n");
			return;
		}
		const float rgba[4] = { r, g, b, a };
		_SubmitClear(colorBuffer, rgba, nullptr, 0.0f, 0, true, false, false);
	}

	void GX2ClearBuffersEx(GX2ColorBuffer* colorBuffer, GX2DepthBuffer* depthBuffer, float r, float g, float b, float a, float depthClearValue, uint8 stencilClearValue, GX2ClearFlags clearFlags)
	{
		_WriteClearValueRegs(_HasFlag(clearFlags, GX2ClearFlags::SET_DEPTH_REG), _HasFlag(clearFlags, GX2ClearFlags::SET_STENCIL_REG), depthClearValue, stencilClearValue);
		const float rgba[4] = { r, g, b, a };
		_SubmitClear(colorBuffer, rgba, depthBuffer, depthClearValue, stencilClearValue,
			true, _HasFlag(clearFlags, GX2ClearFlags::CLEAR_DEPTH), _HasFlag(clearFlags, GX2ClearFlags::CLEAR_STENCIL));
	}

	// the given values are used for the clear even when the flags don't update the clear registers
	void GX2ClearDepthStencilEx(GX2DepthBuffer* depthBuffer, float depthClearValue, uint8 stencilClearValue, GX2ClearFlags clearFlags)
	{
		if (!depthBuffer)
		{
			// previously this dereferenced the null pointer
			debug_printf("GX2ClearDepthStencilEx(): no depth buffer given\n");
			return;
		}
		_WriteClearValueRegs(_HasFlag(clearFlags, GX2ClearFlags::SET_DEPTH_REG), _HasFlag(clearFlags, GX2ClearFlags::SET_STENCIL_REG), depthClearValue, stencilClearValue);
		const float rgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		_SubmitClear(nullptr, rgba, depthBuffer, depthClearValue, stencilClearValue,
			false, _HasFlag(clearFlags, GX2ClearFlags::CLEAR_DEPTH), _HasFlag(clearFlags, GX2ClearFlags::CLEAR_STENCIL));
	}

	void GX2BlitInit()
	{
		cafeExportRegister("gx2", GX2SetClearDepthStencil, LogType::GX2);
		cafeExportRegister("gx2", GX2SetClearDepth, LogType::GX2);
		cafeExportRegister("gx2", GX2SetClearStencil, LogType::GX2);

		cafeExportRegister("gx2", GX2ClearColor, LogType::GX2);
		cafeExportRegister("gx2", GX2ClearBuffersEx, LogType::GX2);
		cafeExportRegister("gx2", GX2ClearDepthStencilEx, LogType::GX2);
	}
}
