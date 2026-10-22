// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "GX2_State.h"
#include "GX2_Command.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/common/OSCommon.h"

namespace GX2
{
	using namespace Latte;
	using MODE_CNTL = LATTE_PA_SU_SC_MODE_CNTL;
	using DEPTH_CONTROL = LATTE_DB_DEPTH_CONTROL;

	// Writes one SET_CONTEXT_REG packet: header, register offset, then one word per value.
	// The caller reserves command space (often for several packets at once).
	template<typename... TVals>
	static void _EmitContextRegs(REGADDR firstReg, const TVals&... values)
	{
		constexpr uint32 count = (uint32)sizeof...(TVals);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + count), (uint32)firstReg - 0xA000, values...);
	}

	// reserves space for and writes a single SET_CONTEXT_REG packet
	template<typename... TVals>
	static void _SetContextRegs(REGADDR firstReg, const TVals&... values)
	{
		GX2ReserveCmdSpace(2 + (uint32)sizeof...(TVals));
		_EmitContextRegs(firstReg, values...);
	}

	// Note: the Latte register setters do not mask their input, so out-of-range game values
	// can spill into neighbouring fields. Fields are therefore set in one fixed order per
	// register (later fields win), which keeps the register words games see stable.

	// point sizes are stored as unsigned 12.3 fixed point, saturated to 16 bits
	static uint32 _PointSizeToFixed(float size)
	{
		if (size < 0.0f)
			cemu_assert_suspicious();
		const float scaled = size * 8.0f;
		if (!(scaled > 0.0f)) // also catches NaN
			return 0;
		if (scaled >= 65535.0f)
			return 0xFFFF;
		return (uint32)scaled;
	}

	/* alpha test */

	void GX2InitAlphaTestReg(GX2AlphaTestReg* reg, uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef)
	{
		LATTE_SX_ALPHA_TEST_CONTROL control;
		control.set_ALPHA_FUNC(alphaFunc).set_ALPHA_TEST_ENABLE(alphaTestEnable != 0);
		reg->regAlphaTestControl = control;
		reg->regAlphaTestRef = LATTE_SX_ALPHA_REF().set_ALPHA_TEST_REF(alphaRef);
	}

	void GX2SetAlphaTestReg(GX2AlphaTestReg* reg)
	{
		GX2ReserveCmdSpace(6);
		_EmitContextRegs(REGADDR::SX_ALPHA_TEST_CONTROL, reg->regAlphaTestControl);
		_EmitContextRegs(REGADDR::SX_ALPHA_REF, reg->regAlphaTestRef);
	}

	void GX2SetAlphaTest(uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef)
	{
		GX2AlphaTestReg r;
		GX2InitAlphaTestReg(&r, alphaTestEnable, alphaFunc, alphaRef);
		GX2SetAlphaTestReg(&r);
	}

	/* color control */

	void GX2InitColorControlReg(GX2ColorControlReg* reg, GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable)
	{
		using SPECIALOP = LATTE_CB_COLOR_CONTROL::E_SPECIALOP;
		LATTE_CB_COLOR_CONTROL v;
		v.set_MULTIWRITE_ENABLE(multiwriteEnable != 0);
		v.set_SPECIAL_OP(colorBufferEnable != 0 ? SPECIALOP::NORMAL : SPECIALOP::DISABLE);
		v.set_BLEND_MASK(blendMask);
		v.set_ROP(logicOp);
		reg->reg = v;
	}

	void GX2SetColorControlReg(GX2ColorControlReg* reg)
	{
		_SetContextRegs(REGADDR::CB_COLOR_CONTROL, reg->reg);
	}

	void GX2SetColorControl(GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable)
	{
		GX2ColorControlReg r;
		GX2InitColorControlReg(&r, logicOp, blendMask, multiwriteEnable, colorBufferEnable);
		GX2SetColorControlReg(&r);
	}

	/* polygon control */

	void GX2InitPolygonControlReg(GX2PolygonControlReg* reg, MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack,
		MODE_CNTL::E_POLYGONMODE usePolygonMode, MODE_CNTL::E_PTYPE polyModeFront, MODE_CNTL::E_PTYPE polyModeBack,
		uint32 polygonOffsetFrontEnable, uint32 polygonOffsetBackEnable, uint32 paraOffsetEnable)
	{
		// only the lowest bit of the boolean style arguments is used
		MODE_CNTL v;
		v.set_FRONT_FACE(frontFace);
		v.set_CULL_FRONT((cullFront & 1) != 0).set_CULL_BACK((cullBack & 1) != 0);
		v.set_POLYGON_MODE(usePolygonMode);
		v.set_FRONT_POLY_MODE(polyModeFront).set_BACK_POLY_MODE(polyModeBack);
		v.set_OFFSET_PARA_ENABLED((paraOffsetEnable & 1) != 0);
		v.set_OFFSET_FRONT_ENABLED((polygonOffsetFrontEnable & 1) != 0);
		v.set_OFFSET_BACK_ENABLED((polygonOffsetBackEnable & 1) != 0);
		reg->reg = v;
	}

	void GX2SetPolygonControlReg(GX2PolygonControlReg* reg)
	{
		_SetContextRegs(REGADDR::PA_SU_SC_MODE_CNTL, reg->reg);
	}

	void GX2SetPolygonControl(MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack,
		MODE_CNTL::E_POLYGONMODE usePolygonMode, MODE_CNTL::E_PTYPE polyModeFront, MODE_CNTL::E_PTYPE polyModeBack,
		uint32 polygonOffsetFrontEnable, uint32 polygonOffsetBackEnable, uint32 paraOffsetEnable)
	{
		GX2PolygonControlReg r{};
		GX2InitPolygonControlReg(&r, frontFace, cullFront, cullBack, usePolygonMode, polyModeFront, polyModeBack,
			polygonOffsetFrontEnable, polygonOffsetBackEnable, paraOffsetEnable);
		GX2SetPolygonControlReg(&r);
	}

	void GX2SetCullOnlyControl(MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack)
	{
		// everything except face orientation and culling is left at zero
		GX2PolygonControlReg r{};
		GX2InitPolygonControlReg(&r, frontFace, cullFront, cullBack, MODE_CNTL::E_POLYGONMODE::UKN0,
			MODE_CNTL::E_PTYPE::POINTS, MODE_CNTL::E_PTYPE::POINTS, 0, 0, 0);
		GX2SetPolygonControlReg(&r);
	}

	/* polygon offset */

	void GX2InitPolygonOffsetReg(GX2PolygonOffsetReg* reg, float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset)
	{
		// the hardware slope scale is in 1/16 units
		reg->regFrontScale = LATTE_PA_SU_POLY_OFFSET_FRONT_SCALE().set_SCALE(frontScale * 16.0f);
		reg->regFrontOffset = LATTE_PA_SU_POLY_OFFSET_FRONT_OFFSET().set_OFFSET(frontOffset);
		reg->regBackScale = LATTE_PA_SU_POLY_OFFSET_BACK_SCALE().set_SCALE(backScale * 16.0f);
		reg->regBackOffset = LATTE_PA_SU_POLY_OFFSET_BACK_OFFSET().set_OFFSET(backOffset);
		reg->regClamp = LATTE_PA_SU_POLY_OFFSET_CLAMP().set_CLAMP(clampOffset);
	}

	void GX2SetPolygonOffsetReg(GX2PolygonOffsetReg* reg)
	{
		GX2ReserveCmdSpace(6 + 3);
		_EmitContextRegs(REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE, reg->regFrontScale, reg->regFrontOffset, reg->regBackScale, reg->regBackOffset);
		_EmitContextRegs(REGADDR::PA_SU_POLY_OFFSET_CLAMP, reg->regClamp);
	}

	void GX2SetPolygonOffset(float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset)
	{
		GX2PolygonOffsetReg r;
		GX2InitPolygonOffsetReg(&r, frontOffset, frontScale, backOffset, backScale, clampOffset);
		GX2SetPolygonOffsetReg(&r);
	}

	/* clipping */

	void GX2SetRasterizerClipControlEx(bool enableRasterizer, bool enableZClip, bool enableHalfZ)
	{
		LATTE_PA_CL_CLIP_CNTL v{};
		v.set_ZCLIP_NEAR_DISABLE(!enableZClip);
		v.set_ZCLIP_FAR_DISABLE(!enableZClip);
		v.set_DX_CLIP_SPACE_DEF(enableHalfZ); // z range 0..1 instead of -1..1
		v.set_DX_RASTERIZATION_KILL(!enableRasterizer);
		v.set_DX_LINEAR_ATTR_CLIP_ENA(true);
		_SetContextRegs(REGADDR::PA_CL_CLIP_CNTL, v);
	}

	void GX2SetRasterizerClipControl(bool enableRasterizer, bool enableZClip)
	{
		GX2SetRasterizerClipControlEx(enableRasterizer, enableZClip, false);
	}

	void GX2SetRasterizerClipControlHalfZ(bool enableRasterizer, bool enableZClip, bool enableHalfZ)
	{
		GX2SetRasterizerClipControlEx(enableRasterizer, enableZClip, enableHalfZ);
	}

	/* viewport */

	void GX2InitViewportReg(GX2ViewportReg* viewportReg, float x, float y, float width, float height, float nearZ, float farZ)
	{
		// maps NDC to window coordinates (y flipped); the guard band words are not touched
		const float halfW = width * 0.5f;
		const float halfH = height * 0.5f;
		viewportReg->xScale = LATTE_PA_CL_VPORT_XSCALE().set_SCALE(halfW);
		viewportReg->xOffset = LATTE_PA_CL_VPORT_XOFFSET().set_OFFSET(x + halfW);
		viewportReg->yScale = LATTE_PA_CL_VPORT_YSCALE().set_SCALE(height * -0.5f);
		viewportReg->yOffset = LATTE_PA_CL_VPORT_YOFFSET().set_OFFSET(y + halfH);
		viewportReg->zScale = LATTE_PA_CL_VPORT_ZSCALE().set_SCALE((farZ - nearZ) * 0.5f);
		viewportReg->zOffset = LATTE_PA_CL_VPORT_ZOFFSET().set_OFFSET((nearZ + farZ) * 0.5f);
	}

	void GX2SetViewportReg(GX2ViewportReg* viewportReg)
	{
		_SetContextRegs(REGADDR::PA_CL_VPORT_XSCALE,
			viewportReg->xScale, viewportReg->xOffset,
			viewportReg->yScale, viewportReg->yOffset,
			viewportReg->zScale, viewportReg->zOffset);
	}

	void GX2SetViewport(float x, float y, float width, float height, float nearZ, float farZ)
	{
		GX2ViewportReg r;
		GX2InitViewportReg(&r, x, y, width, height, nearZ, farZ);
		GX2SetViewportReg(&r);
	}

	/* scissor */

	void GX2InitScissorReg(GX2ScissorReg* scissorReg, uint32 x, uint32 y, uint32 width, uint32 height)
	{
		constexpr uint32 kMaxCoord = 8192;
		const uint32 left = std::min(x, kMaxCoord);
		const uint32 top = std::min(y, kMaxCoord);
		const uint32 right = std::min(x + width, kMaxCoord);
		const uint32 bottom = std::min(y + height, kMaxCoord);
		scissorReg->scissorTL = LATTE_PA_SC_GENERIC_SCISSOR_TL().set_TL_X(left).set_TL_Y(top).set_WINDOW_OFFSET_DISABLE(true);
		scissorReg->scissorBR = LATTE_PA_SC_GENERIC_SCISSOR_BR().set_BR_X(right).set_BR_Y(bottom);
	}

	void GX2SetScissorReg(GX2ScissorReg* scissorReg)
	{
		_SetContextRegs(REGADDR::PA_SC_GENERIC_SCISSOR_TL, scissorReg->scissorTL, scissorReg->scissorBR);
	}

	void GX2GetScissorReg(GX2ScissorReg* scissorReg, uint32be* x, uint32be* y, uint32be* width, uint32be* height)
	{
		const LATTE_PA_SC_GENERIC_SCISSOR_TL tl = scissorReg->scissorTL.value();
		const LATTE_PA_SC_GENERIC_SCISSOR_BR br = scissorReg->scissorBR.value();
		const uint32 left = tl.get_TL_X();
		const uint32 top = tl.get_TL_Y();
		*x = left;
		*y = top;
		*width = (uint32)br.get_BR_X() - left;
		*height = (uint32)br.get_BR_Y() - top;
	}

	void GX2SetScissor(uint32 x, uint32 y, uint32 width, uint32 height)
	{
		GX2ScissorReg r;
		GX2InitScissorReg(&r, x, y, width, height);
		GX2SetScissorReg(&r);
	}

	/* depth / stencil */

	static DEPTH_CONTROL _MakeDepthStencilControl(bool depthTestEnable, bool depthWriteEnable, DEPTH_CONTROL::E_ZFUNC depthFunction,
		bool stencilTestEnable, bool backStencilTestEnable,
		DEPTH_CONTROL::E_STENCILFUNC frontFunc, DEPTH_CONTROL::E_STENCILACTION frontZPass, DEPTH_CONTROL::E_STENCILACTION frontZFail, DEPTH_CONTROL::E_STENCILACTION frontFail,
		DEPTH_CONTROL::E_STENCILFUNC backFunc, DEPTH_CONTROL::E_STENCILACTION backZPass, DEPTH_CONTROL::E_STENCILACTION backZFail, DEPTH_CONTROL::E_STENCILACTION backFail)
	{
		DEPTH_CONTROL v{};
		v.set_Z_ENABLE(depthTestEnable).set_Z_WRITE_ENABLE(depthWriteEnable).set_Z_FUNC(depthFunction);
		v.set_STENCIL_ENABLE(stencilTestEnable).set_BACK_STENCIL_ENABLE(backStencilTestEnable);
		v.set_STENCIL_FUNC_F(frontFunc).set_STENCIL_FUNC_B(backFunc);
		v.set_STENCIL_ZPASS_F(frontZPass).set_STENCIL_ZFAIL_F(frontZFail).set_STENCIL_FAIL_F(frontFail);
		v.set_STENCIL_ZPASS_B(backZPass).set_STENCIL_ZFAIL_B(backZFail).set_STENCIL_FAIL_B(backFail);
		return v;
	}

	void GX2SetDepthOnlyControl(bool depthTestEnable, bool depthWriteEnable, DEPTH_CONTROL::E_ZFUNC depthFunction)
	{
		// stencil fields are all zero, i.e. stencil testing gets turned off
		DEPTH_CONTROL v{};
		v.set_Z_ENABLE(depthTestEnable).set_Z_WRITE_ENABLE(depthWriteEnable).set_Z_FUNC(depthFunction);
		_SetContextRegs(REGADDR::DB_DEPTH_CONTROL, v);
	}

	void GX2SetDepthStencilControl(bool depthTestEnable, bool depthWriteEnable, DEPTH_CONTROL::E_ZFUNC depthFunction,
		bool stencilTestEnable, bool backStencilTestEnable,
		DEPTH_CONTROL::E_STENCILFUNC frontStencilFunction,
		DEPTH_CONTROL::E_STENCILACTION frontStencilZPass, DEPTH_CONTROL::E_STENCILACTION frontStencilZFail, DEPTH_CONTROL::E_STENCILACTION frontStencilFail,
		DEPTH_CONTROL::E_STENCILFUNC backStencilFunction,
		DEPTH_CONTROL::E_STENCILACTION backStencilZPass, DEPTH_CONTROL::E_STENCILACTION backStencilZFail, DEPTH_CONTROL::E_STENCILACTION backStencilFail)
	{
		const DEPTH_CONTROL v = _MakeDepthStencilControl(depthTestEnable, depthWriteEnable, depthFunction, stencilTestEnable, backStencilTestEnable,
			frontStencilFunction, frontStencilZPass, frontStencilZFail, frontStencilFail,
			backStencilFunction, backStencilZPass, backStencilZFail, backStencilFail);
		_SetContextRegs(REGADDR::DB_DEPTH_CONTROL, v);
	}

	void GX2InitDepthStencilControlReg(GX2DepthStencilControlReg* depthStencilControlReg,
		bool depthTestEnable, bool depthWriteEnable, DEPTH_CONTROL::E_ZFUNC depthFunction,
		bool stencilTestEnable, bool backStencilTestEnable,
		DEPTH_CONTROL::E_STENCILFUNC frontStencilFunction,
		DEPTH_CONTROL::E_STENCILACTION frontStencilZPass, DEPTH_CONTROL::E_STENCILACTION frontStencilZFail, DEPTH_CONTROL::E_STENCILACTION frontStencilFail,
		DEPTH_CONTROL::E_STENCILFUNC backStencilFunction,
		DEPTH_CONTROL::E_STENCILACTION backStencilZPass, DEPTH_CONTROL::E_STENCILACTION backStencilZFail, DEPTH_CONTROL::E_STENCILACTION backStencilFail)
	{
		depthStencilControlReg->reg = _MakeDepthStencilControl(depthTestEnable, depthWriteEnable, depthFunction, stencilTestEnable, backStencilTestEnable,
			frontStencilFunction, frontStencilZPass, frontStencilZFail, frontStencilFail,
			backStencilFunction, backStencilZPass, backStencilZFail, backStencilFail);
	}

	void GX2SetDepthStencilControlReg(GX2DepthStencilControlReg* depthStencilControlReg)
	{
		_SetContextRegs(REGADDR::DB_DEPTH_CONTROL, depthStencilControlReg->reg);
	}

	void GX2GetDepthStencilControlReg(GX2DepthStencilControlReg* depthStencilControlReg,
		uint32be* depthTestEnable, uint32be* depthWriteEnable, uint32be* depthFunction,
		uint32be* stencilTestEnable, uint32be* backStencilTestEnable,
		uint32be* frontStencilFunction,
		uint32be* frontStencilZPass, uint32be* frontStencilZFail, uint32be* frontStencilFail,
		uint32be* backStencilFunction,
		uint32be* backStencilZPass, uint32be* backStencilZFail, uint32be* backStencilFail)
	{
		const DEPTH_CONTROL v = depthStencilControlReg->reg.value();
		*depthTestEnable = v.get_Z_ENABLE() ? 1 : 0;
		*depthWriteEnable = v.get_Z_WRITE_ENABLE() ? 1 : 0;
		*depthFunction = (uint32)v.get_Z_FUNC();
		*stencilTestEnable = v.get_STENCIL_ENABLE() ? 1 : 0;
		*backStencilTestEnable = v.get_BACK_STENCIL_ENABLE() ? 1 : 0;
		*frontStencilFunction = (uint32)v.get_STENCIL_FUNC_F();
		*frontStencilZPass = (uint32)v.get_STENCIL_ZPASS_F();
		*frontStencilZFail = (uint32)v.get_STENCIL_ZFAIL_F();
		*frontStencilFail = (uint32)v.get_STENCIL_FAIL_F();
		*backStencilFunction = (uint32)v.get_STENCIL_FUNC_B();
		*backStencilZPass = (uint32)v.get_STENCIL_ZPASS_B();
		*backStencilZFail = (uint32)v.get_STENCIL_ZFAIL_B();
		*backStencilFail = (uint32)v.get_STENCIL_FAIL_B();
	}

	void GX2InitStencilMaskReg(GX2StencilMaskReg* stencilMaskReg, uint8 compareMaskFront, uint8 writeMaskFront, uint8 refFront, uint8 compareMaskBack, uint8 writeMaskBack, uint8 refBack)
	{
		stencilMaskReg->stencilRefMaskFrontReg = LATTE_DB_STENCILREFMASK().set_STENCILREF_F(refFront).set_STENCILMASK_F(compareMaskFront).set_STENCILWRITEMASK_F(writeMaskFront);
		stencilMaskReg->stencilRefMaskBackReg = LATTE_DB_STENCILREFMASK_BF().set_STENCILREF_B(refBack).set_STENCILMASK_B(compareMaskBack).set_STENCILWRITEMASK_B(writeMaskBack);
	}

	void GX2SetStencilMaskReg(GX2StencilMaskReg* stencilMaskReg)
	{
		GX2ReserveCmdSpace(6);
		_EmitContextRegs(REGADDR::DB_STENCILREFMASK, stencilMaskReg->stencilRefMaskFrontReg);
		_EmitContextRegs(REGADDR::DB_STENCILREFMASK_BF, stencilMaskReg->stencilRefMaskBackReg);
	}

	void GX2SetStencilMask(uint8 compareMaskFront, uint8 writeMaskFront, uint8 refFront, uint8 compareMaskBack, uint8 writeMaskBack, uint8 refBack)
	{
		GX2StencilMaskReg r;
		GX2InitStencilMaskReg(&r, compareMaskFront, writeMaskFront, refFront, compareMaskBack, writeMaskBack, refBack);
		GX2SetStencilMaskReg(&r);
	}

	void GX2SetPrimitiveRestartIndex(uint32 restartIndex)
	{
		_SetContextRegs(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, LATTE_VGT_MULTI_PRIM_IB_RESET_INDX().set_RESTART_INDEX(restartIndex));
	}

	/* channel masks */

	void GX2InitTargetChannelMasksReg(GX2TargetChannelMaskReg* reg, GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7)
	{
		const GX2_CHANNELMASK masks[8] = { t0, t1, t2, t3, t4, t5, t6, t7 };
		uint32 packed = 0;
		for (uint32 i = 0; i < 8; i++)
			packed |= (masks[i] & 0xF) << (i * 4);
		reg->reg = LATTE_CB_TARGET_MASK().set_MASK(packed);
	}

	void GX2SetTargetChannelMasksReg(GX2TargetChannelMaskReg* reg)
	{
		_SetContextRegs(REGADDR::CB_TARGET_MASK, reg->reg);
	}

	void GX2SetTargetChannelMasks(GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7)
	{
		GX2TargetChannelMaskReg r;
		GX2InitTargetChannelMasksReg(&r, t0, t1, t2, t3, t4, t5, t6, t7);
		GX2SetTargetChannelMasksReg(&r);
	}

	void GX2GetTargetChannelMasksReg(GX2TargetChannelMaskReg* reg, uint32be* t0, uint32be* t1, uint32be* t2, uint32be* t3,
		uint32be* t4, uint32be* t5, uint32be* t6, uint32be* t7)
	{
		uint32be* out[8] = { t0, t1, t2, t3, t4, t5, t6, t7 };
		const uint32 packed = reg->reg.value().get_MASK();
		for (uint32 i = 0; i < 8; i++)
			*out[i] = (packed >> (i * 4)) & 0xF;
	}

	/* blending */

	void GX2InitBlendControlReg(GX2BlendControlReg* reg, uint32 renderTargetIndex, GX2_BLENDFACTOR colorSrcFactor, GX2_BLENDFACTOR colorDstFactor, GX2_BLENDFUNC colorCombineFunc, uint32 separateAlphaBlend, GX2_BLENDFACTOR alphaSrcFactor, GX2_BLENDFACTOR alphaDstFactor, GX2_BLENDFUNC alphaCombineFunc)
	{
		LATTE_CB_BLENDN_CONTROL v;
		v.set_COLOR_SRCBLEND(colorSrcFactor).set_COLOR_DSTBLEND(colorDstFactor).set_COLOR_COMB_FCN(colorCombineFunc);
		v.set_ALPHA_SRCBLEND(alphaSrcFactor).set_ALPHA_DSTBLEND(alphaDstFactor).set_ALPHA_COMB_FCN(alphaCombineFunc);
		v.set_SEPARATE_ALPHA_BLEND(separateAlphaBlend != 0);
		reg->index = renderTargetIndex;
		reg->reg = v;
	}

	void GX2SetBlendControlReg(GX2BlendControlReg* reg)
	{
		// the per target blend registers are consecutive
		_SetContextRegs((REGADDR)((uint32)REGADDR::CB_BLEND0_CONTROL + (uint32)reg->index), reg->reg);
	}

	void GX2SetBlendControl(uint32 renderTargetIndex, GX2_BLENDFACTOR colorSrcFactor, GX2_BLENDFACTOR colorDstFactor, GX2_BLENDFUNC colorCombineFunc, uint32 separateAlphaBlend, GX2_BLENDFACTOR alphaSrcFactor, GX2_BLENDFACTOR alphaDstFactor, GX2_BLENDFUNC alphaCombineFunc)
	{
		GX2BlendControlReg r;
		GX2InitBlendControlReg(&r, renderTargetIndex, colorSrcFactor, colorDstFactor, colorCombineFunc, separateAlphaBlend, alphaSrcFactor, alphaDstFactor, alphaCombineFunc);
		GX2SetBlendControlReg(&r);
	}

	void GX2InitBlendConstantColorReg(GX2BlendConstantColorReg* reg, float red, float green, float blue, float alpha)
	{
		reg->regRed = LATTE_CB_BLEND_RED().set_RED(red);
		reg->regGreen = LATTE_CB_BLEND_GREEN().set_GREEN(green);
		reg->regBlue = LATTE_CB_BLEND_BLUE().set_BLUE(blue);
		reg->regAlpha = LATTE_CB_BLEND_ALPHA().set_ALPHA(alpha);
	}

	void GX2SetBlendConstantColorReg(GX2BlendConstantColorReg* reg)
	{
		_SetContextRegs(REGADDR::CB_BLEND_RED, reg->regRed, reg->regGreen, reg->regBlue, reg->regAlpha);
	}

	void GX2SetBlendConstantColor(float red, float green, float blue, float alpha)
	{
		GX2BlendConstantColorReg r;
		GX2InitBlendConstantColorReg(&r, red, green, blue, alpha);
		GX2SetBlendConstantColorReg(&r);
	}

	void GX2InitHiStencilInfoRegs(GX2HiStencilInfoReg* hiStencilInfo)
	{
		// hi-stencil is an optimization only, nothing needs to be prepared
	}

	/* points */

	void GX2InitPointSizeReg(GX2PointSizeReg* reg, float width, float height)
	{
		reg->reg = LATTE_PA_SU_POINT_SIZE().set_WIDTH(_PointSizeToFixed(width)).set_HEIGHT(_PointSizeToFixed(height));
	}

	void GX2SetPointSizeReg(GX2PointSizeReg* reg)
	{
		_SetContextRegs(REGADDR::PA_SU_POINT_SIZE, reg->reg);
	}

	void GX2SetPointSize(float width, float height)
	{
		GX2PointSizeReg r;
		GX2InitPointSizeReg(&r, width, height);
		GX2SetPointSizeReg(&r);
	}

	void GX2InitPointLimitsReg(GX2PointLimitsReg* reg, float minSize, float maxSize)
	{
		reg->reg = LATTE_PA_SU_POINT_MINMAX().set_MIN_SIZE(_PointSizeToFixed(minSize)).set_MAX_SIZE(_PointSizeToFixed(maxSize));
	}

	void GX2SetPointLimitsReg(GX2PointLimitsReg* reg)
	{
		_SetContextRegs(REGADDR::PA_SU_POINT_MINMAX, reg->reg);
	}

	void GX2SetPointLimits(float minSize, float maxSize)
	{
		GX2PointLimitsReg r;
		GX2InitPointLimitsReg(&r, minSize, maxSize);
		GX2SetPointLimitsReg(&r);
	}

	/* special states (used by GX2 internally, e.g. for fast clears) */

	enum class GX2_SPECIAL_STATE : uint32
	{
		FAST_CLEAR = 0,
		FAST_CLEAR_HIZ = 1,
	};

	static void _SetFastClearState(bool enable)
	{
		GX2ReserveCmdSpace(6);
		if (!enable)
		{
			// back to regular viewport transform (VTE 0x43F) and clipping
			LATTE_PA_CL_VTE_CNTL vte{};
			vte.set_VPORT_X_SCALE_ENA(true).set_VPORT_X_OFFSET_ENA(true);
			vte.set_VPORT_Y_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true);
			vte.set_VPORT_Z_SCALE_ENA(true).set_VPORT_Z_OFFSET_ENA(true);
			vte.set_VTX_W0_FMT(true);
			_EmitContextRegs(REGADDR::PA_CL_VTE_CNTL, vte);
			GX2SetRasterizerClipControl(true, true);
			return;
		}
		// vertices are passed through in screen space (VTE 0x300), no clipping, no rasterization (CLIP 0x490000)
		LATTE_PA_CL_VTE_CNTL vte{};
		vte.set_VTX_XY_FMT(true).set_VTX_Z_FMT(true);
		LATTE_PA_CL_CLIP_CNTL clip{};
		clip.set_CLIP_DISABLE(true).set_DX_CLIP_SPACE_DEF(true).set_DX_RASTERIZATION_KILL(true);
		_EmitContextRegs(REGADDR::PA_CL_VTE_CNTL, vte);
		_EmitContextRegs(REGADDR::PA_CL_CLIP_CNTL, clip);
	}

	void GX2SetSpecialState(GX2_SPECIAL_STATE stateId, uint32 isEnabled)
	{
		switch (stateId)
		{
		case GX2_SPECIAL_STATE::FAST_CLEAR:
			_SetFastClearState(isEnabled != 0);
			break;
		case GX2_SPECIAL_STATE::FAST_CLEAR_HIZ:
			// only adds hi-z related bits on top of FAST_CLEAR; not needed for emulation
			break;
		default:
			// any other state is forwarded to the command processor as an HLE packet
			GX2ReserveCmdSpace(3);
			gx2WriteGather_submit(pm4HeaderType3(IT_HLE_SPECIAL_STATE, 2), (uint32)stateId, isEnabled);
			break;
		}
	}

	void GX2StateInit()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("gx2", fn, LogType::GX2)
		EXPORT_FN(GX2InitAlphaTestReg); EXPORT_FN(GX2SetAlphaTestReg); EXPORT_FN(GX2SetAlphaTest);

		EXPORT_FN(GX2InitColorControlReg); EXPORT_FN(GX2SetColorControl); EXPORT_FN(GX2SetColorControlReg);

		EXPORT_FN(GX2InitPolygonControlReg); EXPORT_FN(GX2SetPolygonControlReg); EXPORT_FN(GX2SetPolygonControl);
		EXPORT_FN(GX2SetCullOnlyControl);

		EXPORT_FN(GX2InitPolygonOffsetReg); EXPORT_FN(GX2SetPolygonOffsetReg); EXPORT_FN(GX2SetPolygonOffset);

		EXPORT_FN(GX2SetRasterizerClipControl); EXPORT_FN(GX2SetRasterizerClipControlHalfZ);
		EXPORT_FN(GX2SetRasterizerClipControlEx);

		EXPORT_FN(GX2InitViewportReg); EXPORT_FN(GX2SetViewportReg); EXPORT_FN(GX2SetViewport);

		EXPORT_FN(GX2InitScissorReg); EXPORT_FN(GX2SetScissorReg); EXPORT_FN(GX2GetScissorReg);
		EXPORT_FN(GX2SetScissor);

		EXPORT_FN(GX2InitDepthStencilControlReg); EXPORT_FN(GX2SetDepthStencilControlReg);
		EXPORT_FN(GX2GetDepthStencilControlReg); EXPORT_FN(GX2SetDepthOnlyControl); EXPORT_FN(GX2SetDepthStencilControl);

		EXPORT_FN(GX2InitStencilMaskReg); EXPORT_FN(GX2SetStencilMask); EXPORT_FN(GX2SetStencilMaskReg);

		EXPORT_FN(GX2SetPrimitiveRestartIndex);

		EXPORT_FN(GX2InitTargetChannelMasksReg); EXPORT_FN(GX2SetTargetChannelMasksReg); EXPORT_FN(GX2SetTargetChannelMasks);
		EXPORT_FN(GX2GetTargetChannelMasksReg);

		EXPORT_FN(GX2InitBlendControlReg); EXPORT_FN(GX2SetBlendControlReg); EXPORT_FN(GX2SetBlendControl);

		EXPORT_FN(GX2InitBlendConstantColorReg); EXPORT_FN(GX2SetBlendConstantColorReg); EXPORT_FN(GX2SetBlendConstantColor);
		EXPORT_FN(GX2InitHiStencilInfoRegs);

		EXPORT_FN(GX2InitPointSizeReg); EXPORT_FN(GX2SetPointSizeReg); EXPORT_FN(GX2SetPointSize);
		EXPORT_FN(GX2InitPointLimitsReg); EXPORT_FN(GX2SetPointLimitsReg); EXPORT_FN(GX2SetPointLimits);

		EXPORT_FN(GX2SetSpecialState);
#undef EXPORT_FN
	}
}
