// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HLE/HLE_OS.h"

#include <bit>
#include <cinttypes>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/printf.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/HLE/HLE_VarArgs.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE_OS
{
namespace
{
// Where the arguments that follow the format string come from.
enum class ArgSource
{
  Registers,  // printf-style: the arguments continue in the next registers
  VaList,     // vprintf-style: the next register points at a guest va_list
};

// Feeds GetStringVA from a guest argument walker.
class GuestPrintArgs final : public HLEPrintArgs
{
public:
  GuestPrintArgs(const Core::CPUThreadGuard& guard, HLE::SystemVABI::VAList& walker)
      : m_guard(guard), m_walker(walker)
  {
  }

  u32 GetU32() override { return m_walker.GetArgT<u32>(); }
  u64 GetU64() override { return m_walker.GetArgT<u64>(); }
  double GetF64() override { return m_walker.GetArgT<double>(); }

  // A precision of exactly zero prints nothing, so the pointer argument is not
  // even fetched. No precision means "read up to the terminator".
  std::string GetString(std::optional<u32> max_length) override
  {
    if (max_length == 0u)
      return {};
    const u32 address = m_walker.GetArgT<u32>();
    return PowerPC::MMU::HostGetString(m_guard, address, max_length.value_or(0u));
  }

  std::u16string GetU16String(std::optional<u32> max_length) override
  {
    if (max_length == 0u)
      return {};
    const u32 address = m_walker.GetArgT<u32>();
    return PowerPC::MMU::HostGetU16String(m_guard, address, max_length.value_or(0u));
  }

private:
  const Core::CPUThreadGuard& m_guard;
  HLE::SystemVABI::VAList& m_walker;
};

// Reads the format string held in `format_reg` and expands it using the
// arguments that follow it.
std::string ExpandGuestFormat(const Core::CPUThreadGuard& guard, u32 format_reg,
                              ArgSource source = ArgSource::Registers)
{
  const auto& ppc_state = guard.GetSystem().GetPPCState();
  const std::string format = PowerPC::MMU::HostGetString(guard, ppc_state.gpr[format_reg]);

  std::unique_ptr<HLE::SystemVABI::VAList> walker;
  if (source == ArgSource::VaList)
  {
    walker = std::make_unique<HLE::SystemVABI::VAListStruct>(guard, ppc_state.gpr[format_reg + 1]);
  }
  else
  {
    // Register arguments spill into the parameter area 8 bytes above SP.
    walker =
        std::make_unique<HLE::SystemVABI::VAList>(guard, ppc_state.gpr[1] + 0x8, format_reg + 1);
  }

  GuestPrintArgs args(guard, *walker);
  return GetStringVA(&args, format);
}

// Common tail of the logging hooks: trim one trailing newline and emit the
// message tagged with the caller and current PC.
void EmitReport(const PowerPC::PowerPCState& ppc_state, std::string message)
{
  StringPopBackIf(&message, '\n');
  NOTICE_LOG_FMT(OSREPORT_HLE, "{:08x}->{:08x}| {}", LR(ppc_state), ppc_state.pc,
                 SHIFTJISToUTF8(message));
}

bool IsStdStream(s64 fd)
{
  return fd == 1 || fd == 2;
}

// OSReport and its many cousins come in several shapes. Probe the registers
// to guess which one this is and so where the format string lives.
u32 GuessFormatRegister(const Core::CPUThreadGuard& guard)
{
  const auto& ppc_state = guard.GetSystem().GetPPCState();
  const auto is_ram = [&guard](u32 address) {
    return PowerPC::MMU::HostIsRAMAddress(guard, address);
  };

  const u32 r3 = ppc_state.gpr[3];
  const u32 r4 = ppc_state.gpr[4];

  if (!is_ram(r3))
    return 4;  // f(int log_type, const char* fmt, ...)

  // r3 is a pointer. If it points at another pointer (or null) rather than
  // at text, it's most likely a `this` argument.
  const u32 pointee = PowerPC::MMU::HostRead<u32>(guard, r3);
  const bool looks_like_object = is_ram(pointee) || pointee == 0;
  if (!looks_like_object)
    return 3;  // f(const char* fmt, ...)

  if (is_ram(r4))
    return 4;  // f(void* this, const char* fmt, ...)
  return 5;    // f(void* this, int log_type, const char* fmt, ...)
}

void DebugPrint(const Core::CPUThreadGuard& guard, ArgSource source)
{
  const u32 format_reg = GuessFormatRegister(guard);
  EmitReport(guard.GetSystem().GetPPCState(), ExpandGuestFormat(guard, format_reg, source));
}

// (v)dprintf(int fd, const char* fmt, ...): only stdout and stderr are shown.
void DescriptorPrint(const Core::CPUThreadGuard& guard, ArgSource source)
{
  const auto& ppc_state = guard.GetSystem().GetPPCState();
  if (!IsStdStream(ppc_state.gpr[3]))
    return;
  EmitReport(ppc_state, ExpandGuestFormat(guard, 4, source));
}

// (v)fprintf(FILE* stream, const char* fmt, ...): FILE is opaque, but the fd
// is at a known spot for the common runtimes, so that is used to recognise
// stdout and stderr.
void StreamPrint(const Core::CPUThreadGuard& guard, ArgSource source)
{
  const auto& ppc_state = guard.GetSystem().GetPPCState();
  const u32 stream = ppc_state.gpr[3];
  const auto fd_at = [&guard, stream](u32 offset) -> int {
    return static_cast<short>(PowerPC::MMU::HostRead<u16>(guard, stream + offset));
  };

  int fd = -1;
  // libogc and the Dolphin SDK keep a 16-bit fd at offset 0xE.
  if (PowerPC::MMU::HostIsRAMAddress(guard, stream) &&
      PowerPC::MMU::HostIsRAMAddress(guard, stream + 0xF))
  {
    fd = fd_at(0xE);
  }
  // The RVL SDK appears to keep it at offset 0x2 instead.
  if (!IsStdStream(fd))
    fd = fd_at(0x2);
  if (!IsStdStream(fd))
    return;

  EmitReport(ppc_state, ExpandGuestFormat(guard, 4, source));
}
}  // namespace

void HLE_OSPanic(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();

  // OSPanic(const char* file, int line, const char* fmt, ...)
  std::string where = ExpandGuestFormat(guard, 3);
  std::string what = ExpandGuestFormat(guard, 5);
  StringPopBackIf(&where, '\n');
  StringPopBackIf(&what, '\n');

  PanicAlertFmt("OSPanic: {}: {}", where, what);
  ERROR_LOG_FMT(OSREPORT_HLE, "{:08x}->{:08x}| OSPanic: {}: {}", LR(ppc_state), ppc_state.pc, where,
                what);

  ppc_state.npc = LR(ppc_state);
}

void HLE_GeneralDebugPrint(const Core::CPUThreadGuard& guard)
{
  DebugPrint(guard, ArgSource::Registers);
}

void HLE_GeneralDebugVPrint(const Core::CPUThreadGuard& guard)
{
  DebugPrint(guard, ArgSource::VaList);
}

// __write_console(int fd, const void* buffer, const u32* size)
void HLE_write_console(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  const auto& ppc_state = system.GetPPCState();

  std::string text = ExpandGuestFormat(guard, 4);

  const u32 size_ptr = ppc_state.gpr[5];
  if (!PowerPC::MMU::HostIsRAMAddress(guard, size_ptr))
  {
    ERROR_LOG_FMT(OSREPORT_HLE, "__write_console uses an unreachable size pointer");
  }
  else
  {
    const u32 length = system.GetMMU().Read<u32>(size_ptr);
    if (length == 0)
    {
      WARN_LOG_FMT(OSREPORT_HLE, "__write_console uses a size of zero");
    }
    else if (length <= text.size())
    {
      text.resize(length);
    }
    else
    {
      WARN_LOG_FMT(OSREPORT_HLE, "__write_console uses an invalid size of {:#010x}", length);
    }
  }

  EmitReport(ppc_state, std::move(text));
}

void HLE_LogDPrint(const Core::CPUThreadGuard& guard)
{
  DescriptorPrint(guard, ArgSource::Registers);
}

void HLE_LogVDPrint(const Core::CPUThreadGuard& guard)
{
  DescriptorPrint(guard, ArgSource::VaList);
}

void HLE_LogFPrint(const Core::CPUThreadGuard& guard)
{
  StreamPrint(guard, ArgSource::Registers);
}

void HLE_LogVFPrint(const Core::CPUThreadGuard& guard)
{
  StreamPrint(guard, ArgSource::VaList);
}

// ---------------------------------------------------------------------------
// printf-style formatting of guest strings
// ---------------------------------------------------------------------------

namespace
{
enum class Length
{
  Default,
  Char,        // hh
  Short,       // h
  Long,        // l
  LongLong,    // ll
  LongDouble,  // L
};

// Everything between the '%' and the conversion character.
struct Conversion
{
  bool left = false;
  bool plus = false;
  bool space = false;
  bool alternate = false;
  bool zero_pad = false;
  std::optional<u32> width;
  std::optional<u32> precision;
  Length length = Length::Default;
};

class FormatExpander
{
public:
  FormatExpander(HLEPrintArgs& args, std::string_view format) : m_args(args), m_format(format) {}

  std::string Run()
  {
    for (m_pos = 0; m_pos < m_format.size(); ++m_pos)
    {
      const char c = m_format[m_pos];
      if (c != '%')
      {
        m_out += c;
        continue;
      }
      if (!ExpandOne())
        break;
    }
    return std::move(m_out);
  }

private:
  bool AtEnd() const { return m_pos >= m_format.size(); }
  char Peek() const { return m_format[m_pos]; }
  bool PeekIs(char c) const { return !AtEnd() && Peek() == c; }

  // Handles one directive starting at the '%' under m_pos. On return m_pos
  // sits on the directive's last character. Returns false when the format
  // string ran out in the middle of the directive.
  bool ExpandOne()
  {
    const size_t percent = m_pos++;
    if (PeekIs('%'))
    {
      m_out += '%';
      return true;
    }

    Conversion conv;
    ParseFlags(conv);
    conv.width = ParseNumber(&conv.left);
    if (PeekIs('.'))
    {
      ++m_pos;
      // A missing or negative precision counts as zero.
      conv.precision = ParseNumber(nullptr).value_or(0);
    }
    ParseLength(conv);

    if (AtEnd())
    {
      // Truncated directive: echo it verbatim.
      m_out += m_format.substr(percent);
      return false;
    }

    if (!Convert(conv, Peek()))
    {
      // Unknown conversion. The span is computed exactly as the original
      // implementation did, unsigned wrap-around included, so the output
      // matches it byte for byte.
      m_out += m_format.substr(percent, percent - m_pos + 1);
    }
    return true;
  }

  void ParseFlags(Conversion& conv)
  {
    for (; !AtEnd(); ++m_pos)
    {
      switch (Peek())
      {
      case '-':
        conv.left = true;
        break;
      case '+':
        conv.plus = true;
        break;
      case ' ':
        conv.space = true;
        break;
      case '#':
        conv.alternate = true;
        break;
      case '0':
        conv.zero_pad = true;
        break;
      default:
        return;
      }
    }
  }

  // Parses a width (when `left_flag` is given) or a precision. '*' takes the
  // value from the arguments; a negative width turns into left alignment
  // while a negative precision is dropped.
  std::optional<u32> ParseNumber(bool* left_flag)
  {
    if (AtEnd())
      return std::nullopt;

    if (Peek() == '*')
    {
      ++m_pos;
      const s32 value = std::bit_cast<s32>(m_args.GetU32());
      if (value >= 0)
        return static_cast<u32>(value);
      if (left_flag == nullptr)
        return std::nullopt;
      *left_flag = true;
      return static_cast<u32>(-static_cast<s64>(value));
    }

    const size_t digits_begin = m_pos;
    while (!AtEnd() && Peek() >= '0' && Peek() <= '9')
      ++m_pos;
    if (digits_begin == m_pos)
      return std::nullopt;

    // Leading zeroes are dropped before parsing; an all-zero run is zero.
    const std::string_view digits = m_format.substr(digits_begin, m_pos - digits_begin);
    const size_t significant = digits.find_first_not_of('0');
    if (significant == std::string_view::npos)
      return 0;

    u32 parsed = 0;
    if (!TryParse(std::string(digits.substr(significant)), &parsed, 10))
      return std::nullopt;
    return parsed;
  }

  void ParseLength(Conversion& conv)
  {
    if (AtEnd())
      return;

    const char first = Peek();
    if (first != 'h' && first != 'l' && first != 'L')
      return;
    ++m_pos;

    const bool doubled = first != 'L' && PeekIs(first);
    if (doubled)
    {
      ++m_pos;
      conv.length = first == 'h' ? Length::Char : Length::LongLong;
      return;
    }
    if (first == 'h')
      conv.length = Length::Short;
    else if (first == 'l')
      conv.length = Length::Long;
    else
      conv.length = Length::LongDouble;
  }

  // Builds the flag/width/precision part of a printf directive.
  static std::string PrintfOptions(const Conversion& conv, bool keep_alternate, bool keep_precision)
  {
    std::string options;
    if (conv.left)
      options += '-';
    if (conv.plus)
      options += '+';
    if (conv.space)
      options += ' ';
    if (keep_alternate && conv.alternate)
      options += '#';
    if (conv.zero_pad)
      options += '0';
    if (conv.width)
      options += fmt::format("{}", *conv.width);
    if (keep_precision && conv.precision)
      options += fmt::format(".{}", *conv.precision);
    return options;
  }

  template <typename T> void Printf(const std::string& options, std::string_view suffix, T value)
  {
    const std::string directive = fmt::format("%{}{}", options, suffix);
    m_out += fmt::sprintf(directive.c_str(), value);
  }

  template <typename T> void Padded(const Conversion& conv, const T& value)
  {
    const auto spec = fmt::runtime(conv.left ? "{0:<{1}}" : "{0:>{1}}");
    fmt::format_to(std::back_inserter(m_out), spec, value, conv.width.value_or(0));
  }

  static u32 NarrowUnsigned(u32 value, Length length)
  {
    if (length == Length::Short)
      return static_cast<u16>(value);
    if (length == Length::Char)
      return static_cast<u8>(value);
    return value;
  }

  static s32 NarrowSigned(s32 value, Length length)
  {
    if (length == Length::Short)
      return static_cast<s16>(value);
    if (length == Length::Char)
      return static_cast<s8>(value);
    return value;
  }

  // Wide strings and characters are assumed to be UTF-16; that's what retail
  // titles tend to use (homebrew may differ, but there's no way to tell).
  static std::string FromWide(std::u16string_view text)
  {
    return UTF8ToSHIFTJIS(UTF16ToUTF8(text));
  }

  // Emits one conversion. Returns false for an unknown conversion character.
  bool Convert(const Conversion& conv, char specifier)
  {
    const bool wide = conv.length == Length::Long;
    const bool is_64bit = conv.length == Length::LongLong;

    // Every floating point conversion is handed straight to printf.
    constexpr std::string_view floating_conversions = "fFeEaAgG";
    if (floating_conversions.find(specifier) != std::string_view::npos)
    {
      Printf(PrintfOptions(conv, true, true), std::string_view(&specifier, 1), m_args.GetF64());
      return true;
    }

    switch (specifier)
    {
    case 's':
      if (wide)
        Padded(conv, FromWide(m_args.GetU16String(conv.precision)));
      else
        Padded(conv, m_args.GetString(conv.precision));
      return true;

    case 'c':
    {
      const s32 code = std::bit_cast<s32>(m_args.GetU32());
      if (wide)
      {
        const char16_t unit = static_cast<char16_t>(code);
        Padded(conv, FromWide(std::u16string_view(&unit, 1)));
      }
      else
      {
        Padded(conv, static_cast<char>(code));
      }
      return true;
    }

    case 'd':
    case 'i':
    {
      const std::string options = PrintfOptions(conv, false, true);
      if (is_64bit)
        Printf(options, PRId64, std::bit_cast<s64>(m_args.GetU64()));
      else
        Printf(options, PRId32, NarrowSigned(std::bit_cast<s32>(m_args.GetU32()), conv.length));
      return true;
    }

    case 'o':
    case 'x':
    case 'X':
    case 'u':
    {
      const bool octal_or_hex = specifier != 'u';
      const std::string options = PrintfOptions(conv, octal_or_hex, true);
      if (is_64bit)
      {
        const std::string_view suffix = specifier == 'o' ? PRIo64 :
                                        specifier == 'x' ? PRIx64 :
                                        specifier == 'X' ? PRIX64 :
                                                           PRIu64;
        Printf(options, suffix, m_args.GetU64());
      }
      else
      {
        const std::string_view suffix = specifier == 'o' ? PRIo32 :
                                        specifier == 'x' ? PRIx32 :
                                        specifier == 'X' ? PRIX32 :
                                                           PRIu32;
        Printf(options, suffix, NarrowUnsigned(m_args.GetU32(), conv.length));
      }
      return true;
    }

    case 'n':
      // Writes through a pointer and prints nothing; the guest's own code
      // still does the write, so there's nothing to do (or consume) here.
      return true;

    case 'p':
      Printf(PrintfOptions(conv, false, false), PRIx32, m_args.GetU32());
      return true;

    default:
      return false;
    }
  }

  HLEPrintArgs& m_args;
  std::string_view m_format;
  size_t m_pos = 0;
  std::string m_out;
};
}  // namespace

std::string GetStringVA(HLEPrintArgs* args, std::string_view string)
{
  return FormatExpander(*args, string).Run();
}

}  // namespace HLE_OS
