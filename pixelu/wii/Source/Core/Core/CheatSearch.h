// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/MMU.h"

namespace Core
{
class CPUThreadGuard;
}

// The memory scanner behind the cheat search window: find addresses whose value matches, then
// narrow them down over later searches.
namespace Cheats
{
enum class CompareType
{
  Equal,
  NotEqual,
  Less,
  LessOrEqual,
  Greater,
  GreaterOrEqual,
};

enum class FilterType
{
  CompareAgainstSpecificValue,
  CompareAgainstLastValue,
  DoNotFilter,
};

// Same order as the alternatives of SearchValue::m_value.
enum class DataType
{
  U8,
  U16,
  U32,
  U64,
  S8,
  S16,
  S32,
  S64,
  F32,
  F64,
};

struct SearchValue
{
  std::variant<u8, u16, u32, u64, s8, s16, s32, s64, float, double> m_value;
};

enum class SearchResultValueState : u8
{
  ValueFromPhysicalMemory,
  ValueFromVirtualMemory,
  AddressNotAccessible,
};

template <typename T>
struct SearchResult
{
  T m_value;
  SearchResultValueState m_value_state;
  u32 m_address;

  // A value was read (from either address space).
  bool IsValueValid() const
  {
    switch (m_value_state)
    {
    case SearchResultValueState::ValueFromPhysicalMemory:
    case SearchResultValueState::ValueFromVirtualMemory:
      return true;
    default:
      return false;
    }
  }
};

struct MemoryRange
{
  u32 m_start;
  u64 m_length;

  MemoryRange(u32 start, u64 length) : m_start(start), m_length(length) {}
};

enum class SearchErrorCode
{
  Success,

  // Emulation isn't running (or paused).
  NoEmulationActive,

  // The search can't run with the session's current settings (for example no value to compare
  // with).
  InvalidParameters,

  // A virtual address search was asked for while the game has data translation (MSR.DR) off.
  VirtualAddressesCurrentlyNotAccessible,

  // RetroAchievements hardcore mode forbids reading memory for cheats.
  DisabledInHardcoreMode,
};

DataType GetDataType(const SearchValue& value);

// The value's bytes in big-endian (emulated memory) order.
std::vector<u8> GetValueAsByteVector(const SearchValue& value);

// Reads every T in `memory_ranges` (every byte, or only T-aligned addresses) and keeps those for
// which `validator` returns true.
template <typename T>
std::expected<std::vector<SearchResult<T>>, SearchErrorCode>
NewSearch(const Core::CPUThreadGuard& guard, std::span<const MemoryRange> memory_ranges,
          PowerPC::RequestedAddressSpace address_space, bool aligned,
          const std::function<bool(const T& value)>& validator);

// Reads the addresses of `previous_results` again and keeps those for which `validator` (new value,
// previous value) returns true. Addresses that can't be read are kept, marked inaccessible.
template <typename T>
std::expected<std::vector<SearchResult<T>>, SearchErrorCode>
NextSearch(const Core::CPUThreadGuard& guard, std::span<const SearchResult<T>> previous_results,
           PowerPC::RequestedAddressSpace address_space,
           const std::function<bool(const T& new_value, const T& old_value)>& validator);

class CheatSearchSessionBase
{
public:
  virtual ~CheatSearchSessionBase();

  // How values are compared in later searches.
  virtual void SetCompareType(CompareType compare_type) = 0;
  // What values are compared with in later searches.
  virtual void SetFilterType(FilterType filter_type) = 0;
  // The value for FilterType::CompareAgainstSpecificValue. Returns false if it doesn't parse.
  virtual bool SetValueFromString(const std::string& value_as_string, bool force_parse_as_hex) = 0;

  // Drops the results; the next search scans the memory ranges again.
  virtual void ResetResults() = 0;

  // A first search, or a search through the previous results.
  virtual SearchErrorCode RunSearch(const Core::CPUThreadGuard& guard) = 0;

  virtual size_t GetMemoryRangeCount() const = 0;
  virtual MemoryRange GetMemoryRange(size_t index) const = 0;
  virtual PowerPC::RequestedAddressSpace GetAddressSpace() const = 0;
  virtual DataType GetDataType() const = 0;
  virtual bool GetAligned() const = 0;

  virtual bool IsIntegerType() const = 0;
  virtual bool IsFloatingType() const = 0;

  virtual size_t GetResultCount() const = 0;
  virtual size_t GetValidValueCount() const = 0;
  virtual u32 GetResultAddress(size_t index) const = 0;
  virtual SearchValue GetResultValueAsSearchValue(size_t index) const = 0;
  virtual std::string GetResultValueAsString(size_t index, bool hex) const = 0;
  virtual SearchResultValueState GetResultValueState(size_t index) const = 0;
  virtual bool WasFirstSearchDone() const = 0;

  // Writes the session's value to each address. False if any write failed.
  virtual bool WriteValue(const Core::CPUThreadGuard& guard, std::span<u32> addresses) const = 0;

  virtual void RemoveResult(size_t index) = 0;

  virtual std::unique_ptr<CheatSearchSessionBase> Clone() const = 0;
  // A copy holding only the results [begin_index, end_index), so a search can continue on part of
  // the results.
  virtual std::unique_ptr<CheatSearchSessionBase> ClonePartial(size_t begin_index,
                                                               size_t end_index) const = 0;
};

template <typename T>
class CheatSearchSession final : public CheatSearchSessionBase
{
public:
  CheatSearchSession(std::vector<MemoryRange> memory_ranges,
                     PowerPC::RequestedAddressSpace address_space, bool aligned);
  CheatSearchSession(const CheatSearchSession& session);
  CheatSearchSession(CheatSearchSession&& session);
  CheatSearchSession& operator=(const CheatSearchSession& session);
  CheatSearchSession& operator=(CheatSearchSession&& session);
  ~CheatSearchSession() override;

  void SetCompareType(CompareType compare_type) override;
  void SetFilterType(FilterType filter_type) override;
  bool SetValueFromString(const std::string& value_as_string, bool force_parse_as_hex) override;

  void ResetResults() override;
  void RemoveResult(size_t index) override;
  SearchErrorCode RunSearch(const Core::CPUThreadGuard& guard) override;

  size_t GetMemoryRangeCount() const override;
  MemoryRange GetMemoryRange(size_t index) const override;
  PowerPC::RequestedAddressSpace GetAddressSpace() const override;
  DataType GetDataType() const override;
  bool GetAligned() const override;

  bool IsIntegerType() const override;
  bool IsFloatingType() const override;

  size_t GetResultCount() const override;
  size_t GetValidValueCount() const override;
  u32 GetResultAddress(size_t index) const override;
  T GetResultValue(size_t index) const;
  SearchValue GetResultValueAsSearchValue(size_t index) const override;
  std::string GetResultValueAsString(size_t index, bool hex) const override;
  SearchResultValueState GetResultValueState(size_t index) const override;
  bool WasFirstSearchDone() const override;

  bool WriteValue(const Core::CPUThreadGuard& guard, std::span<u32> addresses) const override;

  std::unique_ptr<CheatSearchSessionBase> Clone() const override;
  std::unique_ptr<CheatSearchSessionBase> ClonePartial(size_t begin_index,
                                                       size_t end_index) const override;

private:
  std::vector<SearchResult<T>> m_results;
  std::vector<MemoryRange> m_ranges;
  PowerPC::RequestedAddressSpace m_space;
  CompareType m_compare = CompareType::Equal;
  FilterType m_filter = FilterType::DoNotFilter;
  std::optional<T> m_target;
  bool m_aligned;
  bool m_has_results = false;
};

std::unique_ptr<CheatSearchSessionBase> MakeSession(std::vector<MemoryRange> memory_ranges,
                                                    PowerPC::RequestedAddressSpace address_space,
                                                    bool aligned, DataType data_type);
}  // namespace Cheats
