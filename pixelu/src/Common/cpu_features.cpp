// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "cpu_features.h"

#if BOOST_OS_MACOS
	#include <sys/types.h>
	#include <sys/sysctl.h>
#endif

#if defined(ARCH_X86_64) && defined(__GNUC__)
	#include <cpuid.h>
#endif

namespace
{
#if defined(ARCH_X86_64)
	struct CpuidResult
	{
		uint32 eax, ebx, ecx, edx;

		bool bit(uint32 reg, int index) const { return ((reg >> index) & 1) != 0; }
	};

	CpuidResult queryCpuid(uint32 leaf, uint32 subleaf = 0)
	{
		CpuidResult r{};
	#if defined(_MSC_VER)
		int regs[4];
		__cpuidex(regs, (int)leaf, (int)subleaf);
		r.eax = (uint32)regs[0];
		r.ebx = (uint32)regs[1];
		r.ecx = (uint32)regs[2];
		r.edx = (uint32)regs[3];
	#elif defined(__GNUC__)
		__cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
	#else
		#error cpuid is not available with this compiler
	#endif
		return r;
	}

	// the 12 character vendor string is stored in ebx, edx, ecx order
	bool vendorIs(const CpuidResult& leaf0, const char* vendor)
	{
		char name[13]{};
		memcpy(name + 0, &leaf0.ebx, 4);
		memcpy(name + 4, &leaf0.edx, 4);
		memcpy(name + 8, &leaf0.ecx, 4);
		return strcmp(name, vendor) == 0;
	}
#endif
}

CPUFeaturesImpl::CPUFeaturesImpl()
{
#if BOOST_OS_MACOS
	size_t len = 0;
	if (sysctlbyname("machdep.cpu.brand_string", nullptr, &len, nullptr, 0) == 0 && len > 0)
	{
		std::vector<char> name(len + 1, '\0');
		if (sysctlbyname("machdep.cpu.brand_string", name.data(), &len, nullptr, 0) == 0)
		{
			const size_t n = std::min(strlen(name.data()), sizeof(m_cpuBrandName) - 1);
			memcpy(m_cpuBrandName, name.data(), n);
			m_cpuBrandName[n] = '\0';
		}
	}
#elif defined(ARCH_X86_64)
	const CpuidResult basic = queryCpuid(1);
	x86.ssse3 = basic.bit(basic.ecx, 9);
	x86.sse4_1 = basic.bit(basic.ecx, 19);
	x86.movbe = basic.bit(basic.ecx, 22);
	x86.aesni = basic.bit(basic.ecx, 25);
	x86.avx = basic.bit(basic.ecx, 28);

	// family, including the extended family for family 0xF
	uint32 family = (basic.eax >> 8) & 0xF;
	if (family == 0xF)
		family += (basic.eax >> 20) & 0xFF;

	const CpuidResult extendedFeatures = queryCpuid(7, 0);
	x86.avx2 = extendedFeatures.bit(extendedFeatures.ebx, 5);
	x86.bmi2 = extendedFeatures.bit(extendedFeatures.ebx, 8);

	const CpuidResult extended1 = queryCpuid(0x80000001);
	x86.lzcnt = extended1.bit(extended1.ecx, 5);

	// AMD CPUs before Zen 3 (family 0x19) implement PEXT in microcode
	const CpuidResult vendor = queryCpuid(0);
	const bool intel = vendorIs(vendor, "GenuineIntel");
	const bool amd = vendorIs(vendor, "AuthenticAMD");
	x86.fastPext = x86.bmi2 && (intel || (amd && family >= 0x19));

	const CpuidResult power = queryCpuid(0x80000007);
	x86.invariant_tsc = power.bit(power.edx, 8);

	// brand string from leaves 0x80000002..0x80000004, 16 bytes each
	const uint32 highestExtendedLeaf = queryCpuid(0x80000000).eax;
	for (uint32 part = 0; part < 3; part++)
	{
		const uint32 leaf = 0x80000002 + part;
		if (leaf > highestExtendedLeaf)
			break;
		const CpuidResult r = queryCpuid(leaf);
		const uint32 regs[4] = {r.eax, r.ebx, r.ecx, r.edx};
		memcpy(m_cpuBrandName + part * 16, regs, sizeof(regs));
	}
#endif
}

std::string CPUFeaturesImpl::GetCPUName()
{
	return std::string(m_cpuBrandName);
}

std::string CPUFeaturesImpl::GetCommaSeparatedExtensionList()
{
	const std::pair<bool, const char*> extensions[] = {
		{x86.ssse3, "SSSE3"},
		{x86.sse4_1, "SSE4.1"},
		{x86.avx, "AVX"},
		{x86.avx2, "AVX2"},
		{x86.lzcnt, "LZCNT"},
		{x86.movbe, "MOVBE"},
		{x86.bmi2, "BMI2"},
		{x86.aesni, "AES-NI"},
		{x86.invariant_tsc, "INVARIANT-TSC"},
	};
	std::string list;
	for (const auto& [present, name] : extensions)
	{
		if (!present)
			continue;
		if (!list.empty())
			list += ", ";
		list += name;
	}
	return list;
}

CPUFeaturesImpl g_CPUFeatures;
