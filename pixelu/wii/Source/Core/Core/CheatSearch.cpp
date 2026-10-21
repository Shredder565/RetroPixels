// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/CheatSearch.h"

#include <bit>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#include "Core/AchievementManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Cheats
{
namespace
{
template <DataType type, typename Expected>
constexpr bool kAlternativeIs =
    std::is_same_v<std::variant_alternative_t<static_cast<std::size_t>(type),
                                              decltype(SearchValue::m_value)>,
                   Expected>;
static_assert(kAlternativeIs<DataType::U8, u8> && kAlternativeIs<DataType::U16, u16> &&
              kAlternativeIs<DataType::U32, u32> && kAlternativeIs<DataType::U64, u64> &&
              kAlternativeIs<DataType::S8, s8> && kAlternativeIs<DataType::S16, s16> &&
              kAlternativeIs<DataType::S32, s32> && kAlternativeIs<DataType::S64, s64> &&
              kAlternativeIs<DataType::F32, float> && kAlternativeIs<DataType::F64, double>,
              "DataType must follow the order of SearchValue's alternatives");

// The unsigned integer with T's size.
template <typename T>
using Bits = std::conditional_t<
    sizeof(T) == 1, u8,
    std::conditional_t<sizeof(T) == 2, u16, std::conditional_t<sizeof(T) == 4, u32, u64>>>;

template <typename T>
std::vector<u8> BigEndianBytes(T value)
{
  const Bits<T> big_endian = Common::FromBigEndian(std::bit_cast<Bits<T>>(value));
  std::vector<u8> bytes(sizeof(T));
  std::memcpy(bytes.data(), &big_endian, sizeof(T));
  return bytes;
}

// Hardcore mode, emulation state and address translation are checked before every search.
template <typename T>
std::optional<SearchErrorCode> CheckCanSearch(const Core::CPUThreadGuard& guard,
                                              PowerPC::RequestedAddressSpace space)
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    return SearchErrorCode::DisabledInHardcoreMode;

  Core::System& system = guard.GetSystem();
  const Core::State state = Core::GetState(system);
  if (state != Core::State::Running && state != Core::State::Paused)
    return SearchErrorCode::NoEmulationActive;

  if (space == PowerPC::RequestedAddressSpace::Virtual && !system.GetPPCState().msr.DR)
    return SearchErrorCode::VirtualAddressesCurrentlyNotAccessible;

  return std::nullopt;
}

template <typename T>
SearchResult<T> ReadResult(u32 address, const PowerPC::ReadResult<T>& read)
{
  SearchResult<T> result{};
  result.m_value = read.value;
  result.m_value_state = read.translated ? SearchResultValueState::ValueFromVirtualMemory :
                                           SearchResultValueState::ValueFromPhysicalMemory;
  result.m_address = address;
  return result;
}

template <typename T>
bool Compare(CompareType compare, const T& lhs, const T& rhs)
{
  switch (compare)
  {
  case CompareType::Equal:
    return lhs == rhs;
  case CompareType::NotEqual:
    return lhs != rhs;
  case CompareType::Less:
    return lhs < rhs;
  case CompareType::LessOrEqual:
    return lhs <= rhs;
  case CompareType::Greater:
    return lhs > rhs;
  case CompareType::GreaterOrEqual:
    return lhs >= rhs;
  }
  DEBUG_ASSERT(false);
  return false;
}

template <typename T>
std::optional<T> ParseSearchValue(const std::string& text, bool hex)
{
  if (text.empty())
    return std::nullopt;

  T parsed{};
  bool ok = false;
  if constexpr (std::is_integral_v<T>)
    ok = TryParse(text, &parsed, hex ? 16 : 0);
  else
    ok = TryParse(text, &parsed);
  if (!ok)
    return std::nullopt;
  return parsed;
}
}  // namespace

DataType GetDataType(const SearchValue& value)
{
  return static_cast<DataType>(value.m_value.index());
}

std::vector<u8> GetValueAsByteVector(const SearchValue& value)
{
  return std::visit([](auto held) { return BigEndianBytes(held); }, value.m_value);
}

template <typename T>
auto NewSearch(const Core::CPUThreadGuard& guard, std::span<const MemoryRange> memory_ranges,
               PowerPC::RequestedAddressSpace address_space, bool aligned,
               const std::function<bool(const T& value)>& validator)
    -> std::expected<std::vector<SearchResult<T>>, SearchErrorCode>
{
  if (const std::optional<SearchErrorCode> error = CheckCanSearch<T>(guard, address_space))
    return std::unexpected{*error};

  constexpr u32 size = sizeof(T);
  const u32 step = aligned ? size : 1;

  std::vector<SearchResult<T>> found;
  for (const MemoryRange& range : memory_ranges)
  {
    if (range.m_length < size)
      continue;

    // Skipping to the first aligned address shortens the range (addresses wrap at 4 GiB).
    const u32 first = aligned ? Common::AlignUp(range.m_start, size) : range.m_start;
    const u32 skipped = first - range.m_start;
    if (range.m_length - skipped < size)
      continue;

    // Offsets at which a whole T still fits in the range.
    const u64 end_offset = range.m_length - skipped - (size - 1);
    for (u64 offset = 0; offset < end_offset; offset += step)
    {
      const u32 address = static_cast<u32>(first + offset);
      const auto read = PowerPC::MMU::HostTryRead<T>(guard, address, address_space);
      if (read && validator(read->value))
        found.push_back(ReadResult<T>(address, *read));
    }
  }
  return found;
}

template <typename T>
auto NextSearch(const Core::CPUThreadGuard& guard,
                std::span<const SearchResult<T>> previous_results,
                PowerPC::RequestedAddressSpace address_space,
                const std::function<bool(const T& new_value, const T& old_value)>& validator)
    -> std::expected<std::vector<SearchResult<T>>, SearchErrorCode>
{
  if (const std::optional<SearchErrorCode> error = CheckCanSearch<T>(guard, address_space))
    return std::unexpected{*error};

  std::vector<SearchResult<T>> kept;
  for (const SearchResult<T>& previous : previous_results)
  {
    const auto read = PowerPC::MMU::HostTryRead<T>(guard, previous.m_address, address_space);
    if (!read)
    {
      SearchResult<T> unreadable{};
      unreadable.m_value_state = SearchResultValueState::AddressNotAccessible;
      unreadable.m_address = previous.m_address;
      kept.push_back(unreadable);
      continue;
    }

    // A result without a previous value always takes the new one, so it can't stay invalid.
    if (!previous.IsValueValid() || validator(read->value, previous.m_value))
      kept.push_back(ReadResult<T>(previous.m_address, *read));
  }
  return kept;
}

CheatSearchSessionBase::~CheatSearchSessionBase() = default;

template <typename T>
CheatSearchSession<T>::CheatSearchSession(std::vector<MemoryRange> memory_ranges,
                                          PowerPC::RequestedAddressSpace address_space,
                                          bool aligned)
    : m_ranges(std::move(memory_ranges)), m_space(address_space), m_aligned(aligned)
{
}

template <typename T>
CheatSearchSession<T>::CheatSearchSession(const CheatSearchSession&) = default;
template <typename T>
CheatSearchSession<T>::CheatSearchSession(CheatSearchSession&&) = default;
template <typename T>
CheatSearchSession<T>& CheatSearchSession<T>::operator=(const CheatSearchSession&) = default;
template <typename T>
CheatSearchSession<T>& CheatSearchSession<T>::operator=(CheatSearchSession&&) = default;
template <typename T>
CheatSearchSession<T>::~CheatSearchSession() = default;

template <typename T>
void CheatSearchSession<T>::SetCompareType(CompareType compare_type)
{
  m_compare = compare_type;
}

template <typename T>
void CheatSearchSession<T>::SetFilterType(FilterType filter_type)
{
  m_filter = filter_type;
}

template <typename T>
bool CheatSearchSession<T>::SetValueFromString(const std::string& value_as_string,
                                               bool force_parse_as_hex)
{
  m_target = ParseSearchValue<T>(value_as_string, force_parse_as_hex);
  return m_target.has_value();
}

template <typename T>
void CheatSearchSession<T>::ResetResults()
{
  m_has_results = false;
  m_results.clear();
}

template <typename T>
void CheatSearchSession<T>::RemoveResult(size_t index)
{
  if (index >= m_results.size())
    return;
  m_results.erase(m_results.begin() + static_cast<std::ptrdiff_t>(index));
}

template <typename T>
SearchErrorCode CheatSearchSession<T>::RunSearch(const Core::CPUThreadGuard& guard)
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    return SearchErrorCode::DisabledInHardcoreMode;

  const CompareType compare = m_compare;
  std::expected<std::vector<SearchResult<T>>, SearchErrorCode> outcome =
      std::unexpected{SearchErrorCode::InvalidParameters};

  switch (m_filter)
  {
  case FilterType::CompareAgainstSpecificValue:
  {
    if (!m_target)
      return SearchErrorCode::InvalidParameters;
    const T target = *m_target;
    if (m_has_results)
    {
      outcome = NextSearch<T>(guard, m_results, m_space, [&](const T& value, const T&) {
        return Compare(compare, value, target);
      });
    }
    else
    {
      outcome = NewSearch<T>(guard, m_ranges, m_space, m_aligned,
                             [&](const T& value) { return Compare(compare, value, target); });
    }
    break;
  }
  case FilterType::CompareAgainstLastValue:
    if (!m_has_results)
      return SearchErrorCode::InvalidParameters;
    outcome = NextSearch<T>(guard, m_results, m_space, [&](const T& value, const T& previous) {
      return Compare(compare, value, previous);
    });
    break;
  case FilterType::DoNotFilter:
    if (m_has_results)
      outcome = NextSearch<T>(guard, m_results, m_space, [](const T&, const T&) { return true; });
    else
      outcome = NewSearch<T>(guard, m_ranges, m_space, m_aligned, [](const T&) { return true; });
    break;
  }

  if (!outcome)
    return outcome.error();
  m_results = std::move(*outcome);
  m_has_results = true;
  return SearchErrorCode::Success;
}

template <typename T>
size_t CheatSearchSession<T>::GetMemoryRangeCount() const
{
  return m_ranges.size();
}

template <typename T>
MemoryRange CheatSearchSession<T>::GetMemoryRange(size_t index) const
{
  return m_ranges[index];
}

template <typename T>
PowerPC::RequestedAddressSpace CheatSearchSession<T>::GetAddressSpace() const
{
  return m_space;
}

template <typename T>
DataType CheatSearchSession<T>::GetDataType() const
{
  return Cheats::GetDataType(SearchValue{T{}});
}

template <typename T>
bool CheatSearchSession<T>::GetAligned() const
{
  return m_aligned;
}

template <typename T>
bool CheatSearchSession<T>::IsIntegerType() const
{
  return std::is_integral_v<T>;
}

template <typename T>
bool CheatSearchSession<T>::IsFloatingType() const
{
  return std::is_floating_point_v<T>;
}

template <typename T>
size_t CheatSearchSession<T>::GetResultCount() const
{
  return m_results.size();
}

template <typename T>
size_t CheatSearchSession<T>::GetValidValueCount() const
{
  size_t valid = 0;
  for (const SearchResult<T>& result : m_results)
    valid += result.IsValueValid() ? 1 : 0;
  return valid;
}

template <typename T>
u32 CheatSearchSession<T>::GetResultAddress(size_t index) const
{
  return m_results[index].m_address;
}

template <typename T>
T CheatSearchSession<T>::GetResultValue(size_t index) const
{
  return m_results[index].m_value;
}

template <typename T>
SearchValue CheatSearchSession<T>::GetResultValueAsSearchValue(size_t index) const
{
  return SearchValue{m_results[index].m_value};
}

template <typename T>
std::string CheatSearchSession<T>::GetResultValueAsString(size_t index, bool hex) const
{
  const SearchResult<T>& result = m_results[index];
  if (result.m_value_state == SearchResultValueState::AddressNotAccessible)
    return "(inaccessible)";

  if (!hex)
    return fmt::format("{}", result.m_value);

  if constexpr (std::is_floating_point_v<T>)
  {
    // Floating-point bits are printed as a signed number (so negative values show a '-').
    using SignedBits = std::make_signed_t<Bits<T>>;
    return fmt::format("0x{:0{}x}", std::bit_cast<SignedBits>(result.m_value), sizeof(T) * 2);
  }
  else
  {
    return fmt::format("0x{:0{}x}", std::bit_cast<std::make_unsigned_t<T>>(result.m_value),
                       sizeof(T) * 2);
  }
}

template <typename T>
SearchResultValueState CheatSearchSession<T>::GetResultValueState(size_t index) const
{
  return m_results[index].m_value_state;
}

template <typename T>
bool CheatSearchSession<T>::WasFirstSearchDone() const
{
  return m_has_results;
}

template <typename T>
bool CheatSearchSession<T>::WriteValue(const Core::CPUThreadGuard& guard,
                                       std::span<u32> addresses) const
{
  if (!m_target)
    return false;

  // Every address is tried, even after a failure.
  bool all_written = true;
  for (const u32 address : addresses)
  {
    if (!PowerPC::MMU::HostTryWrite<T>(guard, *m_target, address, m_space))
      all_written = false;
  }
  return all_written;
}

template <typename T>
std::unique_ptr<CheatSearchSessionBase> CheatSearchSession<T>::Clone() const
{
  return std::make_unique<CheatSearchSession<T>>(*this);
}

template <typename T>
std::unique_ptr<CheatSearchSessionBase> CheatSearchSession<T>::ClonePartial(size_t begin_index,
                                                                           size_t end_index) const
{
  if (begin_index == 0 && end_index >= m_results.size())
    return Clone();

  auto copy = std::make_unique<CheatSearchSession<T>>(m_ranges, m_space, m_aligned);
  copy->m_results.assign(m_results.begin() + static_cast<std::ptrdiff_t>(begin_index),
                         m_results.begin() + static_cast<std::ptrdiff_t>(end_index));
  copy->m_compare = m_compare;
  copy->m_filter = m_filter;
  copy->m_target = m_target;
  copy->m_has_results = m_has_results;
  return copy;
}

template class CheatSearchSession<u8>;
template class CheatSearchSession<u16>;
template class CheatSearchSession<u32>;
template class CheatSearchSession<u64>;
template class CheatSearchSession<s8>;
template class CheatSearchSession<s16>;
template class CheatSearchSession<s32>;
template class CheatSearchSession<s64>;
template class CheatSearchSession<float>;
template class CheatSearchSession<double>;

std::unique_ptr<CheatSearchSessionBase> MakeSession(std::vector<MemoryRange> memory_ranges,
                                                    PowerPC::RequestedAddressSpace address_space,
                                                    bool aligned, DataType data_type)
{
  const auto make = [&]<typename T>() -> std::unique_ptr<CheatSearchSessionBase> {
    return std::make_unique<CheatSearchSession<T>>(std::move(memory_ranges), address_space,
                                                   aligned);
  };

  switch (data_type)
  {
  case DataType::U8:
    return make.template operator()<u8>();
  case DataType::U16:
    return make.template operator()<u16>();
  case DataType::U32:
    return make.template operator()<u32>();
  case DataType::U64:
    return make.template operator()<u64>();
  case DataType::S8:
    return make.template operator()<s8>();
  case DataType::S16:
    return make.template operator()<s16>();
  case DataType::S32:
    return make.template operator()<s32>();
  case DataType::S64:
    return make.template operator()<s64>();
  case DataType::F32:
    return make.template operator()<float>();
  case DataType::F64:
    return make.template operator()<double>();
  }
  DEBUG_ASSERT(false);
  return nullptr;
}
}  // namespace Cheats
