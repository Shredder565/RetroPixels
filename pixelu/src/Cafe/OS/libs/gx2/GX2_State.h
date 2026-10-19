// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"

// Pre-built register values for render state. Games fill these with GX2Init*Reg() and
// later emit them with GX2Set*Reg(); the layouts below are the guest (big endian) layouts.

namespace GX2
{
	using GX2_ALPHAFUNC = Latte::LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC;
	using GX2_LOGICOP = Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP;
	using GX2_CHANNELMASK = uint32; // 4 bits per render target (RGBA)
	using GX2_BLENDFACTOR = Latte::LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR;
	using GX2_BLENDFUNC = Latte::LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC;

	// SX_ALPHA_TEST_CONTROL + SX_ALPHA_REF
	struct GX2AlphaTestReg
	{
		betype<Latte::LATTE_SX_ALPHA_TEST_CONTROL> regAlphaTestControl;
		betype<Latte::LATTE_SX_ALPHA_REF> regAlphaTestRef;
	};
	static_assert(sizeof(GX2AlphaTestReg) == 0x08);

	// CB_COLOR_CONTROL
	struct GX2ColorControlReg
	{
		betype<Latte::LATTE_CB_COLOR_CONTROL> reg;
	};
	static_assert(sizeof(GX2ColorControlReg) == 0x04);

	// PA_SU_SC_MODE_CNTL
	struct GX2PolygonControlReg
	{
		betype<Latte::LATTE_PA_SU_SC_MODE_CNTL> reg;
	};
	static_assert(sizeof(GX2PolygonControlReg) == 0x04);

	// PA_SU_POLY_OFFSET_FRONT_SCALE .. BACK_OFFSET (consecutive) and PA_SU_POLY_OFFSET_CLAMP
	struct GX2PolygonOffsetReg
	{
		/* +0x00 */ betype<Latte::LATTE_PA_SU_POLY_OFFSET_FRONT_SCALE> regFrontScale;
		/* +0x04 */ betype<Latte::LATTE_PA_SU_POLY_OFFSET_FRONT_OFFSET> regFrontOffset;
		/* +0x08 */ betype<Latte::LATTE_PA_SU_POLY_OFFSET_BACK_SCALE> regBackScale;
		/* +0x0C */ betype<Latte::LATTE_PA_SU_POLY_OFFSET_BACK_OFFSET> regBackOffset;
		/* +0x10 */ betype<Latte::LATTE_PA_SU_POLY_OFFSET_CLAMP> regClamp;
	};
	static_assert(sizeof(GX2PolygonOffsetReg) == 0x14);

	// DB_DEPTH_CONTROL
	struct GX2DepthStencilControlReg
	{
		betype<Latte::LATTE_DB_DEPTH_CONTROL> reg;
	};
	static_assert(sizeof(GX2DepthStencilControlReg) == 0x04);

	// DB_STENCILREFMASK + DB_STENCILREFMASK_BF
	struct GX2StencilMaskReg
	{
		betype<Latte::LATTE_DB_STENCILREFMASK> stencilRefMaskFrontReg;
		betype<Latte::LATTE_DB_STENCILREFMASK_BF> stencilRefMaskBackReg;
	};
	static_assert(sizeof(GX2StencilMaskReg) == 0x08);

	// CB_TARGET_MASK
	struct GX2TargetChannelMaskReg
	{
		betype<Latte::LATTE_CB_TARGET_MASK> reg;
	};
	static_assert(sizeof(GX2TargetChannelMaskReg) == 0x04);

	// one hi-stencil compare state (front or back); contents are not interpreted by us
	struct GX2HIStencilInfoData
	{
		/* +0x00 */ uint32be ukn00;
		/* +0x04 */ uint8be ukn04;
		/* +0x05 */ uint8be ukn05;
		/* +0x06 */ uint8be ukn06;
		/* +0x07 */ uint8be ukn07;
		/* +0x08 */ uint32be isEnable;
	};
	static_assert(sizeof(GX2HIStencilInfoData) == 0x0C);

	struct GX2HiStencilInfoReg
	{
		/* +0x00 */ GX2HIStencilInfoData state[2];
		/* +0x18 */ uint32be reg[2]; // DB_SRESULTS_COMPARE_STATE0/1
	};
	static_assert(sizeof(GX2HiStencilInfoReg) == 0x20);

	// CB_BLEND0_CONTROL + index
	struct GX2BlendControlReg
	{
		/* +0x00 */ uint32be index; // render target
		/* +0x04 */ betype<Latte::LATTE_CB_BLENDN_CONTROL> reg;
	};
	static_assert(sizeof(GX2BlendControlReg) == 0x08);

	// CB_BLEND_RED .. CB_BLEND_ALPHA
	struct GX2BlendConstantColorReg
	{
		betype<Latte::LATTE_CB_BLEND_RED> regRed;
		betype<Latte::LATTE_CB_BLEND_GREEN> regGreen;
		betype<Latte::LATTE_CB_BLEND_BLUE> regBlue;
		betype<Latte::LATTE_CB_BLEND_ALPHA> regAlpha;
	};
	static_assert(sizeof(GX2BlendConstantColorReg) == 0x10);

	// PA_SU_POINT_SIZE
	struct GX2PointSizeReg
	{
		betype<Latte::LATTE_PA_SU_POINT_SIZE> reg;
	};
	static_assert(sizeof(GX2PointSizeReg) == 0x04);

	// PA_SU_POINT_MINMAX
	struct GX2PointLimitsReg
	{
		betype<Latte::LATTE_PA_SU_POINT_MINMAX> reg;
	};
	static_assert(sizeof(GX2PointLimitsReg) == 0x04);

	// PA_CL_VPORT_XSCALE .. ZOFFSET, followed by guard band values we leave alone
	struct GX2ViewportReg
	{
		/* +0x00 */ betype<Latte::LATTE_PA_CL_VPORT_XSCALE> xScale;
		/* +0x04 */ betype<Latte::LATTE_PA_CL_VPORT_XOFFSET> xOffset;
		/* +0x08 */ betype<Latte::LATTE_PA_CL_VPORT_YSCALE> yScale;
		/* +0x0C */ betype<Latte::LATTE_PA_CL_VPORT_YOFFSET> yOffset;
		/* +0x10 */ betype<Latte::LATTE_PA_CL_VPORT_ZSCALE> zScale;
		/* +0x14 */ betype<Latte::LATTE_PA_CL_VPORT_ZOFFSET> zOffset;
		/* +0x18 */ uint32 ukn[6];
	};
	static_assert(sizeof(GX2ViewportReg) == 0x30);

	// PA_SC_GENERIC_SCISSOR_TL + _BR
	struct GX2ScissorReg
	{
		betype<Latte::LATTE_PA_SC_GENERIC_SCISSOR_TL> scissorTL;
		betype<Latte::LATTE_PA_SC_GENERIC_SCISSOR_BR> scissorBR;
	};
	static_assert(sizeof(GX2ScissorReg) == 0x08);

	// alpha test
	void GX2InitAlphaTestReg(GX2AlphaTestReg* reg, uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef);
	void GX2SetAlphaTestReg(GX2AlphaTestReg* reg);
	void GX2SetAlphaTest(uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef);

	// color control
	void GX2InitColorControlReg(GX2ColorControlReg* reg, GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable);
	void GX2SetColorControl(GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable);
	void GX2SetColorControlReg(GX2ColorControlReg* reg);

	// polygon control / culling
	void GX2InitPolygonControlReg(GX2PolygonControlReg* reg,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE usePolygonMode,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeFront,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeBack,
		uint32 polygonOffsetFrontEnable, uint32 polygonOffsetBackEnable, uint32 paraOffsetEnable);
	void GX2SetPolygonControlReg(GX2PolygonControlReg* reg);
	void GX2SetPolygonControl(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE usePolygonMode,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeFront,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeBack,
		uint32 polygonOffsetFrontEnable, uint32 polygonOffsetBackEnable, uint32 paraOffsetEnable);
	void GX2SetCullOnlyControl(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack);

	// polygon offset
	void GX2InitPolygonOffsetReg(GX2PolygonOffsetReg* reg, float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset);
	void GX2SetPolygonOffsetReg(GX2PolygonOffsetReg* reg);
	void GX2SetPolygonOffset(float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset);

	// points
	void GX2InitPointSizeReg(GX2PointSizeReg* reg, float width, float height);
	void GX2SetPointSizeReg(GX2PointSizeReg* reg);
	void GX2SetPointSize(float width, float height);
	void GX2InitPointLimitsReg(GX2PointLimitsReg* reg, float minSize, float maxSize);
	void GX2SetPointLimitsReg(GX2PointLimitsReg* reg);
	void GX2SetPointLimits(float minSize, float maxSize);

	// clipping
	void GX2SetRasterizerClipControl(bool enableRasterizer, bool enableZClip);
	void GX2SetRasterizerClipControlHalfZ(bool enableRasterizer, bool enableZClip, bool enableHalfZ);
	void GX2SetRasterizerClipControlEx(bool enableRasterizer, bool enableZClip, bool enableHalfZ);

	void GX2SetPrimitiveRestartIndex(uint32 restartIndex);

	// render target write masks
	void GX2InitTargetChannelMasksReg(GX2TargetChannelMaskReg* reg, GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7);
	void GX2SetTargetChannelMasksReg(GX2TargetChannelMaskReg* reg);
	void GX2SetTargetChannelMasks(GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7);

	// blending
	void GX2InitBlendControlReg(GX2BlendControlReg* reg, uint32 renderTargetIndex, GX2_BLENDFACTOR colorSrcFactor, GX2_BLENDFACTOR colorDstFactor, GX2_BLENDFUNC colorCombineFunc, uint32 separateAlphaBlend, GX2_BLENDFACTOR alphaSrcFactor, GX2_BLENDFACTOR alphaDstFactor, GX2_BLENDFUNC alphaCombineFunc);
	void GX2SetBlendControlReg(GX2BlendControlReg* reg);
	void GX2SetBlendControl(uint32 renderTargetIndex, GX2_BLENDFACTOR colorSrcFactor, GX2_BLENDFACTOR colorDstFactor, GX2_BLENDFUNC colorCombineFunc, uint32 separateAlphaBlend, GX2_BLENDFACTOR alphaSrcFactor, GX2_BLENDFACTOR alphaDstFactor, GX2_BLENDFUNC alphaCombineFunc);

	void GX2InitBlendConstantColorReg(GX2BlendConstantColorReg* reg, float red, float green, float blue, float alpha);
	void GX2SetBlendConstantColorReg(GX2BlendConstantColorReg* reg);
	void GX2SetBlendConstantColor(float red, float green, float blue, float alpha);

	void GX2StateInit();
}
