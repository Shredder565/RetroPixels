// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The CPU's view of memory: address translation (block address translation
// through the BAT registers, page address translation through the segment
// registers, the hashed page table and a software TLB), the physical memory
// map (RAM, MEM2, locked L1, EFB, MMIO, the write-gather pipe), and the
// bookkeeping that keeps the fastmem arena's page-table mappings in step
// with the guest's page table.

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"
#include "Common/TypeUtils.h"

class PointerWrap;

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core
namespace Memory
{
class MemoryManager;
}

namespace PowerPC
{
class PowerPCManager;
struct PowerPCState;

enum class RequestedAddressSpace
{
  Effective,  // translate if the MMU is currently on
  Physical,   // never translate
  Virtual,    // always translate; fails if the MMU is off
};

template <typename T>
struct ReadResult
{
  // Whether the address was translated (treated as virtual) or used as a
  // physical address.
  bool translated;

  T value;

  ReadResult(bool translated_, T&& value_) : translated(translated_), value(std::forward<T>(value_))
  {
  }
  ReadResult(bool translated_, const T& value_) : translated(translated_), value(value_) {}
};

struct WriteResult
{
  // Whether the address was translated (treated as virtual) or used as a
  // physical address.
  bool translated;

  explicit WriteResult(bool translated_) : translated(translated_) {}
};

// The BAT lookup tables have one entry per 128 KiB block of the effective
// address space: the translated block address in the upper bits plus the
// flag bits below. The JITs read these tables directly.
constexpr int BAT_INDEX_SHIFT = 17;
constexpr u32 BAT_PAGE_SIZE = 1 << BAT_INDEX_SHIFT;
constexpr u32 BAT_PAGE_COUNT = 1 << (32 - BAT_INDEX_SHIFT);
constexpr u32 BAT_MAPPED_BIT = 0x1;    // a BAT covers this block
constexpr u32 BAT_PHYSICAL_BIT = 0x2;  // ... and fastmem may access it directly
constexpr u32 BAT_WI_BIT = 0x4;        // ... and it is write-through or cache-inhibited
constexpr u32 BAT_RESULT_MASK = UINT32_C(~0x7);
using BatTable = std::array<u32, BAT_PAGE_COUNT>;  // 128 KiB

constexpr size_t HW_PAGE_SIZE = 4096;
constexpr size_t HW_PAGE_MASK = HW_PAGE_SIZE - 1;
constexpr u32 HW_PAGE_INDEX_SHIFT = 12;
constexpr u32 HW_PAGE_INDEX_MASK = 0x3f;

// The smallest hashed page table (HTABMASK = 0) is 64 KiB.
constexpr u32 PAGE_TABLE_MIN_SIZE = 0x10000;

// Result of MMU::TryReadInstruction().
struct TryReadInstResult
{
  bool valid;
  bool from_bat;
  u32 hex;
  u32 physical_address;
};

// Result of MMU::JitCache_TranslateAddress().
struct TranslateResult
{
  bool valid = false;
  bool translated = false;
  bool from_bat = false;
  u32 address = 0;

  TranslateResult() = default;
  explicit TranslateResult(u32 address_) : valid(true), address(address_) {}
  TranslateResult(bool from_bat_, u32 address_)
      : valid(true), translated(true), from_bat(from_bat_), address(address_)
  {
  }
};

// The kind of access a translation is for. The "NoException" kinds are for
// the debugger and other host-side code: they never raise exceptions and
// leave the TLB and the page table's R/C bits alone.
enum class XCheckTLBFlag
{
  NoException,
  Read,
  Write,
  Opcode,
  OpcodeNoException
};

class MMU
{
public:
  MMU(Core::System& system, Memory::MemoryManager& memory, PowerPC::PowerPCManager& power_pc);
  MMU(const MMU& other) = delete;
  MMU(MMU&& other) = delete;
  MMU& operator=(const MMU& other) = delete;
  MMU& operator=(MMU&& other) = delete;
  ~MMU();

  void Reset();
  void DoState(PointerWrap& p, bool sr_changed);

  // ---- Host access (debugger, cheats, HLE, ...) --------------------------
  // These go through the current translation settings like the CPU would,
  // but never raise guest exceptions. They are not for emulating guest
  // accesses.

  // Reads through the current translation. On failure a panic alert is shown
  // and 0 (or an empty string) is returned.
  template <std::unsigned_integral T>
  static T HostRead(const Core::CPUThreadGuard& guard, u32 address);
  template <typename T>
  requires(!std::unsigned_integral<T>)
  static T HostRead(const Core::CPUThreadGuard& guard, const u32 address)
  {
    using U = Common::MakeUnsignedSameSize<T>;
    U result = HostRead<U>(guard, address);
    return std::bit_cast<T>(result);
  }

  static u32 HostRead_Instruction(const Core::CPUThreadGuard& guard, u32 address);
  static std::string HostGetString(const Core::CPUThreadGuard& guard, u32 address, size_t size = 0);
  static std::u16string HostGetU16String(const Core::CPUThreadGuard& guard, u32 address,
                                         size_t size = 0);

  // Reads from the requested address space without any alert; returns
  // nothing if the address is not RAM there.
  template <std::unsigned_integral T>
  static std::optional<ReadResult<T>>
  HostTryRead(const Core::CPUThreadGuard& guard, u32 address,
              RequestedAddressSpace space = RequestedAddressSpace::Effective);
  template <typename T>
  static std::optional<ReadResult<T>>
  HostTryRead(const Core::CPUThreadGuard& guard, u32 address,
              RequestedAddressSpace space = RequestedAddressSpace::Effective)
      requires(!std::unsigned_integral<T>)
  {
    using U = Common::MakeUnsignedSameSize<T>;
    std::optional<ReadResult<U>> result = HostTryRead<U>(guard, address, space);
    return std::bit_cast<std::optional<ReadResult<T>>>(result);
  }
  static std::optional<ReadResult<u32>>
  HostTryReadInstruction(const Core::CPUThreadGuard& guard, u32 address,
                         RequestedAddressSpace space = RequestedAddressSpace::Effective);
  static std::optional<ReadResult<std::string>>
  HostTryReadString(const Core::CPUThreadGuard& guard, u32 address, size_t size = 0,
                    RequestedAddressSpace space = RequestedAddressSpace::Effective);

  // Writes through the current translation; a failure shows a panic alert.
  template <std::unsigned_integral T>
  static void HostWrite(const Core::CPUThreadGuard& guard, Common::MakeAtLeastU32<T> var,
                        u32 address);
  template <typename T>
  static void HostWrite(const Core::CPUThreadGuard& guard, T var, u32 address)
      requires(!std::unsigned_integral<T>)
  {
    using U = Common::MakeUnsignedSameSize<T>;
    U cast_var = std::bit_cast<U>(var);
    return HostWrite<U>(guard, cast_var, address);
  }

  // Writes to the requested address space without any alert; returns
  // nothing if the address is not RAM there.
  template <std::unsigned_integral T>
  static std::optional<WriteResult>
  HostTryWrite(const Core::CPUThreadGuard& guard, Common::MakeAtLeastU32<T> var, u32 address,
               RequestedAddressSpace space = RequestedAddressSpace::Effective);
  template <typename T>
  static std::optional<WriteResult>
  HostTryWrite(const Core::CPUThreadGuard& guard, const T var, const u32 address,
               RequestedAddressSpace space = RequestedAddressSpace::Effective)
      requires(!std::unsigned_integral<T>)
  {
    using U = Common::MakeUnsignedSameSize<T>;
    U cast_var = std::bit_cast<U>(var);
    return HostTryWrite<U>(guard, cast_var, address, space);
  }

  // Whether an access at the address ends up in RAM (MEM1, MEM2, fake VMEM
  // or the locked L1), using the data / instruction translation.
  static bool HostIsRAMAddress(const Core::CPUThreadGuard& guard, u32 address,
                               RequestedAddressSpace space = RequestedAddressSpace::Effective);
  static bool
  HostIsInstructionRAMAddress(const Core::CPUThreadGuard& guard, u32 address,
                              RequestedAddressSpace space = RequestedAddressSpace::Effective);

  // ---- Guest access (interpreter, JIT slow paths) ------------------------

  // Instruction fetch through the instruction cache; raises an ISI on a
  // translation miss.
  u32 Read_Opcode(u32 address);
  TryReadInstResult TryReadInstruction(u32 address);

  template <std::unsigned_integral T>
  T Read(u32 address);

  template <std::unsigned_integral T>
  void Write(Common::MakeAtLeastU32<T> var, u32 address);

  void Write_U16_Swap(u32 var, u32 address);
  void Write_U32_Swap(u32 var, u32 address);
  void Write_U64_Swap(u64 var, u32 address);

  // Locked-cache DMA, in 32-byte blocks.
  void DMA_LCToMemory(u32 mem_address, u32 cache_address, u32 num_blocks);
  void DMA_MemoryToLC(u32 cache_address, u32 mem_address, u32 num_blocks);

  // Data cache block instructions.
  void ClearDCacheLine(u32 address);  // dcbz: zeroes 32 bytes at a 32-byte aligned address
  void StoreDCacheLine(u32 address);
  void InvalidateDCacheLine(u32 address);
  void FlushDCacheLine(u32 address);
  void TouchDCacheLine(u32 address, bool store);

  // Translation state changes.
  void SDRUpdated();
  void SRUpdated();
  void InvalidateTLBEntry(u32 address);
  void PageTableUpdated();
  static void PageTableUpdatedFromJit(MMU* mmu);
  void DBATUpdated();
  void IBATUpdated();

  // Whether the JIT may turn an access into a plain fastmem access: this
  // depends on the BATs and MSR[DR], and never on the page table.
  bool IsOptimizableRAMAddress(u32 address, u32 access_size) const;
  u32 IsOptimizableMMIOAccess(u32 address, u32 access_size) const;
  bool IsOptimizableGatherPipeWrite(u32 address) const;

  TranslateResult JitCache_TranslateAddress(u32 address);

  std::optional<u32> GetTranslatedAddress(u32 address);

  BatTable& GetIBATTable() { return m_ibat_table; }
  BatTable& GetDBATTable() { return m_dbat_table; }

private:
  enum class Translation : u8
  {
    Block,
    Page,
    DirectStore,
    Fault,
  };

  struct Translated
  {
    Translation kind;
    u32 address;
    // Write-through or cache-inhibited.
    bool wi;

    bool Succeeded() const { return kind == Translation::Block || kind == Translation::Page; }
  };

  // What the fastmem bookkeeping knows about one virtual page that some
  // valid PTE maps.
  struct PageMapping
  {
    u32 rpn = 0;
    // 0-7: slot in a primary PTEG, 8-15: slot in a secondary PTEG. Lower
    // wins, as the hardware's search order would pick it first.
    u32 priority = 0;
    // Whether the page may be (or is) mapped into the fastmem arena.
    bool host_mapping = false;

    bool operator==(const PageMapping&) const = default;
  };

  template <XCheckTLBFlag flag>
  Translated Translate(u32 address);
  template <XCheckTLBFlag flag>
  Translated TranslateThroughPageTable(u32 address);

  template <XCheckTLBFlag flag, std::unsigned_integral T, bool never_translate = false>
  T ReadFromHardware(u32 em_address);
  template <XCheckTLBFlag flag, bool never_translate = false>
  void WriteToHardware(u32 em_address, u32 data, u32 size);

  template <XCheckTLBFlag flag>
  bool IsEffectiveRAMAddress(u32 address);
  bool IsPhysicalRAMAddress(u32 address) const;

  template <XCheckTLBFlag flag>
  bool PrepareDCacheOp(u32* address, bool fault_raises_dsi);

  void GenerateDSIException(u32 effective_address, bool write);
  void GenerateISIException(u32 effective_address);
  void UnresolvedAccess(u32 address, bool write);

  void Memcheck(u32 address, u64 var, bool write, size_t size);

  // Page-table mirroring for fastmem.
  void ClearPageTable();
  void ReloadPageTable();
  void ApplyPageTable(std::span<const u8> page_table);
  std::optional<u32> PageAddressFromPTE(u32 pte_word0, u32 pteg_index) const;
  void ForgetPTE(u32 pte_word0, u32 offset, std::span<const u8> page_table,
                 bool* needs_secondary_pass);
  void RecheckShadowedPTE(u32 pte_word0, u32 offset, std::span<const u8> page_table,
                          bool* needs_secondary_pass);
  void LearnPTE(u32 pte_word0, u32 pte_word1, u32 offset);
  void UpdateHostMappingForRC(u32 page_address, u32 pteg_address, u32 pte_word0, u32 pte_word1);

  void UpdateBATs(BatTable& bat_table, u32 base_spr);
  void UpdateFakeMMUBat(BatTable& bat_table, u32 start_addr);
  void RebuildBATTable(BatTable& bat_table, u32 base_spr, u32 extended_base_spr);

  Core::System& m_system;
  Memory::MemoryManager& m_memory;
  PowerPC::PowerPCManager& m_power_pc;
  PowerPC::PowerPCState& m_ppc_state;

  // The guest page table as it was last processed (saved in savestates).
  std::vector<u8> m_page_table;

  // Every virtual page that a valid PTE of m_page_table maps, by page
  // address.
  std::map<u32, PageMapping> m_page_mappings;

  // Scratch space reused between updates.
  std::vector<u8> m_scratch_page_table;
  std::set<u32> m_mappings_to_remove;
  std::map<u32, u32> m_readonly_mappings_to_add;
  std::map<u32, u32> m_readwrite_mappings_to_add;

  BatTable m_ibat_table;
  BatTable m_dbat_table;
};

void ClearDCacheLineFromJit(MMU& mmu, u32 address);
// Returns the value zero-extended.
template <std::unsigned_integral T>
Common::MakeAtLeastU32<T> ReadFromJit(MMU& mmu, u32 address);
template <std::unsigned_integral T>
void WriteFromJit(MMU& mmu, Common::MakeAtLeastU32<T> var, u32 address);
void WriteU16SwapFromJit(MMU& mmu, u32 var, u32 address);
void WriteU32SwapFromJit(MMU& mmu, u32 var, u32 address);
void WriteU64SwapFromJit(MMU& mmu, u64 var, u32 address);
}  // namespace PowerPC
