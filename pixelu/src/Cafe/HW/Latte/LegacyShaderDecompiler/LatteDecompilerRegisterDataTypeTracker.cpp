// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInstructions.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInternal.h"

// Picks how GPRs are declared in the generated code. Registers are untyped on
// the hardware; we store them as int vectors as soon as any integer operation
// shows up (bit-exact reinterpretation), otherwise as float vectors. Indexed
// writes need an array so the target can be selected at runtime.
void LatteDecompiler_analyzeDataTypes(LatteDecompilerShaderContext* shaderContext)
{
	auto& types = shaderContext->typeTracker;
	const auto& facts = shaderContext->analyzer;
	if (facts.usesIntegerValues)
	{
		types.defaultDataType = LATTE_DECOMPILER_DTYPE_SIGNED_INT;
		types.genIntReg = true;
	}
	else
	{
		types.defaultDataType = LATTE_DECOMPILER_DTYPE_FLOAT;
		types.genFloatReg = true;
	}
	if (facts.usesRelativeGPRWrite)
		types.useArrayGPRs = true;
}
