// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <string>

// function attributes that allow GCC/Clang to emit instructions of an extension
// in a single function; MSVC needs no attribute for this
#if defined(__GNUC__)
	#define ATTRIBUTE_AVX2 __attribute__((target("avx2")))
	#define ATTRIBUTE_SSE41 __attribute__((target("sse4.1")))
	#define ATTRIBUTE_AESNI __attribute__((target("aes")))
#else
	#define ATTRIBUTE_AVX2
	#define ATTRIBUTE_SSE41
	#define ATTRIBUTE_AESNI
#endif

// Host CPU capabilities, detected once at startup (see g_CPUFeatures).
class CPUFeaturesImpl
{
public:
	CPUFeaturesImpl();

	std::string GetCPUName(); // empty when unknown
	std::string GetCommaSeparatedExtensionList();

	// x86-64 instruction set extensions; all false on other architectures
	struct
	{
		bool ssse3{false};
		bool sse4_1{false};
		bool avx{false};
		bool avx2{false};
		bool lzcnt{false};
		bool movbe{false};
		bool bmi2{false};
		bool fastPext{false}; // BMI2 PEXT is fast enough to replace shift and mask sequences
		bool aesni{false};
		bool invariant_tsc{false};
	} x86;

private:
	char m_cpuBrandName[0x40]{};
};

extern CPUFeaturesImpl g_CPUFeatures;
