// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/helpers/Serializer.h"

namespace Latte
{
	// Fixed selection of GPU registers (everything relevant to a pipeline except the ALU
	// constants), packed back to back. The layout is part of the pipeline cache format.
	struct GPUCompactedRegisterState
	{
		static constexpr int const NUM_REGS = 1854;

		uint32 rawArray[NUM_REGS];
	};

	// copy the selected registers out of / back into a full register file
	void StoreGPURegisterState(const LatteContextRegister& contextRegister, GPUCompactedRegisterState& registerStateOut);
	void LoadGPURegisterState(LatteContextRegister& contextRegisterOut, const GPUCompactedRegisterState& registerState);

	// compressed on-disk form of the selection
	void SerializeRegisterState(GPUCompactedRegisterState& regState, MemStreamWriter& memWriter);
	bool DeserializeRegisterState(GPUCompactedRegisterState& regState, MemStreamReader& memReader);
}
