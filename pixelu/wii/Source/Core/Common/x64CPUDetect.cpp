// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/CPUDetect.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

#ifdef _WIN32
#include <windows.h>
#include <processthreadsapi.h>

#include "Common/MsgHandler.h"
#endif

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace
{
struct Regs
{
  u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
};

Regs Cpuid(u32 leaf, u32 subleaf = 0)
{
  Regs r;
#ifdef _MSC_VER
  int out[4];
  __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
  r = {static_cast<u32>(out[0]), static_cast<u32>(out[1]), static_cast<u32>(out[2]),
       static_cast<u32>(out[3])};
#else
  __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
#endif
  return r;
}

// XCR0: which register states the OS saves on context switches.
u64 ReadXCR0()
{
#ifdef _MSC_VER
  return _xgetbv(0);
#else
  u32 lo, hi;
  __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (u64{hi} << 32) | lo;
#endif
}

constexpr bool Bit(u32 value, int bit)
{
  return ((value >> bit) & 1) != 0;
}

#ifdef _WIN32
// Windows 11 on ARM runs x64 programs under emulation; tell the user a native build exists.
void WarnIfRunningUnderEmulation()
{
  PROCESS_MACHINE_INFORMATION machine{};
  if (!GetProcessInformation(GetCurrentProcess(), ProcessMachineTypeInfo, &machine,
                             sizeof(machine)))
  {
    return;  // older Windows without this query, and without x64 emulation either
  }
  // KernelEnabled means the process architecture is the machine's native one.
  if (machine.MachineAttributes & MACHINE_ATTRIBUTES::KernelEnabled)
    return;

  PanicAlertFmtT("This build of Dolphin is not natively compiled for your CPU.\n"
                 "Please run the ARM64 build of Dolphin for a better experience.");
}
#endif

// Intel Atom family-6 models (Bonnell through Airmont).
constexpr std::array<u32, 10> ATOM_MODELS = {0x1C, 0x26, 0x27, 0x35, 0x36,
                                             0x37, 0x4A, 0x4D, 0x5A, 0x5D};
}  // namespace

CPUInfo cpu_info;

CPUInfo::CPUInfo()
{
  Detect();
}

void CPUInfo::Detect()
{
#ifdef _WIN32
  WarnIfRunningUnderEmulation();
#endif

  num_cores = std::max(static_cast<int>(std::thread::hardware_concurrency()), 1);

  // Leaf 0: highest standard leaf and the vendor string (EBX, EDX, ECX order).
  const Regs leaf0 = Cpuid(0);
  const u32 max_leaf = leaf0.eax;
  std::string vendor_id(12, '\0');
  std::memcpy(vendor_id.data() + 0, &leaf0.ebx, 4);
  std::memcpy(vendor_id.data() + 4, &leaf0.edx, 4);
  std::memcpy(vendor_id.data() + 8, &leaf0.ecx, 4);
  TruncateToCString(&vendor_id);

  if (vendor_id == "GenuineIntel")
    vendor = CPUVendor::Intel;
  else if (vendor_id == "AuthenticAMD")
    vendor = CPUVendor::AMD;
  else
    vendor = CPUVendor::Other;

  bool has_sse = false;
  bool zen1_or_zen2 = false;

  if (max_leaf >= 1)
  {
    const Regs leaf1 = Cpuid(1);
    const u32 signature = leaf1.eax;
    const u32 family = ((signature >> 8) & 0xF) + ((signature >> 20) & 0xFF);
    const u32 model = ((signature >> 4) & 0xF) + ((signature >> 12) & 0xF0);
    const u32 stepping = signature & 0xF;
    cpu_id = fmt::format("{:02X}:{:02X}:{:X}", family, model, stepping);

    if (vendor == CPUVendor::Intel && family == 6)
      bAtom = std::ranges::find(ATOM_MODELS, model) != ATOM_MODELS.end();

    // Family 17h: Zen, Zen+ and Zen 2, whose PDEP/PEXT are microcoded and very slow.
    zen1_or_zen2 = vendor == CPUVendor::AMD && family == 0x17;

    // Pre-Zen AMD parts set the HTT bit without having real SMT.
    HTT = Bit(leaf1.edx, 28) &&
          (vendor == CPUVendor::Intel || (vendor == CPUVendor::AMD && family >= 0x17));

    has_sse = Bit(leaf1.edx, 25);
    bSSE3 = Bit(leaf1.ecx, 0);
    bSSSE3 = Bit(leaf1.ecx, 9);
    bSSE4_1 = Bit(leaf1.ecx, 19);
    bSSE4_2 = Bit(leaf1.ecx, 20);
    bMOVBE = Bit(leaf1.ecx, 22);
    bAES = Bit(leaf1.ecx, 25);

    // AVX is only usable if the CPU has it, has XSAVE enabled by the OS (OSXSAVE), and the OS
    // saves both SSE and AVX state (XCR0 bits 1 and 2).
    const bool avx = Bit(leaf1.ecx, 28);
    const bool osxsave = Bit(leaf1.ecx, 27);
    if (avx && osxsave && (ReadXCR0() & 0b110) == 0b110)
    {
      bAVX = true;
      bFMA = Bit(leaf1.ecx, 12);
    }

    if (max_leaf >= 7)
    {
      const Regs leaf7 = Cpuid(7, 0);
      bBMI1 = Bit(leaf7.ebx, 3);
      bBMI2 = Bit(leaf7.ebx, 8);
      bSHA1 = bSHA2 = Bit(leaf7.ebx, 29);
    }
  }

  const u32 max_ext_leaf = Cpuid(0x80000000).eax;
  if (max_ext_leaf >= 0x80000004)
  {
    // The brand string: 48 bytes from leaves 0x80000002..4.
    model_name.assign(48, '\0');
    for (u32 i = 0; i < 3; ++i)
    {
      const Regs part = Cpuid(0x80000002 + i);
      std::memcpy(model_name.data() + i * 16, &part, 16);
    }
    TruncateToCString(&model_name);
    model_name = StripSpaces(model_name);
  }
  if (max_ext_leaf >= 0x80000001)
  {
    const Regs ext1 = Cpuid(0x80000001);
    bLZCNT = Bit(ext1.ecx, 5);
    bFMA4 = Bit(ext1.ecx, 16);
  }

  bFlushToZero = has_sse;  // MXCSR FTZ/DAZ
  bBMI2FastParallelBitOps = bBMI2 && !zen1_or_zen2;
  bCRC32 = bSSE4_2;

  // Summarize() separates fields with commas.
  model_name = ReplaceAll(model_name, ",", "_");
  cpu_id = ReplaceAll(cpu_id, ",", "_");
}

std::string CPUInfo::Summarize()
{
  std::vector<std::string> fields{model_name, cpu_id};
  const std::pair<bool, const char*> flags[] = {
      {bSSE3, "SSE3"},   {bSSSE3, "SSSE3"}, {bSSE4_1, "SSE4.1"}, {bSSE4_2, "SSE4.2"},
      {HTT, "HTT"},      {bAVX, "AVX"},     {bBMI1, "BMI1"},     {bBMI2, "BMI2"},
      {bFMA, "FMA"},     {bMOVBE, "MOVBE"}, {bAES, "AES"},       {bCRC32, "CRC32"},
      {bSHA1, "SHA1"},   {bSHA2, "SHA2"},
  };
  for (const auto& [present, name] : flags)
  {
    if (present)
      fields.emplace_back(name);
  }
  return fmt::to_string(fmt::join(fields, ","));
}
