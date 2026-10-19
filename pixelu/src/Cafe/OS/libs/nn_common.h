// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// nn::Result, the 32-bit result code of the nn_* libraries:
//   bits 29-31  level (an error when the top bit is set)
//   bits 20-28  module
//   bits  0-19  description (user-facing codes are stored shifted left by 7)
#pragma once

using nnResult = uint32;

inline bool NN_RESULT_IS_SUCCESS(nnResult r)
{
	return (sint32)r >= 0;
}

inline bool NN_RESULT_IS_FAILURE(nnResult r)
{
	return (sint32)r < 0;
}

#define NN_RESULT_LEVEL_FATAL		(0x7)
#define NN_RESULT_LEVEL_LVL6		(0x6)
#define NN_RESULT_LEVEL_STATUS		(0x5)
#define NN_RESULT_LEVEL_SUCCESS		(0x0)

#define BUILD_NN_RESULT(__level, __module, __detailedErrorCode) ((((uint32)(__level)&0x7)<<29) | ((uint32)(__module)<<20) | ((uint32)(__detailedErrorCode)))
#define BUILD_NN_RESULT_STATUS(__module, __detailedErrorCode) BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, __module, __detailedErrorCode)

#define NN_RESULT_SUCCESS	((nnResult)0)
#define NN_RESULT_PLACEHOLDER_ERROR	((nnResult)0x80000000)

enum NN_RESULT_MODULE : uint32
{
	NN_RESULT_MODULE_COMMON			= 0,
	NN_RESULT_MODULE_NN_IPC			= 1,
	NN_RESULT_MODULE_NN_BOSS		= 2,
	NN_RESULT_MODULE_NN_ACP			= 3,
	NN_RESULT_MODULE_NN_IOS			= 4,
	NN_RESULT_MODULE_NN_NIM			= 5,
	NN_RESULT_MODULE_NN_PDM			= 6,
	NN_RESULT_MODULE_NN_ACT			= 7,
	NN_RESULT_MODULE_NN_NUP			= 10,
	NN_RESULT_MODULE_NN_NDM			= 11,
	NN_RESULT_MODULE_NN_FP			= 12,
	NN_RESULT_MODULE_NN_AC			= 13,
	NN_RESULT_MODULE_NN_DRMAPP		= 15,
	NN_RESULT_MODULE_NN_OLV			= 17,
	NN_RESULT_MODULE_NN_VCTL		= 18,
	NN_RESULT_MODULE_NN_SPM			= 20,
	NN_RESULT_MODULE_NN_EC			= 22,
	NN_RESULT_MODULE_NN_SL			= 24,
	NN_RESULT_MODULE_NN_ECO			= 25,
	NN_RESULT_MODULE_NN_NFP			= 27,

	NN_RESULT_MODULE_MCP			= 511,
};

// four digit error code (0000-9999) as shown by the error viewer, e.g. the 2932 of 102-2932
enum class NN_ERROR_CODE : uint32
{
	ACT_UNKNOWN_SERVER_ERROR = 2932,
};

constexpr inline nnResult nnResultStatus(NN_RESULT_MODULE module, NN_ERROR_CODE errorCode)
{
	return BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, (uint32)module, (uint32)errorCode << 7);
}

// nn_act descriptions
#define NN_ACT_RESULT_ACCOUNT_DOES_NOT_EXIST	128128

// nn_nfp descriptions
#define NN_RESULT_NFP_CODE_APPAREAIDMISMATCH	0x11300
#define NN_RESULT_NFP_CODE_NOAPPAREA			0x10400

namespace nn
{
	inline NN_RESULT_MODULE nnResult_GetModule(const nnResult res) { return (NN_RESULT_MODULE)((res >> 20) & 0x1FF); }
	inline uint32 nnResult_GetDescription(const nnResult res) { return res & 0xFFFFF; }

	// error viewer prefix of nn_act (102-xxxx)
	#define	NN_ERRCODE_MODULE_PREFIX_ACT	1020000

	// full error viewer number (prefix * 10000 + code) of a result from the given module
	inline NN_ERROR_CODE NNResultToErrorCode(nnResult result, NN_RESULT_MODULE expectedModule)
	{
		if ((result & 0x18000000) == 0x18000000)
			cemu_assert_unimplemented(); // module numbers this large have no known prefix
		uint32 prefix = 0;
		if (expectedModule == NN_RESULT_MODULE_NN_ACT)
			prefix = NN_ERRCODE_MODULE_PREFIX_ACT;
		else
			cemu_assert_unimplemented();
		constexpr uint32 kUnknown = 9999;
		if (nnResult_GetModule(result) != expectedModule)
			return (NN_ERROR_CODE)(prefix + kUnknown);
		const uint32 code = nnResult_GetDescription(result) >> 7;
		if (code > kUnknown)
		{
			cemu_assert_suspicious();
			return (NN_ERROR_CODE)(prefix + kUnknown);
		}
		return (NN_ERROR_CODE)(prefix + code);
	}
}

static_assert(BUILD_NN_RESULT_STATUS(NN_RESULT_MODULE_NN_ACT, 317696) == 0xA074D900);
static_assert(BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_ACT, NN_ACT_RESULT_ACCOUNT_DOES_NOT_EXIST) == 0xA071F480);
