// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/MMU.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#ifdef _M_X86_64
#include <emmintrin.h>
#endif

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "VideoCommon/EFBInterface.h"

namespace PowerPC
{
namespace
{
constexpr bool IsOpcodeFlag(XCheckTLBFlag flag)
{
  return flag == XCheckTLBFlag::Opcode || flag == XCheckTLBFlag::OpcodeNoException;
}

constexpr bool IsNoExceptionFlag(XCheckTLBFlag flag)
{
  return flag == XCheckTLBFlag::NoException || flag == XCheckTLBFlag::OpcodeNoException;
}

// Byte order conversion for every access size.
template <typename T>
T FromBigEndian(T value)
{
  if constexpr (sizeof(T) == 1)
    return value;
  else if constexpr (sizeof(T) == 2)
    return static_cast<T>(Common::swap16(static_cast<u16>(value)));
  else if constexpr (sizeof(T) == 4)
    return static_cast<T>(Common::swap32(static_cast<u32>(value)));
  else
    return static_cast<T>(Common::swap64(static_cast<u64>(value)));
}

template <typename T>
T LoadBigEndian(const u8* source)
{
  T value;
  std::memcpy(&value, source, sizeof(T));
  return FromBigEndian(value);
}

u32 LoadWordBE(const u8* source)
{
  return LoadBigEndian<u32>(source);
}

void StoreWordBE(u8* destination, u32 value)
{
  const u32 swapped = Common::swap32(value);
  std::memcpy(destination, &swapped, sizeof(swapped));
}

// ---------------------------------------------------------------------------
// Physical memory map constants.
// ---------------------------------------------------------------------------

// 0x08000000-0x0BFFFFFF is the EFB, 0x0C000000-0x0FFFFFFF the MMIO
// registers.
bool IsEFBOrMMIO(u32 physical)
{
  return (physical & 0xF8000000) == 0x08000000;
}

bool IsEFB(u32 physical)
{
  return physical < 0x0C000000;
}

// Games put the locked L1 at 0xE0000000 (the hardware lets it be anywhere).
constexpr u32 LOCKED_L1_BASE = 0xE0000000;

// With "fake VMEM" (MMU emulation off but a game that wants virtual memory),
// the backing store is placed at physical 0x7E000000-0x7FFFFFFF.
bool IsFakeVMEMAddress(u32 physical)
{
  return (physical & 0xFE000000) == 0x7E000000;
}

constexpr u32 DSISR_PAGE_FAULT = 1u << 30;
constexpr u32 DSISR_STORE = 1u << 25;

// WIMG with W or I set: the page is write-through or cache-inhibited.
bool IsWriteThroughOrInhibited(u32 wimg)
{
  return (wimg & 0b1100) != 0;
}

// ---------------------------------------------------------------------------
// The embedded framebuffer, as the CPU sees it: 4 bytes per pixel, rows
// 4 KiB apart. Bit 22 selects depth instead of colour; bit 23 (a combined Z
// and colour access) is not emulated.
// ---------------------------------------------------------------------------

u16 EFBColumn(u32 address)
{
  return static_cast<u16>((address & 0xFFF) >> 2);
}

u16 EFBRow(u32 address)
{
  return static_cast<u16>((address >> 12) & 0x3FF);
}

u32 EFBRead(u32 address)
{
  const u16 x = EFBColumn(address);
  const u16 y = EFBRow(address);

  if (address & 0x00800000)
  {
    ERROR_LOG_FMT(MEMMAP, "Unimplemented Z+Color EFB read @ {:#010x}", address);
    return 0;
  }

  if (address & 0x00400000)
  {
    const u32 depth = g_efb_interface->PeekDepth(x, y);
    DEBUG_LOG_FMT(MEMMAP, "EFB Z Read @ {}, {}\t= {:#010x}", x, y, depth);
    return depth;
  }

  const u32 color = g_efb_interface->PeekColor(x, y);
  DEBUG_LOG_FMT(MEMMAP, "EFB Color Read @ {}, {}\t= {:#010x}", x, y, color);
  return color;
}

void EFBWrite(u32 data, u32 address)
{
  const u16 x = EFBColumn(address);
  const u16 y = EFBRow(address);

  if (address & 0x00800000)
  {
    // A 64-bit write here is a Z-tested pixel write; nothing is known about
    // it beyond that.
    ERROR_LOG_FMT(MEMMAP, "Unimplemented Z+Color EFB write. {:08x} @ {:#010x}", data, address);
  }
  else if (address & 0x00400000)
  {
    g_efb_interface->PokeDepth(x, y, data);
    DEBUG_LOG_FMT(MEMMAP, "EFB Z Write {:08x} @ {}, {}", data, x, y);
  }
  else
  {
    g_efb_interface->PokeColor(x, y, data);
    DEBUG_LOG_FMT(MEMMAP, "EFB Color Write {:08x} @ {}, {}", data, x, y);
  }
}

bool TranslateThroughBAT(const BatTable& table, u32* address, bool* wi)
{
  const u32 entry = table[*address >> BAT_INDEX_SHIFT];
  if ((entry & BAT_MAPPED_BIT) == 0)
    return false;
  *address = (entry & BAT_RESULT_MASK) | (*address & (BAT_PAGE_SIZE - 1));
  *wi = (entry & BAT_WI_BIT) != 0;
  return true;
}

// ---------------------------------------------------------------------------
// Page table entries. Word 0: V(31) VSID(7-30) H(6) API(0-5). Word 1:
// RPN(12-31) R(8) C(7) WIMG(3-6) PP(0-1). Eight entries make a 64-byte PTE
// group (PTEG).
// ---------------------------------------------------------------------------

constexpr u32 PTE_VALID = 1u << 31;
constexpr u32 PTE_SECONDARY = 1u << 6;
constexpr u32 PTE_REFERENCED = 1u << 8;
constexpr u32 PTE_CHANGED = 1u << 7;
constexpr u32 PTE_RESERVED_BIT = 1u << 2;
constexpr u32 PTEG_SIZE = 64;
constexpr u32 PTE_SIZE = 8;

u32 PTEVsid(u32 word0)
{
  return (word0 >> 7) & 0xFFFFFF;
}

u32 PTEApi(u32 word0)
{
  return word0 & 0x3F;
}

bool PTEIsSecondary(u32 word0)
{
  return (word0 & PTE_SECONDARY) != 0;
}

u32 MappingPriority(u32 offset_in_table, u32 word0)
{
  return (offset_in_table % PTEG_SIZE / PTE_SIZE) | (PTEIsSecondary(word0) ? 8 : 0);
}

bool PTEGroupsEqual(const u8* a, const u8* b)
{
#ifdef _M_X86_64
  // A straight SSE2 compare: memcmp is not inlined well for this on MSVC,
  // and this runs over every PTEG of the table on each update.
  __m128i all = _mm_set1_epi8(-1);
  for (int i = 0; i < 4; ++i)
  {
    const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i * 16));
    const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i * 16));
    all = _mm_and_si128(all, _mm_cmpeq_epi8(x, y));
  }
  return _mm_movemask_epi8(all) == 0xFFFF;
#else
  return std::memcmp(a, b, PTEG_SIZE) == 0;
#endif
}

bool PTEsEqual(const u8* a, const u8* b)
{
  return std::memcmp(a, b, PTE_SIZE) == 0;
}

// ---------------------------------------------------------------------------
// Software TLB. Separate instruction and data TLBs, 64 sets of two ways,
// indexed by the low six bits of the effective page number.
// ---------------------------------------------------------------------------

enum class TLBHit
{
  Hit,
  Miss,
  // The page was found, but a store needs the C bit set in the page table
  // first; the cached copy already has it.
  NeedsChangedBit,
};

TLBEntry& TLBSetFor(PowerPCState& state, XCheckTLBFlag flag, u32 page_number)
{
  const size_t which = IsOpcodeFlag(flag) ? INST_TLB_INDEX : DATA_TLB_INDEX;
  return state.tlb[which][page_number & HW_PAGE_INDEX_MASK];
}

TLBHit LookUpTLB(PowerPCState& state, XCheckTLBFlag flag, u32 address, u32 vsid, u32* physical,
                 bool* wi)
{
  const u32 page_number = address >> HW_PAGE_INDEX_SHIFT;
  TLBEntry& set = TLBSetFor(state, flag, page_number);

  for (u32 way = 0; way < TLB_WAYS; ++way)
  {
    if (set.tag[way] != page_number || set.vsid[way] != vsid)
      continue;

    UPTE_Hi pte(set.pte[way]);
    if (flag == XCheckTLBFlag::Write && pte.C == 0)
    {
      pte.C = 1;
      set.pte[way] = pte.Hex;
      return TLBHit::NeedsChangedBit;
    }

    if (!IsNoExceptionFlag(flag))
      set.recent = way;

    *physical = set.paddr[way] | (address & static_cast<u32>(HW_PAGE_MASK));
    *wi = IsWriteThroughOrInhibited(pte.WIMG);
    return TLBHit::Hit;
  }
  return TLBHit::Miss;
}

void FillTLB(PowerPCState& state, XCheckTLBFlag flag, UPTE_Hi pte, u32 address, u32 vsid)
{
  if (IsNoExceptionFlag(flag))
    return;

  const u32 page_number = address >> HW_PAGE_INDEX_SHIFT;
  TLBEntry& set = TLBSetFor(state, flag, page_number);

  // Replace way 1 if way 0 was used last and holds something; otherwise
  // way 0.
  const u32 way = (set.recent == 0 && set.tag[0] != TLBEntry::INVALID_TAG) ? 1 : 0;
  set.recent = way;
  set.paddr[way] = pte.RPN << HW_PAGE_INDEX_SHIFT;
  set.pte[way] = pte.Hex;
  set.tag[way] = page_number;
  set.vsid[way] = vsid;
}
}  // namespace

MMU::MMU(Core::System& system, Memory::MemoryManager& memory, PowerPC::PowerPCManager& power_pc)
    : m_system(system), m_memory(memory), m_power_pc(power_pc), m_ppc_state(power_pc.GetPPCState())
{
}

MMU::~MMU() = default;

void MMU::Reset()
{
  ClearPageTable();
}

void MMU::DoState(PointerWrap& p, bool sr_changed)
{
  // The page table copy is saved rather than re-read from guest memory on
  // load, so a loaded state ends up with exactly the mappings it had.
  if (!p.IsReadMode())
  {
    p.Do(m_page_table);
    return;
  }

  if (!m_system.GetJitInterface().WantsPageTableMappings())
  {
    p.Do(m_page_table);
    ClearPageTable();
  }
  else if (sr_changed)
  {
    // Different segment registers: rebuild every mapping.
    p.Do(m_page_table);
    SRUpdated();
  }
  else
  {
    // Same segments: apply the saved table as an update of the current one.
    p.Do(m_scratch_page_table);
    ApplyPageTable(m_scratch_page_table);
  }
}

// ---------------------------------------------------------------------------
// Physical accesses
// ---------------------------------------------------------------------------

template <XCheckTLBFlag flag, std::unsigned_integral T, bool never_translate>
T MMU::ReadFromHardware(u32 em_address)
{
  // Real instruction fetches go through TryReadInstruction (which uses the
  // instruction cache); only host-side instruction reads come here.
  static_assert(flag == XCheckTLBFlag::NoException || flag == XCheckTLBFlag::Read ||
                flag == XCheckTLBFlag::OpcodeNoException);

  // An access that straddles two 4 KiB pages is done a byte at a time, since
  // the pages may translate differently.
  const size_t last_byte = em_address + sizeof(T) - 1;
  if ((em_address & ~HW_PAGE_MASK) != static_cast<u32>(last_byte & ~HW_PAGE_MASK))
  {
    T value = 0;
    for (u32 i = 0; i < sizeof(T); ++i)
      value = static_cast<T>((value << 8) | ReadFromHardware<flag, u8, never_translate>(em_address + i));
    return value;
  }

  bool wi = false;
  const bool translate =
      IsOpcodeFlag(flag) ? m_ppc_state.msr.IR.Value() != 0 : m_ppc_state.msr.DR.Value() != 0;
  if (!never_translate && translate)
  {
    const Translated result = Translate<flag>(em_address);
    if (!result.Succeeded())
    {
      if constexpr (flag == XCheckTLBFlag::Read)
        GenerateDSIException(em_address, false);
      return 0;
    }
    em_address = result.address;
    wi = result.wi;
  }

  if (flag == XCheckTLBFlag::Read && IsEFBOrMMIO(em_address))
  {
    if (IsEFB(em_address))
      return static_cast<T>(EFBRead(em_address));
    return static_cast<T>(m_memory.GetMMIOMapping()->Read<T>(m_system, em_address));
  }

  u8* const l1 = m_memory.GetL1Cache();
  if (l1 && (em_address >> 28) == 0xE && em_address < LOCKED_L1_BASE + m_memory.GetL1CacheSize())
    return LoadBigEndian<T>(&l1[em_address & 0x0FFFFFFF]);

  // Reads that may go through the data cache model. Host reads never
  // allocate cache blocks (they count as locked).
  const auto read_ram = [&](u8* base, u32 offset, u32 cache_address) -> T {
    if (!m_ppc_state.m_enable_dcache || wi)
      return LoadBigEndian<T>(&base[offset]);
    T value;
    const bool locked = HID0(m_ppc_state).DLOCK || flag != XCheckTLBFlag::Read;
    m_ppc_state.dCache.Read(m_memory, cache_address, &value, sizeof(T), locked);
    return FromBigEndian(value);
  };

  if (u8* const ram = m_memory.GetRAM(); ram && (em_address & 0xF8000000) == 0)
  {
    // MEM1 is mirrored throughout its 128 MiB window.
    const u32 offset = em_address & m_memory.GetRamMask();
    return read_ram(ram, offset, offset);
  }

  if (u8* const exram = m_memory.GetEXRAM();
      exram && (em_address >> 28) == 0x1 && (em_address & 0x0FFFFFFF) < m_memory.GetExRamSizeReal())
  {
    const u32 offset = em_address & 0x0FFFFFFF;
    return read_ram(exram, offset, offset + 0x10000000);
  }

  if (u8* const vmem = m_memory.GetFakeVMEM(); vmem && IsFakeVMEMAddress(em_address))
    return LoadBigEndian<T>(&vmem[em_address & m_memory.GetFakeVMemMask()]);

  // Nothing is there. The Game Boy Interface probes for MEM2 by reading
  // 0x10000000 from low addresses and expects only the PI interrupt, so that
  // one access does not alert.
  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_PI);
  if (em_address != 0x10000000 || (m_ppc_state.pc >> 28) != 0)
  {
    PanicAlertFmt("Unable to resolve read address {:x} PC {:x}", em_address, m_ppc_state.pc);
    if (m_system.IsPauseOnPanicMode())
    {
      m_system.GetCPU().Break();
      m_ppc_state.Exceptions |= EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT;
    }
  }
  return 0;
}

template <XCheckTLBFlag flag, bool never_translate>
void MMU::WriteToHardware(u32 em_address, const u32 data, const u32 size)
{
  static_assert(flag == XCheckTLBFlag::NoException || flag == XCheckTLBFlag::Write);
  DEBUG_ASSERT(size <= 4);

  // data holds the value in its low `size` bytes, most significant first in
  // memory order.
  const u32 first_page = em_address & ~static_cast<u32>(HW_PAGE_MASK);
  const u32 last_page = (em_address + size - 1) & ~static_cast<u32>(HW_PAGE_MASK);
  if (first_page != last_page)
  {
    // Split at the page boundary; each part may translate differently.
    const u32 head = last_page - em_address;
    const u32 tail = size - head;
    WriteToHardware<flag, never_translate>(em_address, std::rotr(data, tail * 8), head);
    WriteToHardware<flag, never_translate>(last_page, data, tail);
    return;
  }

  bool wi = false;
  if (!never_translate && m_ppc_state.msr.DR)
  {
    const Translated result = Translate<flag>(em_address);
    if (!result.Succeeded())
    {
      if constexpr (flag == XCheckTLBFlag::Write)
        GenerateDSIException(em_address, true);
      return;
    }
    em_address = result.address;
    wi = result.wi;
  }

  // The write-gather pipe matches on the 32-byte block (bits 0-26 in IBM
  // numbering), so the low address bits are ignored; Pac-Man World 3 depends
  // on that. The 750CL manual (9.4.2) warns that such writes put wrong data
  // in the buffer on hardware, but games that do it work.
  if (flag == XCheckTLBFlag::Write &&
      (em_address & 0xFFFFF000) == GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS)
  {
    auto& gather_pipe = m_system.GetGPFifo();
    if (size == 1)
      gather_pipe.Write8(static_cast<u8>(data));
    else if (size == 2)
      gather_pipe.Write16(static_cast<u16>(data));
    else if (size == 4)
      gather_pipe.Write32(data);
    else
      for (u32 shift = size * 8; shift != 0;)
        gather_pipe.Write8(static_cast<u8>(data >> (shift -= 8)));
    return;
  }

  if (flag == XCheckTLBFlag::Write && IsEFBOrMMIO(em_address))
  {
    if (IsEFB(em_address))
    {
      EFBWrite(data, em_address);
      return;
    }

    auto* mmio = m_memory.GetMMIOMapping();
    if (size == 1)
      mmio->Write<u8>(m_system, em_address, static_cast<u8>(data));
    else if (size == 2)
      mmio->Write<u16>(m_system, em_address, static_cast<u16>(data));
    else if (size == 4)
      mmio->Write<u32>(m_system, em_address, data);
    else
      for (u32 shift = size * 8; shift != 0; ++em_address)
        mmio->Write<u8>(m_system, em_address, static_cast<u8>(data >> (shift -= 8)));
    return;
  }

  // The bytes to store, in memory order, at the start of a word.
  const u32 bytes_be = Common::swap32(std::rotr(data, size * 8));

  u8* const l1 = m_memory.GetL1Cache();
  if (l1 && (em_address >> 28) == 0xE && em_address < LOCKED_L1_BASE + m_memory.GetL1CacheSize())
  {
    std::memcpy(&l1[em_address & 0x0FFFFFFF], &bytes_be, size);
    return;
  }

  if (wi && (size < 4 || (em_address & 3) != 0))
  {
    // An uncached or write-through store reaches the memory controller as a
    // 64-bit bus write with only one enable bit per 32-bit half. A store
    // smaller than a word, or not word aligned, therefore writes whole words:
    // the data, rotated into place, is repeated across every 32-bit half of
    // the 8-byte blocks it touches. (Hardware also flags the PI error
    // interrupt; its cause and address registers are not emulated.)
    m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_PI);

    const u32 word = std::rotr(data, ((em_address & 3) + size) * 8);
    const u32 begin = Common::AlignDown(em_address, 8);
    const u32 end = Common::AlignUp(em_address + size, 8);
    for (u32 block = begin; block != end; block += 8)
    {
      WriteToHardware<flag, true>(block, word, 4);
      WriteToHardware<flag, true>(block + 4, word, 4);
    }
    return;
  }

  // Stores to RAM. With the data cache model on, a cacheable guest store
  // only goes to the cache; host stores and uncached stores also (or only)
  // go to memory.
  const auto write_ram = [&](u8* base, u32 offset, u32 cache_address) {
    if (m_ppc_state.m_enable_dcache && !wi)
      m_ppc_state.dCache.Write(m_memory, cache_address, &bytes_be, size, HID0(m_ppc_state).DLOCK);
    if (!m_ppc_state.m_enable_dcache || wi || flag != XCheckTLBFlag::Write)
      std::memcpy(&base[offset], &bytes_be, size);
  };

  if (u8* const ram = m_memory.GetRAM(); ram && (em_address & 0xF8000000) == 0)
  {
    const u32 offset = em_address & m_memory.GetRamMask();
    write_ram(ram, offset, offset);
    return;
  }

  if (u8* const exram = m_memory.GetEXRAM();
      exram && (em_address >> 28) == 0x1 && (em_address & 0x0FFFFFFF) < m_memory.GetExRamSizeReal())
  {
    const u32 offset = em_address & 0x0FFFFFFF;
    write_ram(exram, offset, offset + 0x10000000);
    return;
  }

  if (u8* const vmem = m_memory.GetFakeVMEM(); vmem && IsFakeVMEMAddress(em_address))
  {
    std::memcpy(&vmem[em_address & m_memory.GetFakeVMemMask()], &bytes_be, size);
    return;
  }

  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_PI);
  PanicAlertFmt("Unable to resolve write address {:x} PC {:x}", em_address, m_ppc_state.pc);
  if (m_system.IsPauseOnPanicMode())
  {
    m_system.GetCPU().Break();
    m_ppc_state.Exceptions |= EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT;
  }
}

// ---------------------------------------------------------------------------
// Guest accesses
// ---------------------------------------------------------------------------

u32 MMU::Read_Opcode(u32 address)
{
  const TryReadInstResult result = TryReadInstruction(address);
  if (result.valid)
    return result.hex;
  GenerateISIException(address);
  return 0;
}

TryReadInstResult MMU::TryReadInstruction(u32 address)
{
  bool from_bat = true;
  if (m_ppc_state.msr.IR)
  {
    const Translated result = Translate<XCheckTLBFlag::Opcode>(address);
    if (!result.Succeeded())
      return TryReadInstResult{false, false, 0, 0};
    address = result.address;
    from_bat = result.kind == Translation::Block;
  }

  // Fake VMEM is read directly; the instruction cache model does not know
  // about it.
  u32 hex;
  if (u8* const vmem = m_memory.GetFakeVMEM(); vmem && IsFakeVMEMAddress(address))
    hex = LoadWordBE(&vmem[address & m_memory.GetFakeVMemMask()]);
  else
    hex = m_ppc_state.iCache.ReadInstruction(m_memory, m_ppc_state, address);

  return TryReadInstResult{true, from_bat, hex, address};
}

void MMU::Memcheck(u32 address, u64 var, bool write, size_t size)
{
  MemChecks& memchecks = m_power_pc.GetMemChecks();
  if (!memchecks.HasAny())
    return;

  TMemCheck* check = memchecks.GetMemCheck(address, size);
  if (check == nullptr)
    return;

  // While single-stepping, watchpoints are ignored so that stepping off one
  // works.
  if (m_system.GetCPU().IsStepping())
    return;

  ++check->num_hits;
  if (!check->Action(m_system, var, address, write, size, m_ppc_state.pc))
    return;

  m_system.GetCPU().Break();
  if (GDBStub::IsActive())
    GDBStub::TakeControl();

  // Stop before the access completes: the fake DSI makes the instruction
  // bail out like a real DSI would, without the guest seeing an exception.
  m_ppc_state.Exceptions |= EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT;
}

template <std::unsigned_integral T>
T MMU::Read(const u32 address)
{
  const T value = ReadFromHardware<XCheckTLBFlag::Read, T>(address);
  Memcheck(address, value, false, sizeof(T));
  return value;
}
template u8 MMU::Read<u8>(const u32 address);
template u16 MMU::Read<u16>(const u32 address);
template u32 MMU::Read<u32>(const u32 address);
template u64 MMU::Read<u64>(const u32 address);

template <std::unsigned_integral T>
void MMU::Write(const Common::MakeAtLeastU32<T> var, const u32 address)
{
  Memcheck(address, var, true, sizeof(T));
  WriteToHardware<XCheckTLBFlag::Write>(address, var, sizeof(T));
}
template void MMU::Write<u8>(const u32 var, const u32 address);
template void MMU::Write<u16>(const u32 var, const u32 address);
template void MMU::Write<u32>(const u32 var, const u32 address);
// Doubleword stores are two word stores, high word first.
template <>
void MMU::Write<u64>(const u64 var, const u32 address)
{
  Memcheck(address, var, true, sizeof(u64));
  WriteToHardware<XCheckTLBFlag::Write>(address, static_cast<u32>(var >> 32), 4);
  WriteToHardware<XCheckTLBFlag::Write>(address + 4, static_cast<u32>(var), 4);
}

void MMU::Write_U16_Swap(const u32 var, const u32 address)
{
  const u32 swapped_low = Common::swap16(static_cast<u16>(var));
  Write<u16>((var & 0xFFFF0000) | swapped_low, address);
}

void MMU::Write_U32_Swap(const u32 var, const u32 address)
{
  Write<u32>(Common::swap32(var), address);
}

void MMU::Write_U64_Swap(const u64 var, const u32 address)
{
  Write<u64>(Common::swap64(var), address);
}

// ---------------------------------------------------------------------------
// Host accesses
// ---------------------------------------------------------------------------

template <std::unsigned_integral T>
T MMU::HostRead(const Core::CPUThreadGuard& guard, const u32 address)
{
  return guard.GetSystem().GetMMU().ReadFromHardware<XCheckTLBFlag::NoException, T>(address);
}
template u8 MMU::HostRead<u8>(const Core::CPUThreadGuard& guard, const u32 address);
template u16 MMU::HostRead<u16>(const Core::CPUThreadGuard& guard, const u32 address);
template u32 MMU::HostRead<u32>(const Core::CPUThreadGuard& guard, const u32 address);
template u64 MMU::HostRead<u64>(const Core::CPUThreadGuard& guard, const u32 address);

u32 MMU::HostRead_Instruction(const Core::CPUThreadGuard& guard, const u32 address)
{
  return guard.GetSystem().GetMMU().ReadFromHardware<XCheckTLBFlag::OpcodeNoException, u32>(
      address);
}

template <std::unsigned_integral T>
void MMU::HostWrite(const Core::CPUThreadGuard& guard, const Common::MakeAtLeastU32<T> var,
                    const u32 address)
{
  guard.GetSystem().GetMMU().WriteToHardware<XCheckTLBFlag::NoException>(address, var, sizeof(T));
}
// HostWrite<u64> is specialised below as two u32 writes.
template void MMU::HostWrite<u32>(const Core::CPUThreadGuard&, u32, u32);
template void MMU::HostWrite<u16>(const Core::CPUThreadGuard&, u32, u32);
template void MMU::HostWrite<u8>(const Core::CPUThreadGuard&, u32, u32);
template <>
void MMU::HostWrite<u64>(const Core::CPUThreadGuard& guard, const u64 var, const u32 address)
{
  MMU& mmu = guard.GetSystem().GetMMU();
  mmu.WriteToHardware<XCheckTLBFlag::NoException>(address, static_cast<u32>(var >> 32), 4);
  mmu.WriteToHardware<XCheckTLBFlag::NoException>(address + 4, static_cast<u32>(var), 4);
}

template <std::unsigned_integral T>
std::optional<ReadResult<T>> MMU::HostTryRead(const Core::CPUThreadGuard& guard, const u32 address,
                                              RequestedAddressSpace space)
{
  if (!HostIsRAMAddress(guard, address, space))
    return std::nullopt;

  MMU& mmu = guard.GetSystem().GetMMU();
  if (space == RequestedAddressSpace::Physical)
    return ReadResult<T>(false, mmu.ReadFromHardware<XCheckTLBFlag::NoException, T, true>(address));

  const bool translated = mmu.m_ppc_state.msr.DR;
  if (space == RequestedAddressSpace::Virtual && !translated)
    return std::nullopt;
  return ReadResult<T>(translated, mmu.ReadFromHardware<XCheckTLBFlag::NoException, T>(address));
}
// Every width the debugger and other host code read with.
template std::optional<ReadResult<u64>> MMU::HostTryRead<u64>(const Core::CPUThreadGuard&, u32,
                                                         RequestedAddressSpace);
template std::optional<ReadResult<u32>> MMU::HostTryRead<u32>(const Core::CPUThreadGuard&, u32,
                                                         RequestedAddressSpace);
template std::optional<ReadResult<u16>> MMU::HostTryRead<u16>(const Core::CPUThreadGuard&, u32,
                                                         RequestedAddressSpace);
template std::optional<ReadResult<u8>> MMU::HostTryRead<u8>(const Core::CPUThreadGuard&, u32,
                                                        RequestedAddressSpace);

std::optional<ReadResult<u32>> MMU::HostTryReadInstruction(const Core::CPUThreadGuard& guard,
                                                           const u32 address,
                                                           RequestedAddressSpace space)
{
  if (!HostIsInstructionRAMAddress(guard, address, space))
    return std::nullopt;

  MMU& mmu = guard.GetSystem().GetMMU();
  if (space == RequestedAddressSpace::Physical)
  {
    return ReadResult<u32>(
        false, mmu.ReadFromHardware<XCheckTLBFlag::OpcodeNoException, u32, true>(address));
  }

  const bool translated = mmu.m_ppc_state.msr.IR;
  if (space == RequestedAddressSpace::Virtual && !translated)
    return std::nullopt;
  return ReadResult<u32>(translated,
                         mmu.ReadFromHardware<XCheckTLBFlag::OpcodeNoException, u32>(address));
}

template <std::unsigned_integral T>
std::optional<WriteResult> MMU::HostTryWrite(const Core::CPUThreadGuard& guard,
                                             const Common::MakeAtLeastU32<T> var, const u32 address,
                                             RequestedAddressSpace space)
{
  if (!HostIsRAMAddress(guard, address, space))
    return std::nullopt;

  MMU& mmu = guard.GetSystem().GetMMU();
  if (space == RequestedAddressSpace::Physical)
  {
    mmu.WriteToHardware<XCheckTLBFlag::NoException, true>(address, var, sizeof(T));
    return WriteResult(false);
  }

  const bool translated = mmu.m_ppc_state.msr.DR;
  if (space == RequestedAddressSpace::Virtual && !translated)
    return std::nullopt;
  mmu.WriteToHardware<XCheckTLBFlag::NoException>(address, var, sizeof(T));
  return WriteResult(translated);
}
// u64 is split into two u32 writes below; the other widths use the template.
template std::optional<WriteResult> MMU::HostTryWrite<u32>(const Core::CPUThreadGuard&, u32, u32,
                                                           RequestedAddressSpace);
template std::optional<WriteResult> MMU::HostTryWrite<u16>(const Core::CPUThreadGuard&, u32, u32,
                                                           RequestedAddressSpace);
template std::optional<WriteResult> MMU::HostTryWrite<u8>(const Core::CPUThreadGuard&, u32, u32,
                                                          RequestedAddressSpace);

template <>
std::optional<WriteResult> MMU::HostTryWrite<u64>(const Core::CPUThreadGuard& guard, const u64 var,
                                                  const u32 address, RequestedAddressSpace space)
{
  const auto high = HostTryWrite<u32>(guard, static_cast<u32>(var >> 32), address, space);
  if (!high)
    return high;
  return HostTryWrite<u32>(guard, static_cast<u32>(var), address + 4, space);
}

std::string MMU::HostGetString(const Core::CPUThreadGuard& guard, u32 address, size_t size)
{
  // size == 0 means "up to the terminator".
  std::string text;
  while (HostIsRAMAddress(guard, address))
  {
    const u8 c = HostRead<u8>(guard, address);
    if (c == 0)
      break;
    text.push_back(static_cast<char>(c));
    ++address;
    if (size != 0 && text.length() >= size)
      break;
  }
  return text;
}

std::u16string MMU::HostGetU16String(const Core::CPUThreadGuard& guard, u32 address, size_t size)
{
  std::u16string text;
  while (HostIsRAMAddress(guard, address) && HostIsRAMAddress(guard, address + 1))
  {
    const u16 c = HostRead<u16>(guard, address);
    if (c == 0)
      break;
    text.push_back(static_cast<char16_t>(c));
    address += 2;
    if (size != 0 && text.length() >= size)
      break;
  }
  return text;
}

std::optional<ReadResult<std::string>> MMU::HostTryReadString(const Core::CPUThreadGuard& guard,
                                                              u32 address, size_t size,
                                                              RequestedAddressSpace space)
{
  // Whether the address was translated is decided by the first byte.
  const auto first = HostTryRead<u8>(guard, address, space);
  if (!first)
    return std::nullopt;

  std::string text;
  for (auto c = first; c && c->value != 0;)
  {
    text.push_back(static_cast<char>(c->value));
    if (size != 0 && text.length() >= size)
      break;
    c = HostTryRead<u8>(guard, ++address, space);
  }
  return ReadResult<std::string>(first->translated, std::move(text));
}

bool MMU::IsPhysicalRAMAddress(const u32 address) const
{
  const u32 segment = address >> 28;
  const u32 offset = address & 0x0FFFFFFF;
  auto& memory = m_memory;
  if (memory.GetRAM() && segment == 0x0 && offset < memory.GetRamSizeReal())
    return true;
  if (memory.GetEXRAM() && segment == 0x1 && offset < memory.GetExRamSizeReal())
    return true;
  if (memory.GetFakeVMEM() && IsFakeVMEMAddress(address))
    return true;
  return memory.GetL1Cache() && segment == 0xE &&
         address < LOCKED_L1_BASE + memory.GetL1CacheSize();
}

template <XCheckTLBFlag flag>
bool MMU::IsEffectiveRAMAddress(const u32 address)
{
  const Translated result = Translate<flag>(address);
  return result.Succeeded() && IsPhysicalRAMAddress(result.address);
}

bool MMU::HostIsRAMAddress(const Core::CPUThreadGuard& guard, u32 address,
                           RequestedAddressSpace space)
{
  MMU& mmu = guard.GetSystem().GetMMU();
  const bool translating = mmu.m_ppc_state.msr.DR;
  if (space == RequestedAddressSpace::Physical ||
      (space == RequestedAddressSpace::Effective && !translating))
  {
    return mmu.IsPhysicalRAMAddress(address);
  }
  if (!translating)
    return false;
  return mmu.IsEffectiveRAMAddress<XCheckTLBFlag::NoException>(address);
}

bool MMU::HostIsInstructionRAMAddress(const Core::CPUThreadGuard& guard, u32 address,
                                      RequestedAddressSpace space)
{
  // Instructions are word aligned.
  if ((address & 3) != 0)
    return false;

  MMU& mmu = guard.GetSystem().GetMMU();
  const bool translating = mmu.m_ppc_state.msr.IR;
  if (space == RequestedAddressSpace::Physical ||
      (space == RequestedAddressSpace::Effective && !translating))
  {
    return mmu.IsPhysicalRAMAddress(address);
  }
  if (!translating)
    return false;
  return mmu.IsEffectiveRAMAddress<XCheckTLBFlag::OpcodeNoException>(address);
}

// ---------------------------------------------------------------------------
// Locked cache DMA. Offsets into the locked cache wrap at 256 KiB.
// ---------------------------------------------------------------------------

void MMU::DMA_LCToMemory(const u32 mem_address, const u32 cache_address, const u32 num_blocks)
{
  u8* const l1 = m_memory.GetL1Cache();
  const u32 length = 32 * num_blocks;
  const u32 region = mem_address & 0x0F000000;

  // Avatar: The Last Airbender (GameCube) DMAs its videos to the EFB. The
  // MMIO case is not known to be used.
  if (region == 0x08000000 || region == 0x0C000000)
  {
    for (u32 i = 0; i < length; i += 4)
    {
      const u32 word = LoadWordBE(l1 + ((cache_address + i) & 0x3FFFF));
      if (region == 0x08000000)
        EFBWrite(word, mem_address + i);
      else
        m_memory.GetMMIOMapping()->Write(m_system, mem_address + i, word);
    }
    return;
  }

  m_memory.CopyToEmu(mem_address, l1 + (cache_address & 0x3FFFF), length);
}

void MMU::DMA_MemoryToLC(const u32 cache_address, const u32 mem_address, const u32 num_blocks)
{
  u8* const l1 = m_memory.GetL1Cache();
  const u32 length = 32 * num_blocks;
  const u32 region = mem_address & 0x0F000000;

  // Neither case is known to be used by a game.
  if (region == 0x08000000 || region == 0x0C000000)
  {
    for (u32 i = 0; i < length; i += 4)
    {
      const u32 word = region == 0x08000000 ?
                           EFBRead(mem_address + i) :
                           m_memory.GetMMIOMapping()->Read<u32>(m_system, mem_address + i);
      StoreWordBE(l1 + ((cache_address + i) & 0x3FFFF), word);
    }
    return;
  }

  m_memory.CopyFromEmu(l1 + (cache_address & 0x3FFFF), mem_address, length);
}

// ---------------------------------------------------------------------------
// Cache block instructions
// ---------------------------------------------------------------------------

// Translates the address of a cache block instruction. Returns false if the
// instruction does nothing: a direct-store segment (dcbz there is ignored,
// as on hardware and in the PEM; Advance Game Port relies on it) or a page
// fault (which raises a DSI unless told otherwise).
template <XCheckTLBFlag flag>
bool MMU::PrepareDCacheOp(u32* address, bool fault_raises_dsi)
{
  if (!m_ppc_state.msr.DR)
    return true;

  const Translated result = Translate<flag>(*address);
  if (result.kind == Translation::DirectStore)
    return false;
  if (result.kind == Translation::Fault)
  {
    if (fault_raises_dsi)
      GenerateDSIException(*address, true);
    return false;
  }
  *address = result.address;
  return true;
}

void MMU::ClearDCacheLine(u32 address)
{
  DEBUG_ASSERT((address & 0x1F) == 0);
  if (!PrepareDCacheOp<XCheckTLBFlag::Write>(&address, true))
    return;

  // Not exact for non-RAM targets, but nothing is known to depend on it.
  for (u32 i = 0; i < 32; i += 4)
    WriteToHardware<XCheckTLBFlag::Write, true>(address + i, 0, 4);
}

void MMU::StoreDCacheLine(u32 address)
{
  address &= ~0x1F;
  if (!PrepareDCacheOp<XCheckTLBFlag::Write>(&address, true))
    return;
  if (m_ppc_state.m_enable_dcache)
    m_ppc_state.dCache.Store(m_memory, address);
}

void MMU::InvalidateDCacheLine(u32 address)
{
  address &= ~0x1F;
  if (!PrepareDCacheOp<XCheckTLBFlag::Write>(&address, false))
    return;
  if (m_ppc_state.m_enable_dcache)
    m_ppc_state.dCache.Invalidate(m_memory, address);
}

void MMU::FlushDCacheLine(u32 address)
{
  address &= ~0x1F;
  if (!PrepareDCacheOp<XCheckTLBFlag::Write>(&address, true))
    return;
  if (m_ppc_state.m_enable_dcache)
    m_ppc_state.dCache.Flush(m_memory, address);
}

void MMU::TouchDCacheLine(u32 address, bool store)
{
  address &= ~0x1F;
  if (!PrepareDCacheOp<XCheckTLBFlag::Write>(&address, true))
    return;
  if (m_ppc_state.m_enable_dcache)
    m_ppc_state.dCache.Touch(m_memory, address, store);
}

// ---------------------------------------------------------------------------
// JIT queries
// ---------------------------------------------------------------------------

bool MMU::IsOptimizableRAMAddress(const u32 address, const u32 access_size) const
{
  if (m_power_pc.GetMemChecks().HasAny() || !m_ppc_state.msr.DR || m_ppc_state.m_enable_dcache)
    return false;

  // access_size is in bits. Both ends must be in fastmem-safe BAT blocks.
  const u32 last = address + (access_size >> 3) - 1;
  const u32 first_entry = m_dbat_table[address >> BAT_INDEX_SHIFT];
  const u32 last_entry = m_dbat_table[last >> BAT_INDEX_SHIFT];
  return ((first_entry & last_entry) & BAT_PHYSICAL_BIT) != 0;
}

u32 MMU::IsOptimizableMMIOAccess(u32 address, u32 access_size) const
{
  if (m_power_pc.GetMemChecks().HasAny() || !m_ppc_state.msr.DR || m_ppc_state.m_enable_dcache)
    return 0;

  // Only BAT translations are considered, so that TLB changes never have to
  // invalidate JIT code.
  bool wi = false;
  if (!TranslateThroughBAT(m_dbat_table, &address, &wi))
    return 0;

  const u32 alignment_mask = (access_size >> 3) - 1;
  if ((address & alignment_mask) != 0 || !MMIO::IsMMIOAddress(address, m_system.IsWii()))
    return 0;
  return address;
}

bool MMU::IsOptimizableGatherPipeWrite(u32 address) const
{
  if (m_power_pc.GetMemChecks().HasAny() || !m_ppc_state.msr.DR)
    return false;

  bool wi = false;
  if (!TranslateThroughBAT(m_dbat_table, &address, &wi))
    return false;
  return address == GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS;
}

TranslateResult MMU::JitCache_TranslateAddress(u32 address)
{
  if (!m_ppc_state.msr.IR)
    return TranslateResult{address};

  // This counts as a real instruction fetch (it updates the TLB), also when
  // the debugger asks.
  const Translated result = Translate<XCheckTLBFlag::Opcode>(address);
  if (!result.Succeeded())
    return TranslateResult{};
  return TranslateResult{result.kind == Translation::Block, result.address};
}

std::optional<u32> MMU::GetTranslatedAddress(u32 address)
{
  const Translated result = Translate<XCheckTLBFlag::NoException>(address);
  if (!result.Succeeded())
    return std::nullopt;
  return result.address;
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

namespace
{
// Shown when a translation fails without MMU emulation: the game went wrong, and raising an
// exception it has no handler for would not help.
void AlertInvalidAccess(bool write, u32 effective_address, u32 pc)
{
  if (!write)
  {
    PanicAlertFmtT(
        "Invalid read from {0:#010x}, PC = {1:#010x}.\n\nThe game probably would have crashed on "
        "real hardware. Enable MMU in advanced settings to accurately emulate game crashes.",
        effective_address, pc);
    return;
  }
  PanicAlertFmtT(
      "Invalid write to {0:#010x}, PC = {1:#010x}.\n\nThe game probably would have crashed on "
      "real hardware. Enable MMU in advanced settings to accurately emulate game crashes.",
      effective_address, pc);
}
}  // namespace

void MMU::GenerateDSIException(u32 effective_address, bool write)
{
  if (m_system.IsMMUMode())
  {
    m_ppc_state.spr[SPR_DSISR] = DSISR_PAGE_FAULT | (write ? DSISR_STORE : 0);
    m_ppc_state.spr[SPR_DAR] = effective_address;
    m_ppc_state.Exceptions |= EXCEPTION_DSI;
    return;
  }

  AlertInvalidAccess(write, effective_address, m_ppc_state.pc);
  // With pause-on-panic the CPU stops, and the access is treated like a memcheck hit so the
  // instruction does not complete.
  if (m_system.IsPauseOnPanicMode())
  {
    m_system.GetCPU().Break();
    m_ppc_state.Exceptions |= EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT;
  }
}

void MMU::GenerateISIException(u32 effective_address)
{
  // The interrupt returns to the instruction that could not be fetched.
  m_ppc_state.npc = effective_address;
  m_ppc_state.Exceptions |= EXCEPTION_ISI;
  WARN_LOG_FMT(POWERPC, "ISI exception at {:#010x}", m_ppc_state.pc);
}

// ---------------------------------------------------------------------------
// Translation state
// ---------------------------------------------------------------------------

void MMU::SDRUpdated()
{
  const UReg_SDR1 sdr{m_ppc_state.spr[SPR_SDR]};
  const u32 htabmask = sdr.htabmask;
  const u32 htaborg = sdr.htaborg;

  if (!Common::IsValidLowMask(htabmask))
    WARN_LOG_FMT(POWERPC, "Invalid HTABMASK: 0b{:032b}", htabmask);

  // The PEM (7.6.1.1) asks for HTABORG to be aligned to the table size, but
  // hardware simply ORs the hash into it, so a misaligned base is honoured
  // the same way.
  if ((htaborg & htabmask) != 0)
    WARN_LOG_FMT(POWERPC, "Invalid HTABORG: htaborg=0x{:08x} htabmask=0x{:08x}", htaborg, htabmask);

  m_ppc_state.pagetable_base = htaborg << 16;
  m_ppc_state.pagetable_mask = (htabmask << 16) | 0xFFC0;

  PageTableUpdated();
}

void MMU::SRUpdated()
{
  // The incremental update relies on the segment registers staying the
  // same, so start over.
  m_memory.RemoveAllPageTableMappings();
  ReloadPageTable();
}

void MMU::InvalidateTLBEntry(u32 address)
{
  const u32 set = (address >> HW_PAGE_INDEX_SHIFT) & HW_PAGE_INDEX_MASK;
  m_ppc_state.tlb[DATA_TLB_INDEX][set].Invalidate();
  m_ppc_state.tlb[INST_TLB_INDEX][set].Invalidate();

  // tlbie is how software announces page table changes. With data
  // translation off the table may be in the middle of being rewritten, so
  // the update waits until MSR[DR] is set again.
  if (m_ppc_state.msr.DR)
    PageTableUpdated();
  else
    m_ppc_state.pagetable_update_pending = true;
}

void MMU::PageTableUpdatedFromJit(MMU* mmu)
{
  mmu->PageTableUpdated();
}

template <XCheckTLBFlag flag>
MMU::Translated MMU::Translate(u32 address)
{
  // Hardware looks up BATs and the TLB in parallel and a BAT hit wins.
  bool wi = false;
  const BatTable& bats = IsOpcodeFlag(flag) ? m_ibat_table : m_dbat_table;
  if (TranslateThroughBAT(bats, &address, &wi))
    return Translated{Translation::Block, address, wi};
  return TranslateThroughPageTable<flag>(address);
}

template <XCheckTLBFlag flag>
MMU::Translated MMU::TranslateThroughPageTable(const u32 address)
{
  const UReg_SR segment{m_ppc_state.sr[address >> 28]};
  const u32 vsid = segment.VSID;

  // Page translations always report wi = false: the uncached/write-through
  // handling (and the data cache bypass) is applied to BAT mappings only,
  // which is the long-standing behaviour games are tested against.
  bool tlb_wi = false;
  u32 physical = 0;
  const TLBHit tlb = LookUpTLB(m_ppc_state, flag, address, vsid, &physical, &tlb_wi);
  if (tlb == TLBHit::Hit)
    return Translated{Translation::Page, physical, false};

  if (segment.T != 0)
    return Translated{Translation::DirectStore, 0, false};

  // The Ks/Kp protection keys are not emulated; the no-execute bit is.
  if (IsOpcodeFlag(flag) && segment.N != 0)
    return Translated{Translation::Fault, 0, false};

  const u32 page_index = (address >> 12) & 0xFFFF;
  const u32 api = (address >> 22) & 0x3F;

  // The PTE word 0 to look for, and the primary hash (the secondary hash is
  // its complement).
  u32 wanted = PTE_VALID | (vsid << 7) | api;
  u32 hash = vsid ^ page_index;

  constexpr XCheckTLBFlag pte_read_flag =
      IsNoExceptionFlag(flag) ? XCheckTLBFlag::NoException : XCheckTLBFlag::Read;

  for (int pass = 0; pass < 2; ++pass)
  {
    if (pass == 1)
    {
      hash = ~hash;
      wanted |= PTE_SECONDARY;
    }

    const u32 pteg = ((hash << 6) & m_ppc_state.pagetable_mask) | m_ppc_state.pagetable_base;
    for (u32 slot = 0; slot < 8; ++slot)
    {
      const u32 pte_address = pteg + slot * PTE_SIZE;
      // PTEs are read physically (and through the data cache model).
      if (ReadFromHardware<pte_read_flag, u32, true>(pte_address) != wanted)
        continue;

      UPTE_Hi pte(ReadFromHardware<pte_read_flag, u32, true>(pte_address + 4));
      const u32 before = pte.Hex;
      if constexpr (flag == XCheckTLBFlag::Read || flag == XCheckTLBFlag::Opcode)
      {
        pte.R = 1;
      }
      else if constexpr (flag == XCheckTLBFlag::Write)
      {
        pte.R = 1;
        pte.C = 1;
      }

      if (!IsNoExceptionFlag(flag) && pte.Hex != before)
      {
        m_memory.Write_U32(pte.Hex, pte_address + 4);
        UpdateHostMappingForRC(address & ~static_cast<u32>(HW_PAGE_MASK), pte_address, wanted,
                               pte.Hex);
      }

      // A TLB hit that only lacked the C bit has its entry already.
      if (tlb != TLBHit::NeedsChangedBit)
        FillTLB(m_ppc_state, flag, pte, address, vsid);

      return Translated{Translation::Page, (pte.RPN << 12) | (address & 0xFFF), false};
    }
  }
  return Translated{Translation::Fault, 0, false};
}

// ---------------------------------------------------------------------------
// BATs
// ---------------------------------------------------------------------------

void MMU::UpdateBATs(BatTable& bat_table, u32 base_spr)
{
  // Not emulated: separate supervisor/user BATs (both valid bits count the
  // same), the PP protection bits, and whatever hardware does with
  // overlapping or malformed BATs (they are applied as below, later BATs
  // winning).
  for (u32 n = 0; n < 4; ++n)
  {
    const UReg_BAT_Up upper{m_ppc_state.spr[base_spr + n * 2]};
    const UReg_BAT_Lo lower{m_ppc_state.spr[base_spr + n * 2 + 1]};
    if (upper.VS == 0 && upper.VP == 0)
      continue;

    const u32 length_mask = upper.BL;
    if ((upper.BEPI & length_mask) != 0)
      WARN_LOG_FMT(POWERPC, "Bad BAT setup: BEPI overlaps BL");
    if ((lower.BRPN & length_mask) != 0)
      WARN_LOG_FMT(POWERPC, "Bad BAT setup: BPRN overlaps BL");
    if (!Common::IsValidLowMask(length_mask))
      WARN_LOG_FMT(POWERPC, "Bad BAT setup: invalid mask in BL");

    const bool wi = IsWriteThroughOrInhibited(lower.WIMG);

    // Every 128 KiB block the BAT covers: all values that only use bits of
    // the length mask, ORed into both block numbers.
    for (u32 block = 0; block <= length_mask; ++block)
    {
      if ((block & length_mask) != block)
        continue;

      const u32 physical = (lower.BRPN | block) << BAT_INDEX_SHIFT;
      const u32 effective = (upper.BEPI | block) << BAT_INDEX_SHIFT;

      u32 entry = physical | BAT_MAPPED_BIT;
      if (wi)
      {
        // Uncached quirks cannot be emulated through fastmem, so such
        // blocks are never accessed directly.
        entry |= BAT_WI_BIT;
      }
      else
      {
        const bool fake_vmem = m_memory.GetFakeVMEM() && IsFakeVMEMAddress(physical);
        const bool mem1 = physical < m_memory.GetRamSizeReal();
        const bool mem2 = m_memory.GetEXRAM() && (physical >> 28) == 0x1 &&
                          (physical & 0x0FFFFFFF) < m_memory.GetExRamSizeReal();
        const bool locked_l1 = (physical >> 28) == 0xE &&
                               physical < LOCKED_L1_BASE + m_memory.GetL1CacheSize();
        if (fake_vmem || mem1 || mem2 || locked_l1)
          entry |= BAT_PHYSICAL_BIT;
      }

      // Watched blocks must take the slow path.
      if (m_power_pc.GetMemChecks().OverlapsMemcheck(effective, BAT_PAGE_SIZE))
        entry &= ~BAT_PHYSICAL_BIT;

      bat_table[effective >> BAT_INDEX_SHIFT] = entry;
    }
  }
}

void MMU::UpdateFakeMMUBat(BatTable& bat_table, u32 start_addr)
{
  // Maps the 256 MiB at start_addr onto fake VMEM at 0x7E000000.
  constexpr u32 blocks = 0x10000000 >> BAT_INDEX_SHIFT;
  const u32 first = start_addr >> BAT_INDEX_SHIFT;
  for (u32 i = 0; i < blocks; ++i)
  {
    const u32 physical = 0x7E000000 | ((i << BAT_INDEX_SHIFT) & m_memory.GetFakeVMemMask());
    u32 entry = physical | BAT_MAPPED_BIT | BAT_PHYSICAL_BIT;
    if (m_power_pc.GetMemChecks().OverlapsMemcheck((first + i) << BAT_INDEX_SHIFT, BAT_PAGE_SIZE))
      entry &= ~BAT_PHYSICAL_BIT;
    bat_table[first + i] = entry;
  }
}

void MMU::RebuildBATTable(BatTable& bat_table, u32 base_spr, u32 extended_base_spr)
{
  bat_table.fill(0);
  UpdateBATs(bat_table, base_spr);
  // Broadway's BATs 4-7 exist only when HID4[SBE] is set.
  if (m_system.IsWii() && HID4(m_ppc_state).SBE)
    UpdateBATs(bat_table, extended_base_spr);
  if (m_memory.GetFakeVMEM())
  {
    UpdateFakeMMUBat(bat_table, 0x40000000);
    UpdateFakeMMUBat(bat_table, 0x70000000);
  }
}

void MMU::DBATUpdated()
{
  RebuildBATTable(m_dbat_table, SPR_DBAT0U, SPR_DBAT4U);

#ifndef _ARCH_32
  // Remapping the BATs throws away the fastmem page-table mappings; put
  // them back (BATs and watchpoints may now shadow different pages).
  m_memory.UpdateDBATMappings(m_dbat_table);
  if (!m_page_table.empty())
    ReloadPageTable();
#endif

  // JIT code bakes in BAT-based decisions (IsOptimizable*, dcbz).
  m_system.GetJitInterface().ClearSafe();
}

void MMU::IBATUpdated()
{
  RebuildBATTable(m_ibat_table, SPR_IBAT0U, SPR_IBAT4U);
  m_system.GetJitInterface().ClearSafe();
}

// ---------------------------------------------------------------------------
// Fastmem page-table mappings
//
// m_page_table holds the guest page table as last processed. On each update
// the guest's current table is compared with it, PTE by PTE; for every PTE
// that changed, the mapping its old contents described is withdrawn and the
// one its new contents describe is recorded (if it wins the priority order
// for its page). The host mappings that changed are then removed and added
// in one batch.
//
// Priority follows the hardware search order: a PTE in the primary PTEG
// beats one in the secondary PTEG, and within a PTEG the earlier slot wins.
// Primary PTEs are handled in a first linear pass; secondary ones need a
// second pass, because the primary PTEG that outranks a secondary PTEG lies
// elsewhere in the table and must be processed first.
// ---------------------------------------------------------------------------

void MMU::ClearPageTable()
{
  // Mappings were dropped without being tracked, so incremental updates
  // cannot continue from here.
  m_memory.RemoveAllPageTableMappings();
  m_page_mappings.clear();
  m_page_table.clear();
}

void MMU::ReloadPageTable()
{
  // Re-applies the last processed table from scratch.
  m_page_mappings.clear();
  m_scratch_page_table.clear();
  std::swap(m_page_table, m_scratch_page_table);

  if (m_system.GetJitInterface().WantsPageTableMappings())
    ApplyPageTable(m_scratch_page_table);
  else
    m_memory.RemoveAllPageTableMappings();
}

void MMU::PageTableUpdated()
{
  m_ppc_state.pagetable_update_pending = false;

  // Without a JIT that uses the mappings, keeping them up to date is wasted
  // work.
  if (!m_system.GetJitInterface().WantsPageTableMappings())
  {
    ClearPageTable();
    return;
  }

  const u32 base = m_ppc_state.pagetable_base;
  const u32 end = Common::AlignUp(base | m_ppc_state.pagetable_mask, PAGE_TABLE_MIN_SIZE);
  const u32 size = end - base;

  const u8* table = m_system.GetMemory().GetPointerForRange(base, size);
  if (table == nullptr)
  {
    WARN_LOG_FMT(POWERPC, "Failed to read page table at {:#010x}-{:#010x}", base, end);
    ClearPageTable();
    return;
  }

  ApplyPageTable(std::span(table, size));
}

std::optional<u32> MMU::PageAddressFromPTE(u32 pte_word0, u32 pteg_index) const
{
  // Undo the hash: the PTEG index is (hash & mask) with hash = VSID ^ page
  // index (complemented for the secondary hash). Page index bits 0-9 always
  // come out of that; bits 10-15 only where HTABMASK keeps hash bits, and
  // they are also stored in the PTE as the API. Where both are known they
  // must agree.
  u32 page_index = pteg_index ^ PTEVsid(pte_word0);
  if (PTEIsSecondary(pte_word0))
    page_index = ~page_index;

  const u32 known_api_bits =
      ((m_ppc_state.pagetable_mask & ~m_ppc_state.pagetable_base) >> 16) & 0x3F;
  const u32 api = PTEApi(pte_word0);
  if ((api & known_api_bits) != ((page_index >> 10) & known_api_bits))
    return std::nullopt;

  return ((page_index & 0x3FF) << 12) | (api << 22);
}

void MMU::RecheckShadowedPTE(u32 pte_word0, u32 offset, std::span<const u8> page_table,
                             bool* needs_secondary_pass)
{
  // A mapping is being withdrawn. A lower-priority PTE for the same page
  // (later in this PTEG, or in the secondary PTEG) may now be the one that
  // counts. If that PTE did not change itself it would be skipped, so its
  // saved copy is altered in an unused bit to make it look changed.
  u8* saved = m_page_table.data();
  const u8* current = page_table.data();
  u32 wanted = pte_word0;
  bool in_secondary = false;

  while (true)
  {
    const u32 group = Common::AlignDown(offset, PTEG_SIZE);
    for (u32 entry = offset; entry < group + PTEG_SIZE; entry += PTE_SIZE)
    {
      if (LoadWordBE(current + entry) != wanted)
        continue;

      if (PTEsEqual(saved + entry, current + entry))
      {
        StoreWordBE(saved + entry + 4, LoadWordBE(saved + entry + 4) ^ PTE_RESERVED_BIT);
        if (in_secondary)
          *needs_secondary_pass = true;
      }
      // The first match outranks anything after it.
      return;
    }

    if (PTEIsSecondary(wanted))
      return;

    // Continue with the secondary PTEG, from its start.
    wanted |= PTE_SECONDARY;
    offset = ((~group & m_ppc_state.pagetable_mask) | m_ppc_state.pagetable_base) -
             m_ppc_state.pagetable_base;
    in_secondary = true;
  }
}

void MMU::ForgetPTE(u32 pte_word0, u32 offset, std::span<const u8> page_table,
                    bool* needs_secondary_pass)
{
  const std::optional<u32> page = PageAddressFromPTE(pte_word0, offset / PTEG_SIZE);
  if (!page)
    return;

  const u32 priority = MappingPriority(offset, pte_word0);
  const u32 vsid = PTEVsid(pte_word0);
  for (u32 sr = 0; sr < 16; ++sr)
  {
    const UReg_SR segment{m_ppc_state.sr[sr]};
    if (segment.VSID != vsid || segment.T != 0)
      continue;

    const u32 address = *page | (sr << 28);
    const auto it = m_page_mappings.find(address);
    if (it == m_page_mappings.end() || it->second.priority != priority)
      continue;

    if (it->second.host_mapping)
      m_mappings_to_remove.insert(address);
    m_page_mappings.erase(it);

    RecheckShadowedPTE(pte_word0, offset, page_table, needs_secondary_pass);
  }
}

void MMU::LearnPTE(u32 pte_word0, u32 pte_word1, u32 offset)
{
  const std::optional<u32> page = PageAddressFromPTE(pte_word0, offset / PTEG_SIZE);
  if (!page)
    return;

  const UPTE_Hi pte{pte_word1};
  const u32 priority = MappingPriority(offset, pte_word0);
  const u32 vsid = PTEVsid(pte_word0);

  for (u32 sr = 0; sr < 16; ++sr)
  {
    const UReg_SR segment{m_ppc_state.sr[sr]};
    if (segment.VSID != vsid || segment.T != 0)
      continue;

    const u32 address = *page | (sr << 28);

    // Pages fastmem must not touch: uncached ones (their quirks are only
    // emulated on the slow path, though no software is known to use them
    // through page translation), pages a BAT shadows (BATs win), and pages
    // with a watchpoint.
    const bool host_mapping =
        !IsWriteThroughOrInhibited(pte.WIMG) &&
        (m_dbat_table[address >> BAT_INDEX_SHIFT] & BAT_MAPPED_BIT) == 0 &&
        !m_power_pc.GetMemChecks().OverlapsMemcheck(address, HW_PAGE_SIZE);

    const PageMapping mapping{pte.RPN, priority, host_mapping};
    auto [it, inserted] = m_page_mappings.try_emplace(address, mapping);
    if (!inserted)
    {
      if (it->second.priority < priority)
        continue;
      if (it->second.host_mapping)
        m_mappings_to_remove.insert(address);
      it->second = mapping;
    }

    // Until the guest touches the page (R set), the host mapping is left for
    // TranslateThroughPageTable to create. Without C, it is read-only so the
    // first store comes back through the slow path to set C.
    if (host_mapping && pte.R)
    {
      auto& batch = pte.C ? m_readwrite_mappings_to_add : m_readonly_mappings_to_add;
      batch.emplace(address, pte.RPN << 12);
    }
  }
}

void MMU::ApplyPageTable(std::span<const u8> page_table)
{
  if (page_table.size() % PAGE_TABLE_MIN_SIZE != 0)
  {
    // Only possible with a corrupt savestate.
    PanicAlertFmt("Impossible page table size {}", page_table.size());
    ClearPageTable();
    return;
  }

  m_mappings_to_remove.clear();
  m_readonly_mappings_to_add.clear();
  m_readwrite_mappings_to_add.clear();

  if (m_page_table.size() != page_table.size())
  {
    // Compare against an empty table of the new size.
    m_memory.RemoveAllPageTableMappings();
    m_page_mappings.clear();
    m_page_table.assign(page_table.size(), 0);
  }

  u8* const saved = m_page_table.data();
  const u8* const current = page_table.data();
  const u32 mask = m_ppc_state.pagetable_mask;
  const u32 base = m_ppc_state.pagetable_base;

  // Calls visit(offset) for every PTE that differs from the saved copy. The
  // comparison is made as the scan reaches each PTE, so changes the visitor
  // makes to the saved copy further on are seen.
  const auto for_each_changed_pte = [&](auto&& visit) {
    for (u32 chunk = 0; chunk < page_table.size(); chunk += PAGE_TABLE_MIN_SIZE)
    {
      // With a misaligned HTABORG, chunks whose offset overlaps it are not
      // part of the hashed table.
      if ((chunk & mask) != chunk || (chunk & base) != 0)
        continue;

      for (u32 group = chunk; group < chunk + PAGE_TABLE_MIN_SIZE; group += PTEG_SIZE)
      {
        if (PTEGroupsEqual(saved + group, current + group))
          continue;
        for (u32 entry = group; entry < group + PTEG_SIZE; entry += PTE_SIZE)
        {
          if (!PTEsEqual(saved + entry, current + entry))
            visit(entry);
        }
      }
    }
  };

  bool needs_secondary_pass = false;

  // Pass 1: withdraw what changed PTEs used to map, and learn new primary
  // PTEs. Secondary PTEs are left marked as changed for pass 2.
  for_each_changed_pte([&](u32 entry) {
    const u32 old_word0 = LoadWordBE(saved + entry);
    if ((old_word0 & PTE_VALID) != 0)
      ForgetPTE(old_word0, entry, page_table, &needs_secondary_pass);

    const u32 new_word0 = LoadWordBE(current + entry);
    if ((new_word0 & PTE_VALID) != 0)
    {
      if (PTEIsSecondary(new_word0))
      {
        needs_secondary_pass = true;
        return;
      }
      LearnPTE(new_word0, LoadWordBE(current + entry + 4), entry);
    }
    std::memcpy(saved + entry, current + entry, PTE_SIZE);
  });

  // Pass 2: everything still differing is a valid secondary PTE.
  if (needs_secondary_pass)
  {
    for_each_changed_pte([&](u32 entry) {
      LearnPTE(LoadWordBE(current + entry), LoadWordBE(current + entry + 4), entry);
      std::memcpy(saved + entry, current + entry, PTE_SIZE);
    });
  }

  if (!m_mappings_to_remove.empty())
    m_memory.RemovePageTableMappings(m_mappings_to_remove);
  for (const auto& [logical, physical] : m_readonly_mappings_to_add)
    m_memory.AddPageTableMapping(logical, physical, false);
  for (const auto& [logical, physical] : m_readwrite_mappings_to_add)
    m_memory.AddPageTableMapping(logical, physical, true);
}

void MMU::UpdateHostMappingForRC(u32 page_address, u32 pte_address, u32 pte_word0, u32 pte_word1)
{
  // The guest just got R (and maybe C) set in a PTE. If that PTE is the one
  // a host mapping is meant for, create the mapping now (writable once C is
  // set) and record the new PTE so the next update does not see a change.
  const auto it = m_page_mappings.find(page_address);
  if (it == m_page_mappings.end())
    return;

  const UPTE_Hi pte{pte_word1};
  const u32 priority = MappingPriority(pte_address, pte_word0);
  if (it->second != PageMapping{pte.RPN, priority, true})
    return;

  const u32 offset = pte_address - m_ppc_state.pagetable_base;
  if (offset + PTE_SIZE > m_page_table.size())
    return;
  StoreWordBE(m_page_table.data() + offset, pte_word0);
  StoreWordBE(m_page_table.data() + offset + 4, pte_word1);

  m_memory.AddPageTableMapping(page_address, pte.RPN << 12, pte.C);
}

// ---------------------------------------------------------------------------
// Entry points for JIT code
// ---------------------------------------------------------------------------

void ClearDCacheLineFromJit(MMU& mmu, u32 address)
{
  mmu.ClearDCacheLine(address);
}

template <std::unsigned_integral T>
Common::MakeAtLeastU32<T> ReadFromJit(MMU& mmu, u32 address)
{
  return mmu.Read<T>(address);
}

template <std::unsigned_integral T>
void WriteFromJit(MMU& mmu, Common::MakeAtLeastU32<T> var, u32 address)
{
  mmu.Write<T>(var, address);
}

// The JIT emits calls for every access width.
template u32 ReadFromJit<u8>(MMU&, u32);
template void WriteFromJit<u8>(MMU&, u32, u32);
template u32 ReadFromJit<u16>(MMU&, u32);
template void WriteFromJit<u16>(MMU&, u32, u32);
template u32 ReadFromJit<u32>(MMU&, u32);
template void WriteFromJit<u32>(MMU&, u32, u32);
template u64 ReadFromJit<u64>(MMU&, u32);
template void WriteFromJit<u64>(MMU&, u64, u32);

void WriteU16SwapFromJit(MMU& mmu, u32 var, u32 address)
{
  mmu.Write_U16_Swap(var, address);
}

void WriteU32SwapFromJit(MMU& mmu, u32 var, u32 address)
{
  mmu.Write_U32_Swap(var, address);
}

void WriteU64SwapFromJit(MMU& mmu, u64 var, u32 address)
{
  mmu.Write_U64_Swap(var, address);
}
}  // namespace PowerPC
