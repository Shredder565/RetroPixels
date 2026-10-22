// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Latte (R7xx-class) GPU register file: shared enums, register indices,
// typed register views and the LatteContextRegister block that mirrors the
// whole register space (one uint32 per register index).

namespace Latte
{
	/* ---------------------------------------------------------------------
	   Surface related enums (also used by the GX2 library)
	--------------------------------------------------------------------- */

	// texture resource dimension (SQ_TEX_RESOURCE_WORD0.DIM)
	enum class E_DIM : uint32
	{
		DIM_1D = 0,
		DIM_2D = 1,
		DIM_3D = 2,
		DIM_CUBEMAP = 3,
		DIM_1D_ARRAY = 4,
		DIM_2D_ARRAY = 5,
		DIM_2D_MSAA = 6,
		DIM_2D_ARRAY_MSAA = 7
	};

	// sample count as log2
	enum class E_AAMODE : uint32
	{
		AA_1X = 0,
		AA_2X = 1,
		AA_4X = 2,
		AA_8X = 3,
	};

	// array modes understood by the hardware
	// 1D = micro tiled only, 2D/3D = macro tiled, xB = bank swapped variant
	// THIN1/2/4 select the macro tile aspect, THICK uses 4 slices per micro tile
	enum class E_HWTILEMODE
	{
		TM_LINEAR_GENERAL = 0,
		TM_LINEAR_ALIGNED = 1,
		TM_1D_TILED_THIN1 = 2,
		TM_1D_TILED_THICK = 3,
		TM_2D_TILED_THIN1 = 4,
		TM_2D_TILED_THIN2 = 5,
		TM_2D_TILED_THIN4 = 6,
		TM_2D_TILED_THICK = 7,
		TM_2B_TILED_THIN1 = 8,
		TM_2B_TILED_THIN2 = 9,
		TM_2B_TILED_THIN4 = 10,
		TM_2B_TILED_THICK = 11,
		TM_3D_TILED_THIN1 = 12,
		TM_3D_TILED_THICK = 13,
		TM_3B_TILED_THIN1 = 14,
		TM_3B_TILED_THICK = 15,
	};

	// GX2 tile mode: the hardware modes plus two GX2-only values
	enum class E_GX2TILEMODE : uint32
	{
		TM_LINEAR_GENERAL = 0,
		TM_LINEAR_ALIGNED = 1,
		TM_1D_TILED_THIN1 = 2,
		TM_1D_TILED_THICK = 3,
		TM_2D_TILED_THIN1 = 4,
		TM_2D_TILED_THIN2 = 5,
		TM_2D_TILED_THIN4 = 6,
		TM_2D_TILED_THICK = 7,
		TM_2B_TILED_THIN1 = 8,
		TM_2B_TILED_THIN2 = 9,
		TM_2B_TILED_THIN4 = 10,
		TM_2B_TILED_THICK = 11,
		TM_3D_TILED_THIN1 = 12,
		TM_3D_TILED_THICK = 13,
		TM_3B_TILED_THIN1 = 14,
		TM_3B_TILED_THICK = 15,
		TM_LINEAR_SPECIAL = 16, // GX2 "default" linear layout, no alignment
		TM_32_SPECIAL = 32,
	};

	inline E_HWTILEMODE MakeHWTileMode(const E_GX2TILEMODE gx2Tilemode)
	{
		return static_cast<E_HWTILEMODE>(static_cast<uint32>(gx2Tilemode) & 0xFu);
	}

	inline E_GX2TILEMODE MakeGX2TileMode(const E_HWTILEMODE hwTilemode)
	{
		return static_cast<E_GX2TILEMODE>(hwTilemode);
	}

	inline bool TM_IsMacroTiled(const E_HWTILEMODE tm)
	{
		return static_cast<uint32>(tm) >= static_cast<uint32>(E_HWTILEMODE::TM_2D_TILED_THIN1);
	}

	inline bool TM_IsMacroTiled(const E_GX2TILEMODE tm)
	{
		const uint32 m = static_cast<uint32>(tm);
		return m >= 4 && m != static_cast<uint32>(E_GX2TILEMODE::TM_LINEAR_SPECIAL);
	}

	inline bool TM_IsBankSwapped(const E_HWTILEMODE tileMode)
	{
		switch (tileMode)
		{
		case E_HWTILEMODE::TM_2B_TILED_THIN1:
		case E_HWTILEMODE::TM_2B_TILED_THIN2:
		case E_HWTILEMODE::TM_2B_TILED_THIN4:
		case E_HWTILEMODE::TM_2B_TILED_THICK:
		case E_HWTILEMODE::TM_3B_TILED_THIN1:
		case E_HWTILEMODE::TM_3B_TILED_THICK:
			return true;
		default:
			return false;
		}
	}

	// hardware data formats (6 bit), shared by texture resources and vertex fetches
	enum class E_HWFMT : uint32
	{
		INVALID_FORMAT = 0x00,
		HWFMT_8 = 0x01,
		HWFMT_4_4 = 0x02,
		HWFMT_3_3_2 = 0x03,
		HWFMT_16 = 0x05,
		HWFMT_16_FLOAT = 0x06,
		HWFMT_8_8 = 0x07,
		HWFMT_5_6_5 = 0x08,
		HWFMT_6_5_5 = 0x09,
		HWFMT_1_5_5_5 = 0x0A,
		HWFMT_4_4_4_4 = 0x0B,
		HWFMT_5_5_5_1 = 0x0C,
		HWFMT_32 = 0x0D,
		HWFMT_32_FLOAT = 0x0E,
		HWFMT_16_16 = 0x0F,
		HWFMT_16_16_FLOAT = 0x10,
		HWFMT_8_24 = 0x11,
		HWFMT_8_24_FLOAT = 0x12,
		HWFMT_24_8 = 0x13,
		HWFMT_24_8_FLOAT = 0x14,
		HWFMT_10_11_11 = 0x15,
		HWFMT_10_11_11_FLOAT = 0x16,
		HWFMT_11_11_10 = 0x17,
		HWFMT_11_11_10_FLOAT = 0x18,
		HWFMT_2_10_10_10 = 0x19,
		HWFMT_8_8_8_8 = 0x1A,
		HWFMT_10_10_10_2 = 0x1B,
		HWFMT_X24_8_32_FLOAT = 0x1C,
		HWFMT_32_32 = 0x1D,
		HWFMT_32_32_FLOAT = 0x1E,
		HWFMT_16_16_16_16 = 0x1F,
		HWFMT_16_16_16_16_FLOAT = 0x20,
		HWFMT_32_32_32_32 = 0x22,
		HWFMT_32_32_32_32_FLOAT = 0x23,
		// 3-component formats, only meaningful for vertex fetches
		HWFMT_8_8_8 = 0x2C,
		HWFMT_16_16_16 = 0x2D,
		HWFMT_16_16_16_FLOAT = 0x2E,
		HWFMT_32_32_32 = 0x2F,
		HWFMT_32_32_32_FLOAT = 0x30,
		// block compressed
		HWFMT_BC1 = 0x31,
		HWFMT_BC2 = 0x32,
		HWFMT_BC3 = 0x33,
		HWFMT_BC4 = 0x34,
		HWFMT_BC5 = 0x35,
	};

	// GX2 surface format: hardware format in bits 0-5 plus GX2 type bits
	enum class E_GX2SURFFMT
	{
		INVALID_FORMAT = 0x00,
		HWFMT_8 = 0x01,
		HWFMT_4_4 = 0x02,
		HWFMT_3_3_2 = 0x03,
		HWFMT_16 = 0x05,
		HWFMT_16_FLOAT = 0x06,
		HWFMT_8_8 = 0x07,
		HWFMT_5_6_5 = 0x08,
		HWFMT_6_5_5 = 0x09,
		HWFMT_1_5_5_5 = 0x0A,
		HWFMT_4_4_4_4 = 0x0B,
		HWFMT_5_5_5_1 = 0x0C,
		HWFMT_32 = 0x0D,
		HWFMT_32_FLOAT = 0x0E,
		HWFMT_16_16 = 0x0F,
		HWFMT_16_16_FLOAT = 0x10,
		HWFMT_8_24 = 0x11,
		HWFMT_8_24_FLOAT = 0x12,
		HWFMT_24_8 = 0x13,
		HWFMT_24_8_FLOAT = 0x14,
		HWFMT_10_11_11 = 0x15,
		HWFMT_10_11_11_FLOAT = 0x16,
		HWFMT_11_11_10 = 0x17,
		HWFMT_11_11_10_FLOAT = 0x18,
		HWFMT_2_10_10_10 = 0x19,
		HWFMT_8_8_8_8 = 0x1A,
		HWFMT_10_10_10_2 = 0x1B,
		HWFMT_X24_8_32_FLOAT = 0x1C,
		HWFMT_32_32 = 0x1D,
		HWFMT_32_32_FLOAT = 0x1E,
		HWFMT_16_16_16_16 = 0x1F,
		HWFMT_16_16_16_16_FLOAT = 0x20,
		HWFMT_32_32_32_32 = 0x22,
		HWFMT_32_32_32_32_FLOAT = 0x23,
		HWFMT_BC1 = 0x31,
		HWFMT_BC2 = 0x32,
		HWFMT_BC3 = 0x33,
		HWFMT_BC4 = 0x34,
		HWFMT_BC5 = 0x35,

		// type bits added by GX2
		FMT_BIT_INT = 0x100,
		FMT_BIT_SIGNED = 0x200,
		FMT_BIT_SRGB = 0x400,
		FMT_BIT_FLOAT = 0x800,

		// packed small formats
		R4_G4_UNORM = 0x02,
		R5_G6_B5_UNORM = 0x08,
		R5_G5_B5_A1_UNORM = 0x0A,
		R4_G4_B4_A4_UNORM = 0x0B,
		A1_B5_G5_R5_UNORM = 0x0C,

		// 8 bit channels
		R8_UNORM = 0x01,
		R8_SNORM = 0x201,
		R8_UINT = 0x101,
		R8_SINT = 0x301,
		R8_G8_UNORM = 0x07,
		R8_G8_SNORM = 0x207,
		R8_G8_UINT = 0x107,
		R8_G8_SINT = 0x307,
		R8_G8_B8_A8_UNORM = 0x1A,
		R8_G8_B8_A8_SNORM = 0x21A,
		R8_G8_B8_A8_UINT = 0x11A,
		R8_G8_B8_A8_SINT = 0x31A,
		R8_G8_B8_A8_SRGB = 0x41A,

		// 10:10:10:2
		R10_G10_B10_A2_UNORM = 0x19,
		R10_G10_B10_A2_SNORM = 0x219,
		R10_G10_B10_A2_UINT = 0x119,
		R10_G10_B10_A2_SINT = 0x319,
		R10_G10_B10_A2_SRGB = 0x419,
		A2_B10_G10_R10_UNORM = 0x1B,
		A2_B10_G10_R10_UINT = 0x11B,

		// 16 bit channels
		R16_UNORM = 0x05,
		R16_SNORM = 0x205,
		R16_UINT = 0x105,
		R16_SINT = 0x305,
		R16_FLOAT = 0x806,
		R16_G16_UNORM = 0x0F,
		R16_G16_SNORM = 0x20F,
		R16_G16_UINT = 0x10F,
		R16_G16_SINT = 0x30F,
		R16_G16_FLOAT = 0x810,
		R16_G16_B16_A16_UNORM = 0x1F,
		R16_G16_B16_A16_SNORM = 0x21F,
		R16_G16_B16_A16_UINT = 0x11F,
		R16_G16_B16_A16_SINT = 0x31F,
		R16_G16_B16_A16_FLOAT = 0x820,

		// depth/stencil views as colour
		R24_X8_UNORM = 0x11,
		R24_X8_FLOAT = 0x811,
		X24_G8_UINT = 0x111,
		R32_X8_FLOAT = 0x81C,
		X32_G8_UINT_X24 = 0x11C,

		R11_G11_B10_FLOAT = 0x816,

		// 32 bit channels (integer and float only)
		R32_UINT = 0x10D,
		R32_SINT = 0x30D,
		R32_FLOAT = 0x80E,
		R32_G32_UINT = 0x11D,
		R32_G32_SINT = 0x31D,
		R32_G32_FLOAT = 0x81E,
		R32_G32_B32_A32_UINT = 0x122,
		R32_G32_B32_A32_SINT = 0x322,
		R32_G32_B32_A32_FLOAT = 0x823,

		// depth buffers
		D24_S8_UNORM = 0x11,
		D24_S8_FLOAT = 0x811,
		D32_S8_FLOAT = 0x81C,
		D16_UNORM = 0x05,
		D32_FLOAT = 0x80E,

		// block compressed
		BC1_UNORM = 0x31,
		BC1_SRGB = 0x431,
		BC2_UNORM = 0x32,
		BC2_SRGB = 0x432,
		BC3_UNORM = 0x33,
		BC3_SRGB = 0x433,
		BC4_UNORM = 0x34,
		BC4_SNORM = 0x234,
		BC5_UNORM = 0x35,
		BC5_SNORM = 0x235,

		// video (two planes)
		NV12_UNORM = 0x81,
	};
	DEFINE_ENUM_FLAG_OPERATORS(E_GX2SURFFMT);

	// bits per element of each hardware format (0 = not a surface format)
	inline uint32 GetFormatBits(const Latte::E_HWFMT hwFmt)
	{
		static constexpr uint8 kElementBits[64] =
		{
			// 0x00 - 0x0F
			0, 8, 8, 0, 0, 16, 16, 16, 16, 16, 16, 16, 16, 32, 32, 32,
			// 0x10 - 0x1F
			32, 32, 0, 32, 0, 0, 32, 0, 0, 32, 32, 32, 64, 64, 64, 64,
			// 0x20 - 0x2F
			64, 0, 128, 128, 0, 0, 0, 16, 16, 32, 32, 32, 0, 0, 0, 96,
			// 0x30 - 0x3F
			96, 64, 128, 128, 64, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		};
		const uint32 index = static_cast<uint32>(hwFmt);
		cemu_assert(index < 64);
		return kElementBits[index];
	}

	inline uint32 GetFormatBits(const Latte::E_GX2SURFFMT gx2Fmt)
	{
		return GetFormatBits(static_cast<Latte::E_HWFMT>(static_cast<uint32>(gx2Fmt) & 0x3Fu));
	}

	inline E_HWFMT GetHWFormat(E_GX2SURFFMT format)
	{
		return static_cast<E_HWFMT>(static_cast<uint32>(format) & 0x3Fu);
	}

	inline bool IsCompressedFormat(Latte::E_HWFMT format)
	{
		const uint32 f = static_cast<uint32>(format);
		return f >= static_cast<uint32>(E_HWFMT::HWFMT_BC1) && f <= static_cast<uint32>(E_HWFMT::HWFMT_BC5);
	}

	inline bool IsCompressedFormat(Latte::E_GX2SURFFMT format)
	{
		return IsCompressedFormat(GetHWFormat(format));
	}

	inline bool IsMSAA(Latte::E_DIM dim)
	{
		return dim == E_DIM::DIM_2D_MSAA || dim == E_DIM::DIM_2D_ARRAY_MSAA;
	}

	/* ---------------------------------------------------------------------
	   PM4 SURFACE_SYNC coherency control bits (CP_COHER_CNTL)
	--------------------------------------------------------------------- */
	enum class E_COHER_CNTL : uint32
	{
		DEST_BASE_0_ENA = 0x00000001,
		DEST_BASE_1_ENA = 0x00000002,
		SO0_DEST_BASE_ENA = 0x00000004,
		SO1_DEST_BASE_ENA = 0x00000008,
		SO2_DEST_BASE_ENA = 0x00000010,
		SO3_DEST_BASE_ENA = 0x00000020,
		CB0_DEST_BASE_ENA = 0x00000040,
		CB1_DEST_BASE_ENA = 0x00000080,
		CB2_DEST_BASE_ENA = 0x00000100,
		CB3_DEST_BASE_ENA = 0x00000200,
		CB4_DEST_BASE_ENA = 0x00000400,
		CB5_DEST_BASE_ENA = 0x00000800,
		CB6_DEST_BASE_ENA = 0x00001000,
		CB7_DEST_BASE_ENA = 0x00002000,
		CB_ALL_DEST_BASE_ENA = 0x00003FC0, // all eight colour buffers
		DB_DEST_BASE_ENA = 0x00004000,
		FULL_CACHE_ENA = 0x00100000,
		TC_ACTION_ENA = 0x00800000, // texture cache (also drops cached vertex/uniform data on Latte)
		VC_ACTION_ENA = 0x01000000,
		CB_ACTION_ENA = 0x02000000,
		DB_ACTION_ENA = 0x04000000,
		SH_ACTION_ENA = 0x08000000,
		SX_ACTION_ENA = 0x10000000,
		ENGINE_ME = 0x80000000u,
	};
	DEFINE_ENUM_FLAG_OPERATORS(E_COHER_CNTL);

	enum GPU_LIMITS
	{
		NUM_VERTEX_BUFFERS = 16,
		NUM_TEXTURES_PER_STAGE = 18,
		NUM_SAMPLERS_PER_STAGE = 18,
		NUM_COLOR_ATTACHMENTS = 8,
	};

	/* ---------------------------------------------------------------------
	   Register indices (dword index into the register file)
	   0x2000-0x3FFF config, 0xA000-0xAFFF context, 0xE000-0xEFFF resources,
	   0xF000-0xF0FF samplers
	--------------------------------------------------------------------- */
	enum REGADDR
	{
		// config registers
		VGT_PRIMITIVE_TYPE = 0x2256,
		SQ_CONFIG = 0x2300,
		SQ_GPR_RESOURCE_MGMT_1 = 0x2301,
		SQ_GPR_RESOURCE_MGMT_2 = 0x2302,
		SQ_THREAD_RESOURCE_MGMT = 0x2303,
		SQ_STACK_RESOURCE_MGMT_1 = 0x2304,
		SQ_STACK_RESOURCE_MGMT_2 = 0x2305,
		SQ_ESGS_RING_BASE = 0x2310,
		SQ_ESGS_RING_SIZE = 0x2311,
		SQ_GSVS_RING_BASE = 0x2312,
		SQ_GSVS_RING_SIZE = 0x2313,

		// sampler border colours, four floats per sampler and stage
		TD_PS_SAMPLER0_BORDER_RED = 0x2900,
		TD_PS_SAMPLER0_BORDER_GREEN = 0x2901,
		TD_PS_SAMPLER0_BORDER_BLUE = 0x2902,
		TD_PS_SAMPLER0_BORDER_ALPHA = 0x2903,
		TD_VS_SAMPLER0_BORDER_RED = 0x2980,
		TD_VS_SAMPLER0_BORDER_GREEN = 0x2981,
		TD_VS_SAMPLER0_BORDER_BLUE = 0x2982,
		TD_VS_SAMPLER0_BORDER_ALPHA = 0x2983,
		TD_GS_SAMPLER0_BORDER_RED = 0x2A00,
		TD_GS_SAMPLER0_BORDER_GREEN = 0x2A01,
		TD_GS_SAMPLER0_BORDER_BLUE = 0x2A02,
		TD_GS_SAMPLER0_BORDER_ALPHA = 0x2A03,

		// context registers
		DB_STENCIL_CLEAR = 0xA00A,
		DB_DEPTH_CLEAR = 0xA00B,
		CB_TARGET_MASK = 0xA08E,
		PA_SC_GENERIC_SCISSOR_TL = 0xA090,
		PA_SC_GENERIC_SCISSOR_BR = 0xA091,
		SQ_VTX_SEMANTIC_0 = 0xA0E0,
		SQ_VTX_SEMANTIC_31 = 0xA0FF,
		VGT_MULTI_PRIM_IB_RESET_INDX = 0xA103,
		SX_ALPHA_TEST_CONTROL = 0xA104,
		CB_BLEND_RED = 0xA105,
		CB_BLEND_GREEN = 0xA106,
		CB_BLEND_BLUE = 0xA107,
		CB_BLEND_ALPHA = 0xA108,
		DB_STENCILREFMASK = 0xA10C,
		DB_STENCILREFMASK_BF = 0xA10D,
		SX_ALPHA_REF = 0xA10E,
		PA_CL_VPORT_XSCALE = 0xA10F,
		PA_CL_VPORT_XOFFSET = 0xA110,
		PA_CL_VPORT_YSCALE = 0xA111,
		PA_CL_VPORT_YOFFSET = 0xA112,
		PA_CL_VPORT_ZSCALE = 0xA113,
		PA_CL_VPORT_ZOFFSET = 0xA114,
		SPI_VS_OUT_ID_0 = 0xA185,
		SPI_VS_OUT_CONFIG = 0xA1B1,
		SPI_THREAD_GROUPING = 0xA1B2,
		CB_BLEND0_CONTROL = 0xA1E0,
		CB_BLEND7_CONTROL = 0xA1E7,
		DB_DEPTH_CONTROL = 0xA200,
		CB_COLOR_CONTROL = 0xA202,
		PA_CL_CLIP_CNTL = 0xA204,
		PA_SU_SC_MODE_CNTL = 0xA205,
		PA_CL_VTE_CNTL = 0xA206,
		PA_CL_VS_OUT_CNTL = 0xA207,
		SQ_PGM_START_PS = 0xA210,
		SQ_PGM_RESOURCES_PS = 0xA214,
		SQ_PGM_EXPORTS_PS = 0xA215,
		SQ_PGM_START_VS = 0xA216,
		SQ_PGM_RESOURCES_VS = 0xA21A,
		SQ_PGM_START_GS = 0xA21B,
		SQ_PGM_RESOURCES_GS = 0xA21F,
		SQ_PGM_START_ES = 0xA220,
		SQ_PGM_RESOURCES_ES = 0xA224,
		SQ_PGM_START_FS = 0xA225,
		SQ_PGM_RESOURCES_FS = 0xA229,
		SQ_ESGS_RING_ITEMSIZE = 0xA22A,
		SQ_GSVS_RING_ITEMSIZE = 0xA22B,
		SQ_VTX_SEMANTIC_CLEAR = 0xA238,
		PA_SU_POINT_SIZE = 0xA280,
		PA_SU_POINT_MINMAX = 0xA281,
		VGT_GS_MODE = 0xA290,
		VGT_DMA_INDEX_TYPE = 0xA29F,
		VGT_PRIMITIVEID_EN = 0xA2A1,
		VGT_DMA_NUM_INSTANCES = 0xA2A2,
		VGT_MULTI_PRIM_IB_RESET_EN = 0xA2A5,
		VGT_INSTANCE_STEP_RATE_0 = 0xA2A8,
		VGT_INSTANCE_STEP_RATE_1 = 0xA2A9,
		VGT_STRMOUT_EN = 0xA2AC,
		// stream-out buffers: SIZE, STRIDE, BASE, OFFSET per buffer
		VGT_STRMOUT_BUFFER_SIZE_0 = 0xA2B4,
		VGT_STRMOUT_VTX_STRIDE_0 = 0xA2B5,
		VGT_STRMOUT_BUFFER_BASE_0 = 0xA2B6,
		VGT_STRMOUT_BUFFER_OFFSET_0 = 0xA2B7,
		VGT_STRMOUT_BUFFER_SIZE_1 = 0xA2B8,
		VGT_STRMOUT_VTX_STRIDE_1 = 0xA2B9,
		VGT_STRMOUT_BUFFER_BASE_1 = 0xA2BA,
		VGT_STRMOUT_BUFFER_OFFSET_1 = 0xA2BB,
		VGT_STRMOUT_BUFFER_SIZE_2 = 0xA2BC,
		VGT_STRMOUT_VTX_STRIDE_2 = 0xA2BD,
		VGT_STRMOUT_BUFFER_BASE_2 = 0xA2BE,
		VGT_STRMOUT_BUFFER_OFFSET_2 = 0xA2BF,
		VGT_STRMOUT_BUFFER_SIZE_3 = 0xA2C0,
		VGT_STRMOUT_VTX_STRIDE_3 = 0xA2C1,
		VGT_STRMOUT_BUFFER_BASE_3 = 0xA2C2,
		VGT_STRMOUT_BUFFER_OFFSET_3 = 0xA2C3,
		VGT_STRMOUT_BASE_OFFSET_0 = 0xA2C4,
		VGT_STRMOUT_BASE_OFFSET_1 = 0xA2C5,
		VGT_STRMOUT_BASE_OFFSET_2 = 0xA2C6,
		VGT_STRMOUT_BASE_OFFSET_3 = 0xA2C7,
		VGT_STRMOUT_BUFFER_EN = 0xA2C8,
		DB_SRESULTS_COMPARE_STATE0 = 0xA34A,
		DB_SRESULTS_COMPARE_STATE1 = 0xA34B,
		PA_SU_POLY_OFFSET_CLAMP = 0xA37F,
		PA_SU_POLY_OFFSET_FRONT_SCALE = 0xA380,
		PA_SU_POLY_OFFSET_FRONT_OFFSET = 0xA381,
		PA_SU_POLY_OFFSET_BACK_SCALE = 0xA382,
		PA_SU_POLY_OFFSET_BACK_OFFSET = 0xA383,

		// texture resources, 7 words per unit
		SQ_TEX_RESOURCE_WORD0_N_PS = 0xE000,
		SQ_TEX_RESOURCE_WORD0_N_VS = 0xE460,
		SQ_TEX_RESOURCE_WORD0_N_GS = 0xE930,
		SQ_TEX_RESOURCE_WORD_FIRST = 0xE000,
		SQ_TEX_RESOURCE_WORD_LAST = 0xE930 + 18 * 7 - 1,

		// samplers, 3 words each; pixel, vertex and geometry stages use 18 each
		SQ_TEX_SAMPLER_WORD0_0 = 0xF000,
		SQ_TEX_SAMPLER_WORD1_0 = 0xF001,
		SQ_TEX_SAMPLER_WORD2_0 = 0xF002,
	};

	// first sampler index of each stage within the sampler register block
	inline constexpr int SAMPLER_BASE_INDEX_PIXEL = 0;
	inline constexpr int SAMPLER_BASE_INDEX_VERTEX = 18;
	inline constexpr int SAMPLER_BASE_INDEX_GEOMETRY = 36;

	/* ---------------------------------------------------------------------
	   Typed register views
	   Every view is a single uint32. Field accessors come in pairs:
	   get_NAME() and set_NAME(value), the setter returns the view so calls
	   can be chained.
	--------------------------------------------------------------------- */

	namespace regbits
	{
		constexpr uint32 FieldMask(uint32 width)
		{
			return width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
		}

		// value is placed as given; only the destination field is cleared first
		constexpr uint32 Place(uint32 word, uint32 shift, uint32 width, uint32 value)
		{
			return (word & ~(FieldMask(width) << shift)) | (value << shift);
		}

		constexpr uint32 Take(uint32 word, uint32 shift, uint32 width)
		{
			return (word >> shift) & FieldMask(width);
		}

		constexpr sint32 TakeSigned(uint32 word, uint32 shift, uint32 width)
		{
			const uint32 raw = Take(word, shift, width);
			const uint32 signBit = 1u << (width - 1);
			return static_cast<sint32>((raw ^ signBit) - signBit);
		}
	}

	class LATTEREG
	{
	public:
		uint32 getRawValue() const { return v; }
		uint32 getRawValueBE() const { return _swapEndianU32(v); }

	protected:
		uint32 v{};
	};

#define LREG_UINT(_name, _shift, _width) \
	auto& set_##_name(uint32 value) \
	{ \
		cemu_assert_debug(value <= regbits::FieldMask(_width)); \
		v = regbits::Place(v, _shift, _width, value); \
		return *this; \
	} \
	uint32 get_##_name() const { return regbits::Take(v, _shift, _width); }

#define LREG_SINT(_name, _shift, _width) \
	auto& set_##_name(sint32 value) \
	{ \
		cemu_assert_debug(value >= -(1 << ((_width) - 1)) && value < (1 << ((_width) - 1))); \
		v = regbits::Place(v, _shift, _width, static_cast<uint32>(value) & regbits::FieldMask(_width)); \
		return *this; \
	} \
	sint32 get_##_name() const { return regbits::TakeSigned(v, _shift, _width); }

#define LREG_BOOL(_name, _bit) \
	auto& set_##_name(bool value) \
	{ \
		v = regbits::Place(v, _bit, 1, value ? 1u : 0u); \
		return *this; \
	} \
	bool get_##_name() const { return regbits::Take(v, _bit, 1) != 0; }

#define LREG_ENUM(_name, _shift, _width, _type) \
	auto& set_##_name(_type value) \
	{ \
		cemu_assert_debug(static_cast<uint32>(value) <= regbits::FieldMask(_width)); \
		v = regbits::Place(v, _shift, _width, static_cast<uint32>(value)); \
		return *this; \
	} \
	_type get_##_name() const { return static_cast<_type>(regbits::Take(v, _shift, _width)); }

#define LREG_WORD(_name, _type) \
	auto& set_##_name(_type value) \
	{ \
		v = static_cast<uint32>(value); \
		return *this; \
	} \
	_type get_##_name() const { return static_cast<_type>(v); }

#define LREG_FLOAT(_name) \
	auto& set_##_name(float value) \
	{ \
		std::memcpy(&v, &value, sizeof(v)); \
		return *this; \
	} \
	float get_##_name() const \
	{ \
		float f; \
		std::memcpy(&f, &v, sizeof(f)); \
		return f; \
	}

	// comparison function shared by depth, stencil and alpha tests
	enum class E_COMPAREFUNC
	{
		NEVER,
		LESS,
		EQUAL,
		LEQUAL,
		GREATER,
		NOTEQUAL,
		GEQUAL,
		ALWAYS
	};

	enum class E_ENDIAN_SWAP
	{
		SWAP_NONE = 0,
	};

	/* config registers */

	struct LATTE_VGT_PRIMITIVE_TYPE : LATTEREG
	{
		enum class E_PRIMITIVE_TYPE
		{
			NONE = 0x00,
			POINTS = 0x01,
			LINES = 0x02,
			LINE_STRIP = 0x03,
			TRIANGLES = 0x04,
			TRIANGLE_FAN = 0x05,
			TRIANGLE_STRIP = 0x06,
			LINES_ADJACENT = 0x0A,
			LINE_STRIP_ADJACENT = 0x0B,
			TRIANGLES_ADJACENT = 0x0C,
			TRIANGLE_STRIP_ADJACENT = 0x0D,
			RECTS = 0x11,
			LINE_LOOP = 0x12,
			QUADS = 0x13,
			QUAD_STRIP = 0x14,
		};
		LREG_WORD(PRIMITIVE_MODE, E_PRIMITIVE_TYPE);
	};

	struct LATTE_SQ_CONFIG : LATTEREG
	{
		LREG_BOOL(DX9_CONSTS, 2);
		LREG_BOOL(ALU_INST_PREFER_VECTOR, 3);
		LREG_UINT(PS_PRIO, 24, 2);
		LREG_UINT(VS_PRIO, 26, 2);
		LREG_UINT(GS_PRIO, 28, 2);
		LREG_UINT(ES_PRIO, 30, 2);
	};

	struct LATTE_SQ_GPR_RESOURCE_MGMT_1 : LATTEREG
	{
		LREG_UINT(NUM_PS_GPRS, 0, 8);
		LREG_UINT(NUM_VS_GPRS, 8, 8);
	};

	struct LATTE_SQ_GPR_RESOURCE_MGMT_2 : LATTEREG
	{
		LREG_UINT(NUM_GS_GPRS, 0, 8);
		LREG_UINT(NUM_ES_GPRS, 8, 8);
	};

	struct LATTE_SQ_STACK_RESOURCE_MGMT_1 : LATTEREG
	{
		LREG_UINT(NUM_PS_STACK_ENTRIES, 0, 12);
		LREG_UINT(NUM_VS_STACK_ENTRIES, 16, 12);
	};

	struct LATTE_SQ_STACK_RESOURCE_MGMT_2 : LATTEREG
	{
		LREG_UINT(NUM_GS_STACK_ENTRIES, 0, 12);
		LREG_UINT(NUM_ES_STACK_ENTRIES, 16, 12);
	};

	struct LATTE_TD_BORDER_COLOR : LATTEREG
	{
		LREG_FLOAT(channelValue);
	};

	/* context registers */

	struct LATTE_DB_STENCIL_CLEAR : LATTEREG
	{
		LREG_UINT(clearValue, 0, 8);
	};

	struct LATTE_DB_DEPTH_CLEAR : LATTEREG
	{
		LREG_FLOAT(clearValue);
	};

	struct LATTE_CB_TARGET_MASK : LATTEREG
	{
		LREG_WORD(MASK, uint32); // 4 bits per render target
	};

	struct LATTE_PA_SC_GENERIC_SCISSOR_TL : LATTEREG
	{
		LREG_UINT(TL_X, 0, 15);
		LREG_UINT(TL_Y, 16, 15);
		LREG_BOOL(WINDOW_OFFSET_DISABLE, 31);
	};

	struct LATTE_PA_SC_GENERIC_SCISSOR_BR : LATTEREG
	{
		LREG_UINT(BR_X, 0, 15);
		LREG_UINT(BR_Y, 16, 15);
	};

	struct LATTE_SQ_VTX_SEMANTIC_X : LATTEREG
	{
		LREG_UINT(SEMANTIC_ID, 0, 8);
	};

	struct LATTE_VGT_MULTI_PRIM_IB_RESET_INDX : LATTEREG
	{
		LREG_WORD(RESTART_INDEX, uint32);
	};

	struct LATTE_SX_ALPHA_TEST_CONTROL : LATTEREG
	{
		using E_ALPHA_FUNC = E_COMPAREFUNC;
		LREG_ENUM(ALPHA_FUNC, 0, 3, E_ALPHA_FUNC);
		LREG_BOOL(ALPHA_TEST_ENABLE, 3);
		LREG_BOOL(ALPHA_TEST_BYPASS, 8);
	};

	struct LATTE_CB_BLEND_RED : LATTEREG { LREG_FLOAT(RED); };
	struct LATTE_CB_BLEND_GREEN : LATTEREG { LREG_FLOAT(GREEN); };
	struct LATTE_CB_BLEND_BLUE : LATTEREG { LREG_FLOAT(BLUE); };
	struct LATTE_CB_BLEND_ALPHA : LATTEREG { LREG_FLOAT(ALPHA); };

	struct LATTE_DB_STENCILREFMASK : LATTEREG
	{
		LREG_UINT(STENCILREF_F, 0, 8);
		LREG_UINT(STENCILMASK_F, 8, 8);
		LREG_UINT(STENCILWRITEMASK_F, 16, 8);
	};

	struct LATTE_DB_STENCILREFMASK_BF : LATTEREG
	{
		LREG_UINT(STENCILREF_B, 0, 8);
		LREG_UINT(STENCILMASK_B, 8, 8);
		LREG_UINT(STENCILWRITEMASK_B, 16, 8);
	};

	struct LATTE_SX_ALPHA_REF : LATTEREG { LREG_FLOAT(ALPHA_TEST_REF); };

	struct LATTE_PA_CL_VPORT_XSCALE : LATTEREG { LREG_FLOAT(SCALE); };
	struct LATTE_PA_CL_VPORT_XOFFSET : LATTEREG { LREG_FLOAT(OFFSET); };
	struct LATTE_PA_CL_VPORT_YSCALE : LATTEREG { LREG_FLOAT(SCALE); };
	struct LATTE_PA_CL_VPORT_YOFFSET : LATTEREG { LREG_FLOAT(OFFSET); };
	struct LATTE_PA_CL_VPORT_ZSCALE : LATTEREG { LREG_FLOAT(SCALE); };
	struct LATTE_PA_CL_VPORT_ZOFFSET : LATTEREG { LREG_FLOAT(OFFSET); };

	// four 8-bit output semantics per register
	struct LATTE_SPI_VS_OUT_ID_N : LATTEREG
	{
		uint8 get_SEMANTIC(sint32 index) const
		{
			cemu_assert_debug(index < 4);
			return static_cast<uint8>(regbits::Take(v, static_cast<uint32>(index) * 8, 8));
		}

		void set_SEMANTIC(sint32 index, uint8 value)
		{
			cemu_assert_debug(index < 4);
			v = regbits::Place(v, static_cast<uint32>(index) * 8, 8, value);
		}
	};

	struct LATTE_SPI_VS_OUT_CONFIG : LATTEREG
	{
		LREG_BOOL(VS_PER_COMPONENT, 0);
		LREG_UINT(VS_EXPORT_COUNT, 1, 5);
		LREG_BOOL(EXPORTS_FOG, 8);
		LREG_UINT(VS_OUT_FOG_VEC_ADDR, 9, 5);
	};

	struct LATTE_CB_BLENDN_CONTROL : LATTEREG
	{
		enum class E_BLENDFACTOR
		{
			BLEND_ZERO = 0x00,
			BLEND_ONE = 0x01,
			BLEND_SRC_COLOR = 0x02,
			BLEND_ONE_MINUS_SRC_COLOR = 0x03,
			BLEND_SRC_ALPHA = 0x04,
			BLEND_ONE_MINUS_SRC_ALPHA = 0x05,
			BLEND_DST_ALPHA = 0x06,
			BLEND_ONE_MINUS_DST_ALPHA = 0x07,
			BLEND_DST_COLOR = 0x08,
			BLEND_ONE_MINUS_DST_COLOR = 0x09,
			BLEND_SRC_ALPHA_SATURATE = 0x0A,
			BLEND_BOTH_SRC_ALPHA = 0x0B,
			BLEND_BOTH_INV_SRC_ALPHA = 0x0C,
			BLEND_CONST_COLOR = 0x0D,
			BLEND_ONE_MINUS_CONST_COLOR = 0x0E,
			BLEND_SRC1_COLOR = 0x0F,
			BLEND_INV_SRC1_COLOR = 0x10,
			BLEND_SRC1_ALPHA = 0x11,
			BLEND_INV_SRC1_ALPHA = 0x12,
			BLEND_CONST_ALPHA = 0x13,
			BLEND_ONE_MINUS_CONST_ALPHA = 0x14
		};

		enum class E_COMBINEFUNC
		{
			DST_PLUS_SRC = 0,
			SRC_MINUS_DST = 1,
			MIN_DST_SRC = 2,
			MAX_DST_SRC = 3,
			DST_MINUS_SRC = 4
		};

		LREG_ENUM(COLOR_SRCBLEND, 0, 5, E_BLENDFACTOR);
		LREG_ENUM(COLOR_COMB_FCN, 5, 3, E_COMBINEFUNC);
		LREG_ENUM(COLOR_DSTBLEND, 8, 5, E_BLENDFACTOR);
		LREG_BOOL(OPACITY_WEIGHT, 13);
		LREG_ENUM(ALPHA_SRCBLEND, 16, 5, E_BLENDFACTOR);
		LREG_ENUM(ALPHA_COMB_FCN, 21, 3, E_COMBINEFUNC);
		LREG_ENUM(ALPHA_DSTBLEND, 24, 5, E_BLENDFACTOR);
		LREG_BOOL(SEPARATE_ALPHA_BLEND, 29);
	};

	struct LATTE_DB_DEPTH_CONTROL : LATTEREG
	{
		using E_ZFUNC = E_COMPAREFUNC;
		using E_STENCILFUNC = E_COMPAREFUNC;

		enum class E_STENCILACTION
		{
			KEEP,
			ZERO,
			REPLACE,
			INCR,
			DECR,
			INVERT,
			INCR_WRAP,
			DECR_WRAP,
		};

		LREG_BOOL(STENCIL_ENABLE, 0);
		LREG_BOOL(Z_ENABLE, 1);
		LREG_BOOL(Z_WRITE_ENABLE, 2);
		LREG_ENUM(Z_FUNC, 4, 3, E_ZFUNC);
		LREG_BOOL(BACK_STENCIL_ENABLE, 7);
		// front face stencil
		LREG_ENUM(STENCIL_FUNC_F, 8, 3, E_STENCILFUNC);
		LREG_ENUM(STENCIL_FAIL_F, 11, 3, E_STENCILACTION);
		LREG_ENUM(STENCIL_ZPASS_F, 14, 3, E_STENCILACTION);
		LREG_ENUM(STENCIL_ZFAIL_F, 17, 3, E_STENCILACTION);
		// back face stencil
		LREG_ENUM(STENCIL_FUNC_B, 20, 3, E_STENCILFUNC);
		LREG_ENUM(STENCIL_FAIL_B, 23, 3, E_STENCILACTION);
		LREG_ENUM(STENCIL_ZPASS_B, 26, 3, E_STENCILACTION);
		LREG_ENUM(STENCIL_ZFAIL_B, 29, 3, E_STENCILACTION);
	};

	struct LATTE_CB_COLOR_CONTROL : LATTEREG
	{
		enum class E_SPECIALOP
		{
			NORMAL = 0,
			DISABLE = 1, // colour writes off
		};

		// ROP3 codes
		enum class E_LOGICOP
		{
			CLEAR = 0x00,
			COPY = 0xCC,
			OR = 0xEE,
			SET = 0xFF,
		};

		LREG_BOOL(FOG_ENABLE, 0);
		LREG_BOOL(MULTIWRITE_ENABLE, 1);
		LREG_BOOL(DITHER_ENABLE, 2);
		LREG_BOOL(DEGAMMA_ENABLE, 3);
		LREG_ENUM(SPECIAL_OP, 4, 3, E_SPECIALOP);
		LREG_BOOL(PER_MRT_BLEND, 7);
		LREG_UINT(BLEND_MASK, 8, 8);
		LREG_ENUM(ROP, 16, 8, E_LOGICOP);
	};

	struct LATTE_PA_CL_CLIP_CNTL : LATTEREG
	{
		LREG_BOOL(CLIP_DISABLE, 16);
		LREG_BOOL(DX_CLIP_SPACE_DEF, 19); // z range 0..1 instead of -1..1
		LREG_BOOL(DX_RASTERIZATION_KILL, 22);
		LREG_BOOL(DX_LINEAR_ATTR_CLIP_ENA, 24);
		LREG_BOOL(ZCLIP_NEAR_DISABLE, 26);
		LREG_BOOL(ZCLIP_FAR_DISABLE, 27);
	};

	struct LATTE_PA_SU_SC_MODE_CNTL : LATTEREG
	{
		enum class E_FRONTFACE
		{
			CCW = 0,
			CW = 1
		};

		enum class E_POLYGONMODE
		{
			UKN0 = 0, // filled triangles
		};

		enum class E_PTYPE
		{
			POINTS = 0,
			LINES = 1,
			TRIANGLES = 2,
		};

		LREG_BOOL(CULL_FRONT, 0);
		LREG_BOOL(CULL_BACK, 1);
		LREG_ENUM(FRONT_FACE, 2, 1, E_FRONTFACE);
		LREG_ENUM(POLYGON_MODE, 3, 2, E_POLYGONMODE);
		LREG_ENUM(FRONT_POLY_MODE, 5, 3, E_PTYPE);
		LREG_ENUM(BACK_POLY_MODE, 8, 3, E_PTYPE);
		LREG_BOOL(OFFSET_FRONT_ENABLED, 11);
		LREG_BOOL(OFFSET_BACK_ENABLED, 12);
		LREG_BOOL(OFFSET_PARA_ENABLED, 13);
	};

	struct LATTE_PA_CL_VTE_CNTL : LATTEREG
	{
		LREG_BOOL(VPORT_X_SCALE_ENA, 0);
		LREG_BOOL(VPORT_X_OFFSET_ENA, 1);
		LREG_BOOL(VPORT_Y_SCALE_ENA, 2);
		LREG_BOOL(VPORT_Y_OFFSET_ENA, 3);
		LREG_BOOL(VPORT_Z_SCALE_ENA, 4);
		LREG_BOOL(VPORT_Z_OFFSET_ENA, 5);
		LREG_BOOL(VTX_XY_FMT, 8);
		LREG_BOOL(VTX_Z_FMT, 9);
		LREG_BOOL(VTX_W0_FMT, 10);
	};

	struct LATTE_PA_CL_VS_OUT_CNTL : LATTEREG
	{
		LREG_UINT(CLIP_DIST_ENA_MASK, 0, 8);
		LREG_UINT(CULL_DIST_ENA_MASK, 8, 8);
	};

	// shader program registers (same layout for PS, VS, GS, ES and FS)
	struct LATTE_SQ_PGM_START_X : LATTEREG
	{
		LREG_WORD(PGM_START, uint32); // address >> 8
	};

	struct LATTE_SQ_PGM_RESOURCES_PS : LATTEREG
	{
		LREG_UINT(NUM_GPRS, 0, 8);
		LREG_UINT(NUM_STACK_ENTRIES, 8, 8);
		LREG_BOOL(DX10_CLAMP, 21); // clamp turns NaN into 0
		LREG_UINT(FETCH_CACHE_LINES, 24, 3);
		LREG_BOOL(UNCACHED_FIRST_INST, 28);
		LREG_BOOL(CLAMP_CONSTS, 31);
	};

	struct LATTE_SQ_PGM_RESOURCES_VS : LATTEREG
	{
		LREG_UINT(NUM_GPRS, 0, 8);
		LREG_UINT(NUM_STACK_ENTRIES, 8, 8);
		LREG_BOOL(DX10_CLAMP, 21);
		LREG_UINT(FETCH_CACHE_LINES, 24, 3);
		LREG_BOOL(UNCACHED_FIRST_INST, 28);
	};

	struct LATTE_SQ_PGM_RESOURCES_GS : LATTEREG
	{
		LREG_UINT(NUM_GPRS, 0, 8);
		LREG_UINT(NUM_STACK_ENTRIES, 8, 8);
		LREG_BOOL(DX10_CLAMP, 21);
	};

	struct LATTE_SQ_PGM_RESOURCES_ES : LATTEREG
	{
		LREG_UINT(NUM_GPRS, 0, 8);
		LREG_UINT(NUM_STACK_ENTRIES, 8, 8);
		LREG_BOOL(DX10_CLAMP, 21);
	};

	struct LATTE_SQ_PGM_RESOURCES_FS : LATTEREG
	{
		LREG_UINT(NUM_GPRS, 0, 8);
		LREG_UINT(NUM_STACK_ENTRIES, 8, 8);
		LREG_BOOL(DX10_CLAMP, 21);
	};

	// ring item size registers (ESGS, GSVS and the temp rings)
	struct LATTE_SQ_XX_ITEMSIZE : LATTEREG
	{
		LREG_UINT(ITEMSIZE, 0, 15);
	};

	struct LATTE_SQ_VTX_SEMANTIC_CLEAR : LATTEREG
	{
		LREG_WORD(CLEAR_MASK, uint32);
	};

	struct LATTE_PA_SU_POINT_SIZE : LATTEREG
	{
		LREG_UINT(HEIGHT, 0, 16);
		LREG_UINT(WIDTH, 16, 16);
	};

	struct LATTE_PA_SU_POINT_MINMAX : LATTEREG
	{
		LREG_UINT(MIN_SIZE, 0, 16);
		LREG_UINT(MAX_SIZE, 16, 16);
	};

	struct LATTE_VGT_GS_MODE : LATTEREG
	{
		enum class E_MODE
		{
			OFF = 0,
			SCENARIO_A = 1,
			SCENARIO_B = 2,
			SCENARIO_G = 3
		};

		enum class E_CUT_MODE
		{
			CUT_1024 = 0,
			CUT_512 = 1,
			CUT_256 = 2,
			CUT_128 = 3,
		};

		enum class E_COMPUTE_MODE
		{
			OFF = 0,
			UKN_1 = 1,
			UKN_2 = 2,
			ON = 3,
		};

		LREG_ENUM(MODE, 0, 2, E_MODE);
		LREG_BOOL(ES_PASSTHRU, 2);
		LREG_ENUM(CUT_MODE, 3, 2, E_CUT_MODE);
		LREG_ENUM(COMPUTE_MODE, 14, 2, E_COMPUTE_MODE);
		LREG_BOOL(PARTIAL_THD_AT_EOI, 17);
	};

	struct LATTE_VGT_DMA_INDEX_TYPE : LATTEREG
	{
		// bit 0 selects 32-bit indices, the upper bits select the byte swap
		enum class E_INDEX_TYPE
		{
			U16_LE = 0,
			U32_LE = 1,
			U16_BE = 4,
			U32_BE = 9,
			AUTO = 0xFFFF, // emulator-side marker for generated indices, never written by games
		};
		LREG_WORD(INDEX_TYPE, E_INDEX_TYPE);
	};

	struct LATTE_VGT_PRIMITIVEID_EN : LATTEREG
	{
		LREG_BOOL(PRIMITIVEID_EN, 0);
	};

	struct LATTE_VGT_DMA_NUM_INSTANCES : LATTEREG
	{
		LREG_WORD(NUM_INSTANCES, uint32);
	};

	struct LATTE_VGT_MULTI_PRIM_IB_RESET_EN : LATTEREG
	{
		LREG_BOOL(RESET_EN, 0);
	};

	struct LATTE_VGT_INSTANCE_STEP_RATE_X : LATTEREG
	{
		LREG_WORD(STEP_RATE, uint32);
	};

	// stream-out buffer registers
	struct LATTE_VGT_STRMOUT_BUFFER_SIZE_X : LATTEREG { LREG_WORD(SIZE, uint32); };
	struct LATTE_VGT_STRMOUT_STRIDE_X : LATTEREG { LREG_WORD(STRIDE, uint32); };
	struct LATTE_VGT_STRMOUT_BUFFER_BASE_X : LATTEREG { LREG_WORD(BASE, uint32); };
	struct LATTE_VGT_STRMOUT_BUFFER_OFFSET_X : LATTEREG { LREG_WORD(BUFFER_OFFSET, uint32); };
	struct LATTE_VGT_STRMOUT_BASE_OFFSET_X : LATTEREG { LREG_WORD(BASE_OFFSET, uint32); };

	struct LATTE_VGT_STRMOUT_BUFFER_EN : LATTEREG
	{
		LREG_BOOL(BUFFER_ENABLE_0, 0);
		LREG_BOOL(BUFFER_ENABLE_1, 1);
		LREG_BOOL(BUFFER_ENABLE_2, 2);
		LREG_BOOL(BUFFER_ENABLE_3, 3);
	};

	// polygon offset
	struct LATTE_PA_SU_POLY_OFFSET_CLAMP : LATTEREG { LREG_FLOAT(CLAMP); };
	struct LATTE_PA_SU_POLY_OFFSET_FRONT_SCALE : LATTEREG { LREG_FLOAT(SCALE); };
	struct LATTE_PA_SU_POLY_OFFSET_FRONT_OFFSET : LATTEREG { LREG_FLOAT(OFFSET); };
	struct LATTE_PA_SU_POLY_OFFSET_BACK_SCALE : LATTEREG { LREG_FLOAT(SCALE); };
	struct LATTE_PA_SU_POLY_OFFSET_BACK_OFFSET : LATTEREG { LREG_FLOAT(OFFSET); };

	/* texture resource words (7 per texture unit) */

	struct LATTE_SQ_TEX_RESOURCE_WORD0_N : LATTEREG
	{
		LREG_ENUM(DIM, 0, 3, E_DIM);
		LREG_ENUM(TILE_MODE, 3, 4, E_HWTILEMODE);
		LREG_BOOL(TILE_TYPE, 7);
		LREG_UINT(PITCH, 8, 11); // (pitch / 8) - 1
		LREG_UINT(WIDTH, 19, 13); // width - 1
	};

	struct LATTE_SQ_TEX_RESOURCE_WORD1_N : LATTEREG
	{
		LREG_UINT(HEIGHT, 0, 13);
		LREG_UINT(DEPTH, 13, 13);
		LREG_ENUM(DATA_FORMAT, 26, 6, E_HWFMT);
	};

	struct LATTE_SQ_TEX_RESOURCE_WORD2_N : LATTEREG
	{
		LREG_WORD(BASE_ADDRESS, uint32);
	};

	struct LATTE_SQ_TEX_RESOURCE_WORD3_N : LATTEREG
	{
		LREG_WORD(MIP_ADDRESS, uint32);
	};

	struct LATTE_SQ_TEX_RESOURCE_WORD4_N : LATTEREG
	{
		enum class E_FORMAT_COMP
		{
			COMP_UNSIGNED = 0,
			COMP_SIGNED = 1,
			COMP_UNSIGNED_BIASED = 2,
		};

		enum class E_NUM_FORMAT_ALL
		{
			NUM_FORMAT_NORM = 0,
			NUM_FORMAT_INT = 1,
			NUM_FORMAT_SCALED = 2,
		};

		enum class E_SRF_MODE_ALL
		{
			SRF_MODE_ZERO_CLAMP_MINUS_ONE = 0,
			SRF_MODE_NO_ZERO = 1,
		};

		enum class E_SEL
		{
			SEL_X = 0,
			SEL_Y = 1,
			SEL_Z = 2,
			SEL_W = 3,
			SEL_0 = 4,
			SEL_1 = 5
		};

		LREG_ENUM(FORMAT_COMP_X, 0, 2, E_FORMAT_COMP);
		LREG_ENUM(FORMAT_COMP_Y, 2, 2, E_FORMAT_COMP);
		LREG_ENUM(FORMAT_COMP_Z, 4, 2, E_FORMAT_COMP);
		LREG_ENUM(FORMAT_COMP_W, 6, 2, E_FORMAT_COMP);
		LREG_ENUM(NUM_FORM_ALL, 8, 2, E_NUM_FORMAT_ALL);
		LREG_ENUM(SRF_MODE_ALL, 10, 1, E_SRF_MODE_ALL);
		LREG_BOOL(FORCE_DEGAMMA, 11);
		LREG_ENUM(ENDIAN_SWAP, 12, 2, E_ENDIAN_SWAP);
		LREG_UINT(REQUEST_SIZE, 14, 2);
		LREG_ENUM(DST_SEL_X, 16, 3, E_SEL);
		LREG_ENUM(DST_SEL_Y, 19, 3, E_SEL);
		LREG_ENUM(DST_SEL_Z, 22, 3, E_SEL);
		LREG_ENUM(DST_SEL_W, 25, 3, E_SEL);
		LREG_UINT(BASE_LEVEL, 28, 4);
	};

	struct LATTE_SQ_TEX_RESOURCE_WORD5_N : LATTEREG
	{
		LREG_UINT(LAST_LEVEL, 0, 4); // MSAA textures keep the sample count here
		LREG_UINT(BASE_ARRAY, 4, 13);
		LREG_UINT(LAST_ARRAY, 17, 13);
		LREG_BOOL(UKN_BIT_30, 30);
	};

	struct LATTE_SQ_TEX_RESOURCE_WORD6_N : LATTEREG
	{
		enum class E_MPEG_CLAMP
		{
			UKN = 0,
		};

		enum class E_TYPE
		{
			VTX_INVALID_TEXTURE = 0,
			VTX_INVALID_BUFFER = 1,
			VTX_VALID_TEXTURE = 2,
			VTX_VALID_BUFFER = 3,
		};

		LREG_ENUM(MPEG_CLAMP, 0, 2, E_MPEG_CLAMP);
		LREG_UINT(MAX_ANISO, 2, 3);
		LREG_UINT(PERF_MODULATION, 5, 3);
		LREG_BOOL(INTERLACED, 8);
		LREG_ENUM(TYPE, 30, 2, E_TYPE);
	};

	/* sampler words (3 per sampler) */

	struct LATTE_SQ_TEX_SAMPLER_WORD0_0 : LATTEREG
	{
		enum class E_CLAMP
		{
			WRAP = 0,
			MIRROR = 1,
			CLAMP_LAST_TEXEL = 2,
			MIRROR_ONCE_LAST_TEXEL = 3,
			CLAMP_HALF_BORDER = 4,
			MIRROR_ONCE_HALF_BORDER = 5,
			CLAMP_BORDER = 6,
			MIRROR_ONCE_BORDER = 7,
		};

		enum class E_XY_FILTER
		{
			POINT = 0,
			BILINEAR = 1,
			BICUBIC = 2,
			ANISO_POINT = 4,
			ANISO_BILINEAR = 5,
		};

		enum class E_Z_FILTER
		{
			NONE = 0,
			POINT = 1,
			LINEAR = 2,
		};

		enum class E_BORDER_COLOR_TYPE
		{
			TRANSPARENT_BLACK = 0,
			OPAQUE_BLACK = 1,
			OPAQUE_WHITE = 2,
			REGISTER = 3, // TD_*_SAMPLER*_BORDER_* registers
		};

		enum class E_DEPTH_COMPARE
		{
			NEVER = 0,
			LESS = 1,
			EQUAL = 2,
			LEQUAL = 3,
			GREATER = 4,
			NOTEQUAL = 5,
			GEQUAL = 6,
			ALWAYS = 7
		};

		enum class E_CHROMA_KEY
		{
			DISABLE = 0,
			KILL = 1,
			BLEND = 2,
		};

		LREG_ENUM(CLAMP_X, 0, 3, E_CLAMP);
		LREG_ENUM(CLAMP_Y, 3, 3, E_CLAMP);
		LREG_ENUM(CLAMP_Z, 6, 3, E_CLAMP);
		LREG_ENUM(XY_MAG_FILTER, 9, 3, E_XY_FILTER);
		LREG_ENUM(XY_MIN_FILTER, 12, 3, E_XY_FILTER);
		LREG_ENUM(Z_FILTER, 15, 2, E_Z_FILTER);
		LREG_ENUM(MIP_FILTER, 17, 2, E_Z_FILTER);
		LREG_UINT(MAX_ANISO_RATIO, 19, 3);
		LREG_ENUM(BORDER_COLOR_TYPE, 22, 2, E_BORDER_COLOR_TYPE);
		LREG_BOOL(POINT_SAMPLING_CLAMP, 24);
		LREG_BOOL(TEX_ARRAY_OVERRIDE, 25);
		LREG_ENUM(DEPTH_COMPARE_FUNCTION, 26, 3, E_DEPTH_COMPARE);
		LREG_ENUM(CHROMA_KEY, 28, 2, E_CHROMA_KEY);
		// PixelU has always read this flag from bit 25, the same bit as TEX_ARRAY_OVERRIDE
		LREG_BOOL(LOD_USES_MINOR_AXIS, 25);
	};

	struct LATTE_SQ_TEX_SAMPLER_WORD1_0 : LATTEREG
	{
		LREG_UINT(MIN_LOD, 0, 10); // 4.6 fixed point
		LREG_UINT(MAX_LOD, 10, 10);
		LREG_SINT(LOD_BIAS, 20, 12);
	};

	struct LATTE_SQ_TEX_SAMPLER_WORD2_0 : LATTEREG
	{
		enum E_SAMPLER_TYPE
		{
			UKN0 = 0,
			UKN1 = 1,
		};

		LREG_UINT(LOD_BIAS_SECONDARY, 0, 12);
		LREG_BOOL(MC_COORD_TRUNCATE, 12);
		LREG_BOOL(FORCE_DEGAMMA, 13);
		LREG_BOOL(HIGH_PRECISION_FILTER, 14);
		LREG_UINT(ANISO_BIAS, 22, 6);
		LREG_ENUM(TYPE, 31, 1, E_SAMPLER_TYPE);
	};

#undef LREG_UINT
#undef LREG_SINT
#undef LREG_BOOL
#undef LREG_ENUM
#undef LREG_WORD
#undef LREG_FLOAT
};

/* -------------------------------------------------------------------------
   Register groups that repeat inside the register file
------------------------------------------------------------------------- */

struct _LatteRegisterSetTextureUnit
{
	Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N word0;
	Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N word1;
	Latte::LATTE_SQ_TEX_RESOURCE_WORD2_N word2;
	Latte::LATTE_SQ_TEX_RESOURCE_WORD3_N word3;
	Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N word4;
	Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N word5;
	Latte::LATTE_SQ_TEX_RESOURCE_WORD6_N word6;
};
static_assert(sizeof(_LatteRegisterSetTextureUnit) == 7 * 4);

struct _LatteRegisterSetSampler
{
	Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0 WORD0;
	Latte::LATTE_SQ_TEX_SAMPLER_WORD1_0 WORD1;
	Latte::LATTE_SQ_TEX_SAMPLER_WORD2_0 WORD2;
};
static_assert(sizeof(_LatteRegisterSetSampler) == 3 * 4);

struct _LatteRegisterSetSamplerBorderColor
{
	Latte::LATTE_TD_BORDER_COLOR red;
	Latte::LATTE_TD_BORDER_COLOR green;
	Latte::LATTE_TD_BORDER_COLOR blue;
	Latte::LATTE_TD_BORDER_COLOR alpha;
};
static_assert(sizeof(_LatteRegisterSetSamplerBorderColor) == 4 * 4);

struct _LatteRegisterSetStreamoutBuffer
{
	Latte::LATTE_VGT_STRMOUT_BUFFER_SIZE_X SIZE;
	Latte::LATTE_VGT_STRMOUT_STRIDE_X STRIDE;
	Latte::LATTE_VGT_STRMOUT_BUFFER_BASE_X BASE;
	Latte::LATTE_VGT_STRMOUT_BUFFER_OFFSET_X BUFFER_OFFSET;
};
static_assert(sizeof(_LatteRegisterSetStreamoutBuffer) == 4 * 4);

/* -------------------------------------------------------------------------
   The complete register file: 0x10000 registers followed by 9 words of
   emulator state. Named members sit at (register index * 4); the gaps are
   declared as raw words, sized from the register indices of their neighbours.
------------------------------------------------------------------------- */

#define LATTE_REG_GAP(_name, _fromReg, _toReg) uint32 _name[(_toReg) - (_fromReg)]

struct LatteContextRegister
{
	LATTE_REG_GAP(rawRegs_0000, 0x0000, 0x2256);
	Latte::LATTE_VGT_PRIMITIVE_TYPE VGT_PRIMITIVE_TYPE; // 0x2256
	LATTE_REG_GAP(rawRegs_2257, 0x2257, 0x2300);
	Latte::LATTE_SQ_CONFIG SQ_CONFIG; // 0x2300
	Latte::LATTE_SQ_GPR_RESOURCE_MGMT_1 SQ_GPR_RESOURCE_MGMT_1;
	Latte::LATTE_SQ_GPR_RESOURCE_MGMT_2 SQ_GPR_RESOURCE_MGMT_2;
	uint32 SQ_THREAD_RESOURCE_MGMT;
	Latte::LATTE_SQ_STACK_RESOURCE_MGMT_1 SQ_STACK_RESOURCE_MGMT_1;
	Latte::LATTE_SQ_STACK_RESOURCE_MGMT_2 SQ_STACK_RESOURCE_MGMT_2; // 0x2305
	LATTE_REG_GAP(rawRegs_2306, 0x2306, 0x2310);
	uint32 SQ_ESGS_RING_BASE; // 0x2310
	uint32 SQ_ESGS_RING_SIZE;
	uint32 SQ_GSVS_RING_BASE;
	uint32 SQ_GSVS_RING_SIZE; // 0x2313
	LATTE_REG_GAP(rawRegs_2314, 0x2314, 0x2900);
	_LatteRegisterSetSamplerBorderColor TD_PS_SAMPLER_BORDER_COLOR[Latte::GPU_LIMITS::NUM_SAMPLERS_PER_STAGE]; // 0x2900
	LATTE_REG_GAP(rawRegs_2948, 0x2948, 0x2980);
	_LatteRegisterSetSamplerBorderColor TD_VS_SAMPLER_BORDER_COLOR[Latte::GPU_LIMITS::NUM_SAMPLERS_PER_STAGE]; // 0x2980
	LATTE_REG_GAP(rawRegs_29C8, 0x29C8, 0x2A00);
	_LatteRegisterSetSamplerBorderColor TD_GS_SAMPLER_BORDER_COLOR[Latte::GPU_LIMITS::NUM_SAMPLERS_PER_STAGE]; // 0x2A00
	LATTE_REG_GAP(rawRegs_2A48, 0x2A48, 0xA08E);
	Latte::LATTE_CB_TARGET_MASK CB_TARGET_MASK; // 0xA08E
	LATTE_REG_GAP(rawRegs_A08F, 0xA08F, 0xA090);
	Latte::LATTE_PA_SC_GENERIC_SCISSOR_TL PA_SC_GENERIC_SCISSOR_TL; // 0xA090
	Latte::LATTE_PA_SC_GENERIC_SCISSOR_BR PA_SC_GENERIC_SCISSOR_BR; // 0xA091
	LATTE_REG_GAP(rawRegs_A092, 0xA092, 0xA0E0);
	Latte::LATTE_SQ_VTX_SEMANTIC_X SQ_VTX_SEMANTIC_X[32]; // 0xA0E0
	LATTE_REG_GAP(rawRegs_A100, 0xA100, 0xA103);
	Latte::LATTE_VGT_MULTI_PRIM_IB_RESET_INDX VGT_MULTI_PRIM_IB_RESET_INDX; // 0xA103
	Latte::LATTE_SX_ALPHA_TEST_CONTROL SX_ALPHA_TEST_CONTROL;
	Latte::LATTE_CB_BLEND_RED CB_BLEND_RED;
	Latte::LATTE_CB_BLEND_GREEN CB_BLEND_GREEN;
	Latte::LATTE_CB_BLEND_BLUE CB_BLEND_BLUE;
	Latte::LATTE_CB_BLEND_ALPHA CB_BLEND_ALPHA; // 0xA108
	LATTE_REG_GAP(rawRegs_A109, 0xA109, 0xA10C);
	Latte::LATTE_DB_STENCILREFMASK DB_STENCILREFMASK; // 0xA10C
	Latte::LATTE_DB_STENCILREFMASK_BF DB_STENCILREFMASK_BF;
	Latte::LATTE_SX_ALPHA_REF SX_ALPHA_REF;
	Latte::LATTE_PA_CL_VPORT_XSCALE PA_CL_VPORT_XSCALE;
	Latte::LATTE_PA_CL_VPORT_XOFFSET PA_CL_VPORT_XOFFSET;
	Latte::LATTE_PA_CL_VPORT_YSCALE PA_CL_VPORT_YSCALE;
	Latte::LATTE_PA_CL_VPORT_YOFFSET PA_CL_VPORT_YOFFSET;
	Latte::LATTE_PA_CL_VPORT_ZSCALE PA_CL_VPORT_ZSCALE;
	Latte::LATTE_PA_CL_VPORT_ZOFFSET PA_CL_VPORT_ZOFFSET; // 0xA114
	LATTE_REG_GAP(rawRegs_A115, 0xA115, 0xA185);
	Latte::LATTE_SPI_VS_OUT_ID_N LATTE_SPI_VS_OUT_ID_N[10]; // 0xA185
	LATTE_REG_GAP(rawRegs_A18F, 0xA18F, 0xA1B1);
	Latte::LATTE_SPI_VS_OUT_CONFIG SPI_VS_OUT_CONFIG; // 0xA1B1
	uint32 SPI_THREAD_GROUPING; // 0xA1B2
	LATTE_REG_GAP(rawRegs_A1B3, 0xA1B3, 0xA1E0);
	Latte::LATTE_CB_BLENDN_CONTROL CB_BLENDN_CONTROL[8]; // 0xA1E0
	LATTE_REG_GAP(rawRegs_A1E8, 0xA1E8, 0xA200);
	Latte::LATTE_DB_DEPTH_CONTROL DB_DEPTH_CONTROL; // 0xA200
	uint32 rawReg_A201;
	Latte::LATTE_CB_COLOR_CONTROL CB_COLOR_CONTROL; // 0xA202
	uint32 rawReg_A203;
	Latte::LATTE_PA_CL_CLIP_CNTL PA_CL_CLIP_CNTL; // 0xA204
	Latte::LATTE_PA_SU_SC_MODE_CNTL PA_SU_SC_MODE_CNTL;
	Latte::LATTE_PA_CL_VTE_CNTL PA_CL_VTE_CNTL;
	Latte::LATTE_PA_CL_VS_OUT_CNTL PA_CL_VS_OUT_CNTL; // 0xA207
	LATTE_REG_GAP(rawRegs_A208, 0xA208, 0xA210);
	// shader programs: START, SIZE, two unused words, RESOURCES (PS also has EXPORTS)
	Latte::LATTE_SQ_PGM_START_X SQ_PGM_START_PS; // 0xA210
	uint32 SQ_PGM_SIZE_PS_raw;
	uint32 rawRegs_A212[2];
	Latte::LATTE_SQ_PGM_RESOURCES_PS SQ_PGM_RESOURCES_PS; // 0xA214
	uint32 SQ_PGM_EXPORTS_PS_raw;
	Latte::LATTE_SQ_PGM_START_X SQ_PGM_START_VS; // 0xA216
	uint32 SQ_PGM_SIZE_VS_raw;
	uint32 rawRegs_A218[2];
	Latte::LATTE_SQ_PGM_RESOURCES_VS SQ_PGM_RESOURCES_VS; // 0xA21A
	Latte::LATTE_SQ_PGM_START_X SQ_PGM_START_GS; // 0xA21B
	uint32 SQ_PGM_SIZE_GS_raw;
	uint32 rawRegs_A21D[2];
	Latte::LATTE_SQ_PGM_RESOURCES_GS SQ_PGM_RESOURCES_GS; // 0xA21F
	Latte::LATTE_SQ_PGM_START_X SQ_PGM_START_ES; // 0xA220
	uint32 SQ_PGM_SIZE_ES_raw;
	uint32 rawRegs_A222[2];
	Latte::LATTE_SQ_PGM_RESOURCES_ES SQ_PGM_RESOURCES_ES; // 0xA224
	Latte::LATTE_SQ_PGM_START_X SQ_PGM_START_FS; // 0xA225
	uint32 SQ_PGM_SIZE_FS_raw;
	uint32 rawRegs_A227[2];
	Latte::LATTE_SQ_PGM_RESOURCES_FS SQ_PGM_RESOURCES_FS; // 0xA229
	Latte::LATTE_SQ_XX_ITEMSIZE SQ_ESGS_RING_ITEMSIZE; // 0xA22A
	Latte::LATTE_SQ_XX_ITEMSIZE SQ_GSVS_RING_ITEMSIZE;
	Latte::LATTE_SQ_XX_ITEMSIZE SQ_ESTMP_RING_ITEMSIZE;
	Latte::LATTE_SQ_XX_ITEMSIZE SQ_GSTMP_RING_ITEMSIZE;
	Latte::LATTE_SQ_XX_ITEMSIZE SQ_VSTMP_RING_ITEMSIZE; // 0xA22E
	LATTE_REG_GAP(rawRegs_A22F, 0xA22F, 0xA238);
	Latte::LATTE_SQ_VTX_SEMANTIC_CLEAR SQ_VTX_SEMANTIC_CLEAR; // 0xA238
	LATTE_REG_GAP(rawRegs_A239, 0xA239, 0xA280);
	Latte::LATTE_PA_SU_POINT_SIZE PA_SU_POINT_SIZE; // 0xA280
	Latte::LATTE_PA_SU_POINT_MINMAX PA_SU_POINT_MINMAX; // 0xA281
	LATTE_REG_GAP(rawRegs_A282, 0xA282, 0xA290);
	Latte::LATTE_VGT_GS_MODE VGT_GS_MODE; // 0xA290
	LATTE_REG_GAP(rawRegs_A291, 0xA291, 0xA29F);
	Latte::LATTE_VGT_DMA_INDEX_TYPE VGT_DMA_INDEX_TYPE; // 0xA29F
	uint32 rawReg_A2A0;
	Latte::LATTE_VGT_PRIMITIVEID_EN VGT_PRIMITIVEID_EN; // 0xA2A1
	Latte::LATTE_VGT_DMA_NUM_INSTANCES VGT_DMA_NUM_INSTANCES; // 0xA2A2
	uint32 rawRegs_A2A3[2];
	Latte::LATTE_VGT_MULTI_PRIM_IB_RESET_EN VGT_MULTI_PRIM_IB_RESET_EN; // 0xA2A5
	uint32 rawRegs_A2A6[2];
	Latte::LATTE_VGT_INSTANCE_STEP_RATE_X VGT_INSTANCE_STEP_RATE_0; // 0xA2A8
	Latte::LATTE_VGT_INSTANCE_STEP_RATE_X VGT_INSTANCE_STEP_RATE_1; // 0xA2A9
	LATTE_REG_GAP(rawRegs_A2AA, 0xA2AA, 0xA2AC);
	uint32 VGT_STRMOUT_EN; // 0xA2AC
	LATTE_REG_GAP(rawRegs_A2AD, 0xA2AD, 0xA2B4);
	_LatteRegisterSetStreamoutBuffer VGT_STRMOUT_BUFFER_X[4]; // 0xA2B4
	Latte::LATTE_VGT_STRMOUT_BASE_OFFSET_X VGT_STRMOUT_BASE_OFFSET_X[4]; // 0xA2C4
	Latte::LATTE_VGT_STRMOUT_BUFFER_EN VGT_STRMOUT_BUFFER_EN; // 0xA2C8
	LATTE_REG_GAP(rawRegs_A2C9, 0xA2C9, 0xA37F);
	Latte::LATTE_PA_SU_POLY_OFFSET_CLAMP PA_SU_POLY_OFFSET_CLAMP; // 0xA37F
	Latte::LATTE_PA_SU_POLY_OFFSET_FRONT_SCALE PA_SU_POLY_OFFSET_FRONT_SCALE;
	Latte::LATTE_PA_SU_POLY_OFFSET_FRONT_OFFSET PA_SU_POLY_OFFSET_FRONT_OFFSET;
	Latte::LATTE_PA_SU_POLY_OFFSET_BACK_SCALE PA_SU_POLY_OFFSET_BACK_SCALE;
	Latte::LATTE_PA_SU_POLY_OFFSET_BACK_OFFSET PA_SU_POLY_OFFSET_BACK_OFFSET; // 0xA383
	LATTE_REG_GAP(rawRegs_A384, 0xA384, 0xE000);
	// texture units: 18 are used per stage, the register space reserves
	// 160 units for PS (0xE000) and 176 for VS (0xE460), GS starts at 0xE930
	_LatteRegisterSetTextureUnit SQ_TEX_START_PS[Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE]; // 0xE000
	_LatteRegisterSetTextureUnit texUnitsUnusedPS[160 - Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE];
	_LatteRegisterSetTextureUnit SQ_TEX_START_VS[Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE]; // 0xE460
	_LatteRegisterSetTextureUnit texUnitsUnusedVS[176 - Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE];
	_LatteRegisterSetTextureUnit SQ_TEX_START_GS[Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE]; // 0xE930
	LATTE_REG_GAP(rawRegs_E9AE, 0xE9AE, 0xF000);
	_LatteRegisterSetSampler SQ_TEX_SAMPLER[18 * 3]; // 0xF000
	LATTE_REG_GAP(rawRegs_F0A2, 0xF0A2, 0x10000);

	// emulator state appended after the register file (no longer written)
	uint32 hleSpecialState[9];

	uint32* GetRawView() const
	{
		return const_cast<uint32*>(rawRegs_0000);
	}

	uint32* GetSpecialStateValues() const
	{
		return const_cast<uint32*>(hleSpecialState);
	}

	bool IsRasterizationEnabled() const
	{
		// GX2SetSpecialState(0, true) sets DX_RASTERIZATION_KILL but games still expect
		// depth output when the viewport X offset is disabled, so that case counts as enabled
		bool enabled = !PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() || !PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA();
		// culling both faces leaves nothing to rasterize
		if (PA_SU_SC_MODE_CNTL.get_CULL_FRONT() && PA_SU_SC_MODE_CNTL.get_CULL_BACK())
			enabled = false;
		return enabled;
	}
};

#undef LATTE_REG_GAP

static_assert(sizeof(LatteContextRegister) == (0x10000 + 9) * sizeof(uint32));

// every named member must sit at its register index
#define LATTE_CHECK_REG(_member, _reg) static_assert(offsetof(LatteContextRegister, _member) == (_reg) * sizeof(uint32), #_member)
LATTE_CHECK_REG(VGT_PRIMITIVE_TYPE, Latte::REGADDR::VGT_PRIMITIVE_TYPE);
LATTE_CHECK_REG(SQ_CONFIG, Latte::REGADDR::SQ_CONFIG);
LATTE_CHECK_REG(SQ_GPR_RESOURCE_MGMT_1, Latte::REGADDR::SQ_GPR_RESOURCE_MGMT_1);
LATTE_CHECK_REG(SQ_GPR_RESOURCE_MGMT_2, Latte::REGADDR::SQ_GPR_RESOURCE_MGMT_2);
LATTE_CHECK_REG(SQ_THREAD_RESOURCE_MGMT, Latte::REGADDR::SQ_THREAD_RESOURCE_MGMT);
LATTE_CHECK_REG(SQ_STACK_RESOURCE_MGMT_1, Latte::REGADDR::SQ_STACK_RESOURCE_MGMT_1);
LATTE_CHECK_REG(SQ_STACK_RESOURCE_MGMT_2, Latte::REGADDR::SQ_STACK_RESOURCE_MGMT_2);
LATTE_CHECK_REG(SQ_ESGS_RING_BASE, Latte::REGADDR::SQ_ESGS_RING_BASE);
LATTE_CHECK_REG(SQ_ESGS_RING_SIZE, Latte::REGADDR::SQ_ESGS_RING_SIZE);
LATTE_CHECK_REG(SQ_GSVS_RING_BASE, Latte::REGADDR::SQ_GSVS_RING_BASE);
LATTE_CHECK_REG(SQ_GSVS_RING_SIZE, Latte::REGADDR::SQ_GSVS_RING_SIZE);
LATTE_CHECK_REG(TD_PS_SAMPLER_BORDER_COLOR, Latte::REGADDR::TD_PS_SAMPLER0_BORDER_RED);
LATTE_CHECK_REG(TD_VS_SAMPLER_BORDER_COLOR, Latte::REGADDR::TD_VS_SAMPLER0_BORDER_RED);
LATTE_CHECK_REG(TD_GS_SAMPLER_BORDER_COLOR, Latte::REGADDR::TD_GS_SAMPLER0_BORDER_RED);
LATTE_CHECK_REG(CB_TARGET_MASK, Latte::REGADDR::CB_TARGET_MASK);
LATTE_CHECK_REG(PA_SC_GENERIC_SCISSOR_TL, Latte::REGADDR::PA_SC_GENERIC_SCISSOR_TL);
LATTE_CHECK_REG(PA_SC_GENERIC_SCISSOR_BR, Latte::REGADDR::PA_SC_GENERIC_SCISSOR_BR);
LATTE_CHECK_REG(SQ_VTX_SEMANTIC_X, Latte::REGADDR::SQ_VTX_SEMANTIC_0);
LATTE_CHECK_REG(VGT_MULTI_PRIM_IB_RESET_INDX, Latte::REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX);
LATTE_CHECK_REG(SX_ALPHA_TEST_CONTROL, Latte::REGADDR::SX_ALPHA_TEST_CONTROL);
LATTE_CHECK_REG(CB_BLEND_RED, Latte::REGADDR::CB_BLEND_RED);
LATTE_CHECK_REG(CB_BLEND_GREEN, Latte::REGADDR::CB_BLEND_GREEN);
LATTE_CHECK_REG(CB_BLEND_BLUE, Latte::REGADDR::CB_BLEND_BLUE);
LATTE_CHECK_REG(CB_BLEND_ALPHA, Latte::REGADDR::CB_BLEND_ALPHA);
LATTE_CHECK_REG(DB_STENCILREFMASK, Latte::REGADDR::DB_STENCILREFMASK);
LATTE_CHECK_REG(DB_STENCILREFMASK_BF, Latte::REGADDR::DB_STENCILREFMASK_BF);
LATTE_CHECK_REG(SX_ALPHA_REF, Latte::REGADDR::SX_ALPHA_REF);
LATTE_CHECK_REG(PA_CL_VPORT_XSCALE, Latte::REGADDR::PA_CL_VPORT_XSCALE);
LATTE_CHECK_REG(PA_CL_VPORT_XOFFSET, Latte::REGADDR::PA_CL_VPORT_XOFFSET);
LATTE_CHECK_REG(PA_CL_VPORT_YSCALE, Latte::REGADDR::PA_CL_VPORT_YSCALE);
LATTE_CHECK_REG(PA_CL_VPORT_YOFFSET, Latte::REGADDR::PA_CL_VPORT_YOFFSET);
LATTE_CHECK_REG(PA_CL_VPORT_ZSCALE, Latte::REGADDR::PA_CL_VPORT_ZSCALE);
LATTE_CHECK_REG(PA_CL_VPORT_ZOFFSET, Latte::REGADDR::PA_CL_VPORT_ZOFFSET);
LATTE_CHECK_REG(LATTE_SPI_VS_OUT_ID_N, Latte::REGADDR::SPI_VS_OUT_ID_0);
LATTE_CHECK_REG(SPI_VS_OUT_CONFIG, Latte::REGADDR::SPI_VS_OUT_CONFIG);
LATTE_CHECK_REG(SPI_THREAD_GROUPING, Latte::REGADDR::SPI_THREAD_GROUPING);
LATTE_CHECK_REG(CB_BLENDN_CONTROL, Latte::REGADDR::CB_BLEND0_CONTROL);
LATTE_CHECK_REG(DB_DEPTH_CONTROL, Latte::REGADDR::DB_DEPTH_CONTROL);
LATTE_CHECK_REG(CB_COLOR_CONTROL, Latte::REGADDR::CB_COLOR_CONTROL);
LATTE_CHECK_REG(PA_CL_CLIP_CNTL, Latte::REGADDR::PA_CL_CLIP_CNTL);
LATTE_CHECK_REG(PA_SU_SC_MODE_CNTL, Latte::REGADDR::PA_SU_SC_MODE_CNTL);
LATTE_CHECK_REG(PA_CL_VTE_CNTL, Latte::REGADDR::PA_CL_VTE_CNTL);
LATTE_CHECK_REG(PA_CL_VS_OUT_CNTL, Latte::REGADDR::PA_CL_VS_OUT_CNTL);
LATTE_CHECK_REG(SQ_PGM_START_PS, Latte::REGADDR::SQ_PGM_START_PS);
LATTE_CHECK_REG(SQ_PGM_RESOURCES_PS, Latte::REGADDR::SQ_PGM_RESOURCES_PS);
LATTE_CHECK_REG(SQ_PGM_EXPORTS_PS_raw, Latte::REGADDR::SQ_PGM_EXPORTS_PS);
LATTE_CHECK_REG(SQ_PGM_START_VS, Latte::REGADDR::SQ_PGM_START_VS);
LATTE_CHECK_REG(SQ_PGM_RESOURCES_VS, Latte::REGADDR::SQ_PGM_RESOURCES_VS);
LATTE_CHECK_REG(SQ_PGM_START_GS, Latte::REGADDR::SQ_PGM_START_GS);
LATTE_CHECK_REG(SQ_PGM_RESOURCES_GS, Latte::REGADDR::SQ_PGM_RESOURCES_GS);
LATTE_CHECK_REG(SQ_PGM_START_ES, Latte::REGADDR::SQ_PGM_START_ES);
LATTE_CHECK_REG(SQ_PGM_RESOURCES_ES, Latte::REGADDR::SQ_PGM_RESOURCES_ES);
LATTE_CHECK_REG(SQ_PGM_START_FS, Latte::REGADDR::SQ_PGM_START_FS);
LATTE_CHECK_REG(SQ_PGM_RESOURCES_FS, Latte::REGADDR::SQ_PGM_RESOURCES_FS);
LATTE_CHECK_REG(SQ_ESGS_RING_ITEMSIZE, Latte::REGADDR::SQ_ESGS_RING_ITEMSIZE);
LATTE_CHECK_REG(SQ_GSVS_RING_ITEMSIZE, Latte::REGADDR::SQ_GSVS_RING_ITEMSIZE);
LATTE_CHECK_REG(SQ_VTX_SEMANTIC_CLEAR, Latte::REGADDR::SQ_VTX_SEMANTIC_CLEAR);
LATTE_CHECK_REG(PA_SU_POINT_SIZE, Latte::REGADDR::PA_SU_POINT_SIZE);
LATTE_CHECK_REG(PA_SU_POINT_MINMAX, Latte::REGADDR::PA_SU_POINT_MINMAX);
LATTE_CHECK_REG(VGT_GS_MODE, Latte::REGADDR::VGT_GS_MODE);
LATTE_CHECK_REG(VGT_DMA_INDEX_TYPE, Latte::REGADDR::VGT_DMA_INDEX_TYPE);
LATTE_CHECK_REG(VGT_PRIMITIVEID_EN, Latte::REGADDR::VGT_PRIMITIVEID_EN);
LATTE_CHECK_REG(VGT_DMA_NUM_INSTANCES, Latte::REGADDR::VGT_DMA_NUM_INSTANCES);
LATTE_CHECK_REG(VGT_MULTI_PRIM_IB_RESET_EN, Latte::REGADDR::VGT_MULTI_PRIM_IB_RESET_EN);
LATTE_CHECK_REG(VGT_INSTANCE_STEP_RATE_0, Latte::REGADDR::VGT_INSTANCE_STEP_RATE_0);
LATTE_CHECK_REG(VGT_INSTANCE_STEP_RATE_1, Latte::REGADDR::VGT_INSTANCE_STEP_RATE_1);
LATTE_CHECK_REG(VGT_STRMOUT_EN, Latte::REGADDR::VGT_STRMOUT_EN);
LATTE_CHECK_REG(VGT_STRMOUT_BUFFER_X, Latte::REGADDR::VGT_STRMOUT_BUFFER_SIZE_0);
LATTE_CHECK_REG(VGT_STRMOUT_BASE_OFFSET_X, Latte::REGADDR::VGT_STRMOUT_BASE_OFFSET_0);
LATTE_CHECK_REG(VGT_STRMOUT_BUFFER_EN, Latte::REGADDR::VGT_STRMOUT_BUFFER_EN);
LATTE_CHECK_REG(PA_SU_POLY_OFFSET_CLAMP, Latte::REGADDR::PA_SU_POLY_OFFSET_CLAMP);
LATTE_CHECK_REG(PA_SU_POLY_OFFSET_FRONT_SCALE, Latte::REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE);
LATTE_CHECK_REG(PA_SU_POLY_OFFSET_FRONT_OFFSET, Latte::REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET);
LATTE_CHECK_REG(PA_SU_POLY_OFFSET_BACK_SCALE, Latte::REGADDR::PA_SU_POLY_OFFSET_BACK_SCALE);
LATTE_CHECK_REG(PA_SU_POLY_OFFSET_BACK_OFFSET, Latte::REGADDR::PA_SU_POLY_OFFSET_BACK_OFFSET);
LATTE_CHECK_REG(SQ_TEX_START_PS, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS);
LATTE_CHECK_REG(SQ_TEX_START_VS, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS);
LATTE_CHECK_REG(SQ_TEX_START_GS, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS);
LATTE_CHECK_REG(SQ_TEX_SAMPLER, Latte::REGADDR::SQ_TEX_SAMPLER_WORD0_0);
LATTE_CHECK_REG(hleSpecialState, 0x10000);
#undef LATTE_CHECK_REG
