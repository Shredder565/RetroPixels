// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <fmt/format.h>
#include <string>
#include <winerror.h>
#include <winrt/base.h>

#include "Common/CommonTypes.h"

namespace Common
{
// The system's text for an HRESULT.
std::string GetHResultMessage(HRESULT hr);

// Formats an HRESULT through fmt as "<message> (0x########)". HRESULT is only a typedef of long,
// so it needs this distinct wrapper type to get its own formatter.
struct HRWrap
{
  constexpr explicit HRWrap(HRESULT hr) : m_hr(hr) {}
  const HRESULT m_hr;
};
}  // namespace Common

template <>
struct fmt::formatter<Common::HRWrap>
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const Common::HRWrap& wrapped, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "{} ({:#010x})", Common::GetHResultMessage(wrapped.m_hr),
                          static_cast<u32>(wrapped.m_hr));
  }
};

// Same format for C++/WinRT's hresult.
template <>
struct fmt::formatter<winrt::hresult>
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const winrt::hresult& result, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "{} ({:#010x})", Common::GetHResultMessage(result.value),
                          static_cast<u32>(result.value));
  }
};
