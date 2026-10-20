// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The Action Replay code engine.
//
// Each code line is an address word and a value word. The address word is split into fields:
//   bits 0-24  address (relative to 0x80000000)
//   bits 25-26 data size (8, 16, 32 bit, 32 bit float)
//   bits 27-29 type (0 = write/pointer/add/master, 1-7 = comparisons)
//   bits 30-31 subtype (for type 0: what to do; for comparisons: how many lines to skip)
// An address word of 0 is a "zero code" whose kind is in the top 3 bits of the value word. Some
// zero codes (fill and slide, memory copy) take their arguments from the line that follows.
//
// Every step is traced through LogStep(); the trace goes to the log and, when self logging is on,
// to a buffer the cheats window shows. Its text is part of what users see, so it is stable.

#include "Core/ActionReplay.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/ARDecrypt.h"
#include "Core/AchievementManager.h"
#include "Core/CheatCodes.h"
#include "Core/Config/MainSettings.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/PowerPC/MMU.h"

namespace ActionReplay
{
namespace
{
// Guards the code lists and the trace buffer.
std::mutex s_mutex;
std::vector<ARCode> s_running;
std::vector<ARCode> s_netplay_synced;
std::vector<std::string> s_trace;
std::atomic<bool> s_trace_enabled{false};
// Code being run, for the names in error messages.
const ARCode* s_running_code = nullptr;
// Tracing stops after the first frame that runs the codes, until the codes change.
bool s_trace_paused = false;

void VLogStep(std::string_view format, fmt::format_args args)
{
  if (s_trace_paused)
    return;

  const bool keep = s_trace_enabled.load(std::memory_order_relaxed);
  if constexpr (Common::Log::MAX_EFFECTIVE_LOGLEVEL < Common::Log::LogLevel::LINFO)
  {
    if (!keep)
      return;
  }

  std::string line = fmt::vformat(format, args);
  INFO_LOG_FMT(ACTIONREPLAY, "{}", line);
  if (keep)
    s_trace.push_back(line + '\n');
}

template <typename... Args>
void LogStep(std::string_view format, const Args&... args)
{
  VLogStep(format, fmt::make_format_args(args...));
}

const std::string& RunningName()
{
  return s_running_code->name;
}

// The fields of an address word.
struct CodeWord
{
  explicit CodeWord(u32 raw) : word(raw) {}

  u32 Offset() const { return word & 0x01FFFFFF; }
  u32 Size() const { return (word >> 25) & 3; }
  u32 Type() const { return (word >> 27) & 7; }
  u32 Subtype() const { return word >> 30; }
  u32 Target() const { return Offset() | 0x80000000; }

  u32 word;
};

enum DataSize : u32
{
  Size8 = 0,
  Size16 = 1,
  Size32 = 2,
  SizeFloat = 3,
};

// What a failed comparison skips (the subtype of a comparison code).
enum SkipMode : u32
{
  SkipOneLine = 0,
  SkipTwoLines = 1,
  SkipUntilEndIf = 2,
  SkipRest = 3,
};

u8 Read8(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead<u8>(guard, address);
}
u16 Read16(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead<u16>(guard, address);
}
u32 Read32(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead<u32>(guard, address);
}

void InvalidSizeAlert(const char* where_text, u32 size, u32 address);

class CodeRunner
{
public:
  CodeRunner(const Core::CPUThreadGuard& guard, const ARCode& code) : m_guard(guard), m_code(code)
  {
  }

  // Returns false if the code hit something it can't do; such codes are dropped.
  bool Run();

private:
  enum class Pending
  {
    None,
    FillAndSlide,
    MemoryCopy,
  };

  // Returns 1 to continue with the next line, 0 to stop with success, -1 to stop with failure.
  int Step(const AREntry& line);
  bool SkipLine(const AREntry& line, bool* stop);
  int ZeroCode(u32 value);

  bool Write(CodeWord word, u32 value);
  bool WriteThroughPointer(CodeWord word, u32 value);
  bool Add(CodeWord word, u32 value);
  bool Compare(CodeWord word, u32 value);
  bool FillAndSlide(CodeWord word, u32 value);
  bool MemoryCopy(CodeWord word, u32 value);

  const Core::CPUThreadGuard& m_guard;
  const ARCode& m_code;
  Pending m_pending = Pending::None;
  u32 m_pending_word = 0;
  // Lines still to skip after a failed comparison (> 0), or a SkipMode as a negative number.
  int m_skip = 0;
};

bool CodeRunner::Run()
{
  s_running_code = &m_code;

  LogStep("Code Name: {}", m_code.name);
  LogStep("Number of codes: {}", m_code.ops.size());

  for (const AREntry& line : m_code.ops)
  {
    const int result = Step(line);
    if (result <= 0)
      return result == 0;
  }
  return true;
}

bool CodeRunner::SkipLine(const AREntry& line, bool* stop)
{
  if (m_skip == 0)
    return false;

  if (m_skip > 0)
  {
    LogStep("Line skipped");
    --m_skip;
  }
  else if (m_skip == -static_cast<int>(SkipRest))
  {
    LogStep("All Lines skipped");
    *stop = true;
  }
  else if (m_skip == -static_cast<int>(SkipUntilEndIf))
  {
    LogStep("Line skipped");
    // "00000000 40000000" ends the skipped block.
    if (line.cmd_addr == 0 && line.value == 0x40000000)
      m_skip = 0;
  }
  return true;
}

int CodeRunner::Step(const AREntry& line)
{
  bool stop = false;
  if (SkipLine(line, &stop))
    return stop ? 0 : 1;

  const CodeWord word(line.cmd_addr);
  const u32 value = line.value;

  LogStep("--- Running Code: {:08x} {:08x} ---", word.word, value);

  if (m_pending != Pending::None)
  {
    const Pending what = std::exchange(m_pending, Pending::None);
    bool ok;
    if (what == Pending::FillAndSlide)
    {
      LogStep("Doing Fill And Slide");
      ok = FillAndSlide(word, value);
    }
    else
    {
      LogStep("Doing Memory Copy");
      ok = MemoryCopy(word, value);
    }
    return ok ? 1 : -1;
  }

  if (word.word >= 0x00002000 && word.word < 0x00003000)
  {
    LogStep("This action replay simulator does not support codes that modify Action Replay itself.");
    PanicAlertFmtT(
        "This action replay simulator does not support codes that modify Action Replay itself.");
    return -1;
  }

  if (word.word == 0)
    return ZeroCode(value);

  LogStep("Doing Normal Code {:08x}", word.Type());
  LogStep("Subtype: {:08x}", word.Subtype());

  if (word.Type() != 0)
  {
    LogStep("This Normal Code is a Conditional Code");
    return Compare(word, value) ? 1 : -1;
  }

  switch (word.Subtype())
  {
  case 0:
    LogStep("Doing Ram Write And Fill");
    return Write(word, value) ? 1 : -1;
  case 1:
    LogStep("Doing Write To Pointer");
    return WriteThroughPointer(word, value) ? 1 : -1;
  case 2:
    LogStep("Doing Add Code");
    return Add(word, value) ? 1 : -1;
  case 3:
    LogStep("Doing Master Code And Write to CCXXXXXX (ncode not supported)");
    PanicAlertFmtT("Action Replay Error: Master Code and Write To CCXXXXXX not implemented ({0})\n"
                   "Master codes are not needed. Do not use master codes.",
                   RunningName());
    return -1;
  default:
    LogStep("Bad Subtype");
    PanicAlertFmtT("Action Replay: Normal Code 0: Invalid Subtype {0:08x} ({1})", word.Subtype(),
                   RunningName());
    return -1;
  }
}

int CodeRunner::ZeroCode(u32 value)
{
  const u32 kind = value >> 29;
  LogStep("Doing Zero Code {:08x}", kind);

  switch (kind)
  {
  case 0:
    LogStep("ZCode: End Of Codes");
    return 0;

  case 2:
    // Also covers the "00000000 40000000" end-if line outside of a skipped block.
    LogStep("ZCode: Normal execution of codes, set register 1BB4 to 0 (zcode not supported)");
    return 1;

  case 3:
    LogStep("ZCode: Executes all codes in the same row, Set register 1BB4 to 1 (zcode not "
            "supported)");
    PanicAlertFmtT("Zero 3 code not supported");
    return -1;

  case 4:
    if (((value >> 25) & 3) == 3)
    {
      LogStep("ZCode: Memory Copy");
      m_pending = Pending::MemoryCopy;
    }
    else
    {
      LogStep("ZCode: Fill And Slide");
      m_pending = Pending::FillAndSlide;
    }
    m_pending_word = value;
    return 1;

  default:
    LogStep("ZCode: Unknown");
    PanicAlertFmtT("Zero code unknown to Dolphin: {0:08x}", kind);
    return -1;
  }
}

void InvalidSizeAlert(const char* where_text, u32 size, u32 address)
{
  LogStep("Bad Size");
  const std::string_view where(where_text);
  if (where == "Ram Write And Fill")
  {
    PanicAlertFmtT("Action Replay Error: Invalid size "
                   "({0:08x} : address = {1:08x}) in Ram Write And Fill ({2})",
                   size, address, RunningName());
  }
  else if (where == "Write To Pointer")
  {
    PanicAlertFmtT("Action Replay Error: Invalid size "
                   "({0:08x} : address = {1:08x}) in Write To Pointer ({2})",
                   size, address, RunningName());
  }
  else if (where == "Add Code")
  {
    PanicAlertFmtT("Action Replay Error: Invalid size "
                   "({0:08x} : address = {1:08x}) in Add Code ({2})",
                   size, address, RunningName());
  }
  else
  {
    PanicAlertFmtT(
        "Action Replay Error: Invalid size ({0:08x} : address = {1:08x}) in Fill and Slide ({2})",
        size, address, RunningName());
  }
}

bool CodeRunner::Write(CodeWord word, u32 value)
{
  const u32 target = word.Target();
  LogStep("Hardware Address: {:08x}", target);
  LogStep("Size: {:08x}", word.Size());

  switch (word.Size())
  {
  case Size8:
  case Size16:
  {
    // The upper bits of the value give how many more copies follow the first one.
    const bool bytes = word.Size() == Size8;
    const unsigned width = bytes ? 1 : 2;
    const u32 extra = bytes ? value >> 8 : value >> 16;
    const u32 mask = bytes ? 0xFF : 0xFFFF;
    LogStep(bytes ? "8-bit Write" : "16-bit Write");
    LogStep("--------");
    for (u32 n = 0; n <= extra; ++n)
    {
      const u32 address = target + n * width;
      if (bytes)
        ApplyMemoryPatch<u8>(m_guard, static_cast<u8>(value), address);
      else
        ApplyMemoryPatch<u16>(m_guard, static_cast<u16>(value), address);
      LogStep("Wrote {:08x} to address {:08x}", value & mask, address);
    }
    LogStep("--------");
    return true;
  }
  case Size32:
  case SizeFloat:
    LogStep("32-bit Write");
    LogStep("--------");
    ApplyMemoryPatch<u32>(m_guard, value, target);
    LogStep("Wrote {:08x} to address {:08x}", value, target);
    LogStep("--------");
    return true;
  default:
    InvalidSizeAlert("Ram Write And Fill", word.Size(), word.Offset());
    return false;
  }
}

bool CodeRunner::WriteThroughPointer(CodeWord word, u32 value)
{
  const u32 target = word.Target();
  const u32 pointer = Read32(m_guard, target);
  LogStep("Hardware Address: {:08x}", target);
  LogStep("Size: {:08x}", word.Size());

  switch (word.Size())
  {
  case Size8:
  case Size16:
  {
    const bool bytes = word.Size() == Size8;
    const u32 small_value = bytes ? (value & 0xFF) : (value & 0xFFFF);
    // The rest of the value is the offset from the pointer, in units of the data size.
    const u32 offset = bytes ? value >> 8 : (value >> 16) << 1;
    LogStep(bytes ? "Write 8-bit to pointer" : "Write 16-bit to pointer");
    LogStep("--------");
    LogStep("Pointer: {:08x}", pointer);
    LogStep("Byte: {:08x}", small_value);
    LogStep("Offset: {:08x}", offset);
    if (bytes)
      ApplyMemoryPatch<u8>(m_guard, static_cast<u8>(small_value), pointer + offset);
    else
      ApplyMemoryPatch<u16>(m_guard, static_cast<u16>(small_value), pointer + offset);
    LogStep("Wrote {:08x} to address {:08x}", small_value, pointer + offset);
    LogStep("--------");
    return true;
  }
  case Size32:
  case SizeFloat:
    LogStep("Write 32-bit to pointer");
    LogStep("--------");
    ApplyMemoryPatch<u32>(m_guard, value, pointer);
    LogStep("Wrote {:08x} to address {:08x}", value, pointer);
    LogStep("--------");
    return true;
  default:
    InvalidSizeAlert("Write To Pointer", word.Size(), word.Offset());
    return false;
  }
}

bool CodeRunner::Add(CodeWord word, u32 value)
{
  const u32 target = word.Target();
  LogStep("Hardware Address: {:08x}", target);
  LogStep("Size: {:08x}", word.Size());

  switch (word.Size())
  {
  case Size8:
    LogStep("8-bit Add");
    LogStep("--------");
    ApplyMemoryPatch<u8>(m_guard, static_cast<u8>(Read8(m_guard, target) + value), target);
    LogStep("Wrote {:02x} to address {:08x}", Read8(m_guard, target), target);
    LogStep("--------");
    return true;
  case Size16:
    LogStep("16-bit Add");
    LogStep("--------");
    ApplyMemoryPatch<u16>(m_guard, static_cast<u16>(Read16(m_guard, target) + value), target);
    LogStep("Wrote {:04x} to address {:08x}", Read16(m_guard, target), target);
    LogStep("--------");
    return true;
  case Size32:
    LogStep("32-bit Add");
    LogStep("--------");
    ApplyMemoryPatch<u32>(m_guard, Read32(m_guard, target) + value, target);
    LogStep("Wrote {:08x} to address {:08x}", Read32(m_guard, target), target);
    LogStep("--------");
    return true;
  case SizeFloat:
  {
    LogStep("32-bit floating Add");
    LogStep("--------");
    // The value is an integer that is added to the float in memory.
    const u32 before = Read32(m_guard, target);
    const float sum = std::bit_cast<float>(before) + static_cast<float>(value);
    const u32 after = std::bit_cast<u32>(sum);
    ApplyMemoryPatch<u32>(m_guard, after, target);
    LogStep("Old Value {:08x}", before);
    LogStep("Increment {:08x}", value);
    LogStep("New value {:08x}", after);
    LogStep("--------");
    return true;
  }
  default:
    InvalidSizeAlert("Add Code", word.Size(), word.Offset());
    return false;
  }
}

bool CodeRunner::Compare(CodeWord word, u32 value)
{
  const u32 target = word.Target();
  LogStep("Size: {:08x}", word.Size());
  LogStep("Hardware Address: {:08x}", target);

  u32 current;
  u32 wanted;
  switch (word.Size())
  {
  case Size8:
    current = Read8(m_guard, target);
    wanted = value & 0xFF;
    break;
  case Size16:
    current = Read16(m_guard, target);
    wanted = value & 0xFFFF;
    break;
  case Size32:
  case SizeFloat:
    current = Read32(m_guard, target);
    wanted = value;
    break;
  default:
    LogStep("Bad Size");
    PanicAlertFmtT("Action Replay: Conditional Code: Invalid Size {0:08x} ({1})", word.Size(),
                   RunningName());
    return false;
  }

  bool holds;
  switch (word.Type())
  {
  case 1:
    LogStep("Type 1: If Equal");
    holds = current == wanted;
    break;
  case 2:
    LogStep("Type 2: If Not Equal");
    holds = current != wanted;
    break;
  case 3:
    LogStep("Type 3: If Less Than (Signed)");
    holds = static_cast<s32>(current) < static_cast<s32>(wanted);
    break;
  case 4:
    LogStep("Type 4: If Greater Than (Signed)");
    holds = static_cast<s32>(current) > static_cast<s32>(wanted);
    break;
  case 5:
    LogStep("Type 5: If Less Than (Unsigned)");
    holds = current < wanted;
    break;
  case 6:
    LogStep("Type 6: If Greater Than (Unsigned)");
    holds = current > wanted;
    break;
  case 7:
    LogStep("Type 7: If And");
    holds = (current & wanted) != 0;
    break;
  default:
    LogStep("Unknown Compare type");
    PanicAlertFmtT("Action Replay: Invalid Normal Code Type {0:08x} ({1})", word.Type(),
                   RunningName());
    holds = false;
    break;
  }

  if (holds)
    return true;

  switch (word.Subtype())
  {
  case SkipOneLine:
  case SkipTwoLines:
    m_skip = static_cast<int>(word.Subtype()) + 1;
    return true;
  case SkipUntilEndIf:
  case SkipRest:
    m_skip = -static_cast<int>(word.Subtype());
    return true;
  default:
    LogStep("Bad Subtype");
    PanicAlertFmtT("Action Replay: Normal Code {0}: Invalid subtype {1:08x} ({2})", 1,
                   word.Subtype(), RunningName());
    return false;
  }
}

// Writes a run of values, stepping both the address and the value. The zero code line holds the
// start address and data size; this line's address word is the first value, and its value word
// packs the value step (top byte), the count and the address step (low 16 bits, signed, in units
// of the data size).
bool CodeRunner::FillAndSlide(CodeWord word, u32 value)
{
  const CodeWord setup(m_pending_word);
  const u32 start = setup.Target();
  const u32 size = setup.Size();

  const auto address_step = static_cast<s16>(value & 0xFFFF);
  const auto value_step = static_cast<s8>(value >> 24);
  const auto count = static_cast<u8>(value >> 16);

  u32 current = word.word;
  u32 address = start;

  LogStep("Current Hardware Address: {:08x}", start);
  LogStep("Size: {:08x}", word.Size());
  LogStep("Write Num: {:08x}", count);
  LogStep("Address Increment: {}", address_step);
  LogStep("Value Increment: {}", s32{value_step});

  if (size > Size32)
  {
    InvalidSizeAlert("Fill and Slide", size, start);
    return false;
  }

  static constexpr const char* titles[] = {"8-bit Write", "16-bit Write", "32-bit Write"};
  const int unit = 1 << size;
  LogStep(titles[size]);
  LogStep("--------");
  for (int n = 0; n < count; ++n)
  {
    if (size == Size8)
    {
      // For bytes, the trace shows the stepped value and address (after the write).
      ApplyMemoryPatch<u8>(m_guard, static_cast<u8>(current), address);
      address += static_cast<u32>(address_step);
      current += static_cast<u32>(value_step);
      LogStep("Write {:08x} to address {:08x}", current & 0xFF, address);
    }
    else
    {
      if (size == Size16)
      {
        ApplyMemoryPatch<u16>(m_guard, static_cast<u16>(current), address);
        LogStep("Write {:08x} to address {:08x}", current & 0xFFFF, address);
      }
      else
      {
        ApplyMemoryPatch<u32>(m_guard, current, address);
        LogStep("Write {:08x} to address {:08x}", current, address);
      }
      address += static_cast<u32>(address_step * unit);
      current += static_cast<u32>(value_step);
    }
    LogStep("Value Update: {:08x}", current);
    LogStep("Current Hardware Address Update: {:08x}", address);
  }
  LogStep("--------");
  return true;
}

// Copies bytes. The zero code line holds the destination; this line's address word is the source
// and the low bits of its value word the byte count. A non-zero top byte makes both addresses
// pointers to the real addresses.
bool CodeRunner::MemoryCopy(CodeWord word, u32 value)
{
  const u32 destination = m_pending_word & ~0x06000000u;
  const u32 source = word.Target();
  const auto count = static_cast<u8>(value & 0x7FFF);

  LogStep("Dest Address: {:08x}", destination);
  LogStep("Src Address: {:08x}", source);
  LogStep("Size: {:08x}", count);

  if ((value & 0xFF0000) != 0)
  {
    LogStep("Bad Value");
    PanicAlertFmtT("Action Replay Error: Invalid value ({0:08x}) in Memory Copy ({1})",
                   value & 0xFFFF8000u, RunningName());
    return false;
  }

  u32 from = source;
  u32 to = destination;
  if ((value >> 24) != 0)
  {
    LogStep("Memory Copy With Pointers Support");
    LogStep("--------");
    to = Read32(m_guard, destination);
    LogStep("Resolved Dest Address to: {:08x}", to);
    from = Read32(m_guard, source);
    LogStep("Resolved Src Address to: {:08x}", from);
  }
  else
  {
    LogStep("Memory Copy Without Pointers Support");
    LogStep("--------");
  }

  for (u32 n = 0; n < count; ++n)
  {
    ApplyMemoryPatch<u8>(m_guard, Read8(m_guard, from + n), to + n);
    LogStep("Wrote {:08x} to address {:08x}", Read8(m_guard, from + n), to + n);
  }
  LogStep("--------");
  return true;
}

// Codes collected for one ini section.
class SectionReader
{
public:
  SectionReader(std::vector<ARCode>& out, bool user_defined) : m_out(out), m_user(user_defined) {}

  void Line(const std::string& line)
  {
    if (line.front() == '$')
    {
      // A new code starts; the previous one is finished.
      FlushPlain();
      FlushEncrypted();
      m_code.name = line.substr(1);
      m_code.user_defined = m_user;
      return;
    }

    const auto parsed = DeserializeLine(line);
    if (const auto* entry = std::get_if<AREntry>(&parsed))
      m_code.ops.push_back(*entry);
    else if (const auto* encrypted = std::get_if<EncryptedLine>(&parsed))
      m_encrypted.push_back(*encrypted);
    else
      PanicAlertFmtT("Action Replay Error: invalid AR code line: {0}", line);
  }

  // The last code of the section. Unlike a code followed by another, its decrypted lines are
  // added after its plain lines in a second copy of the code.
  void Finish()
  {
    if (!m_code.ops.empty())
      m_out.push_back(m_code);
    if (!m_encrypted.empty())
    {
      DecryptARCode(m_encrypted, &m_code.ops);
      m_out.push_back(m_code);
    }
  }

private:
  void FlushPlain()
  {
    if (m_code.ops.empty())
      return;
    m_out.push_back(m_code);
    m_code.ops.clear();
  }

  void FlushEncrypted()
  {
    if (m_encrypted.empty())
      return;
    DecryptARCode(m_encrypted, &m_code.ops);
    m_out.push_back(m_code);
    m_code.ops.clear();
    m_encrypted.clear();
  }

  std::vector<ARCode>& m_out;
  const bool m_user;
  ARCode m_code;
  std::vector<std::string> m_encrypted;
};
}  // namespace

void ApplyCodes(std::span<const ARCode> codes, const std::string& game_id, u16 revision)
{
  if (!Config::AreCheatsEnabled())
    return;

  std::lock_guard lock(s_mutex);
  s_trace_paused = false;
  s_running.clear();
  for (const ARCode& code : codes)
  {
    if (AchievementManager::GetInstance().ShouldARCodeBeActivated(code, game_id, revision))
      s_running.push_back(code);
  }
  s_running.shrink_to_fit();
}

void SetSyncedCodesAsActive()
{
  s_running = s_netplay_synced;
}

void UpdateSyncedCodes(std::span<const ARCode> codes)
{
  s_netplay_synced.clear();
  for (const ARCode& code : codes)
  {
    if (code.enabled)
      s_netplay_synced.push_back(code);
  }
  s_netplay_synced.shrink_to_fit();
}

std::vector<ARCode> ApplyAndReturnCodes(std::span<const ARCode> codes)
{
  if (Config::AreCheatsEnabled())
  {
    std::lock_guard lock(s_mutex);
    s_trace_paused = false;
    s_running.clear();
    for (const ARCode& code : codes)
    {
      if (code.enabled)
        s_running.push_back(code);
    }
  }
  s_running.shrink_to_fit();
  return s_running;
}

void AddCode(ARCode code)
{
  if (!Config::AreCheatsEnabled() || !code.enabled)
    return;

  std::lock_guard lock(s_mutex);
  s_trace_paused = false;
  s_running.push_back(std::move(code));
}

size_t CountEnabledCodes()
{
  if (!Config::AreCheatsEnabled())
    return 0;

  std::lock_guard lock(s_mutex);
  return s_running.size();
}

void LoadAndApplyCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini,
                       const std::string& game_id, u16 revision)
{
  ApplyCodes(LoadCodes(global_ini, local_ini), game_id, revision);
}

std::vector<ARCode> LoadCodes(const Common::IniFile& global_ini, const Common::IniFile& local_ini)
{
  std::vector<ARCode> codes;

  for (const Common::IniFile* ini : {&global_ini, &local_ini})
  {
    const bool is_local = ini == &local_ini;
    std::vector<std::string> lines;
    ini->GetLines("ActionReplay", &lines);

    SectionReader reader(codes, is_local);
    for (const std::string& line : lines)
    {
      if (!line.empty())
        reader.Line(line);
    }
    reader.Finish();

    ReadEnabledAndDisabled(*ini, "ActionReplay", &codes);

    // What the global ini switches on is the default.
    if (!is_local)
    {
      for (ARCode& code : codes)
        code.default_enabled = code.enabled;
    }
  }

  return codes;
}

void SaveCodes(Common::IniFile* local_ini, std::span<const ARCode> codes)
{
  std::vector<std::string> enabled;
  std::vector<std::string> disabled;
  std::vector<std::string> user_codes;

  for (const ARCode& code : codes)
  {
    if (code.enabled != code.default_enabled)
      (code.enabled ? enabled : disabled).push_back('$' + code.name);

    if (!code.user_defined)
      continue;

    user_codes.push_back('$' + code.name);
    for (const AREntry& op : code.ops)
      user_codes.push_back(SerializeLine(op));
  }

  local_ini->SetLines("ActionReplay_Enabled", enabled);
  local_ini->SetLines("ActionReplay_Disabled", disabled);
  local_ini->SetLines("ActionReplay", user_codes);
}

std::variant<std::monostate, AREntry, EncryptedLine> DeserializeLine(const std::string& line)
{
  const std::vector<std::string> words = SplitString(line, ' ');
  if (words.size() == 2 && words[0].size() == 8 && words[1].size() == 8)
  {
    AREntry entry;
    if (TryParse(words[0], &entry.cmd_addr, 16) && TryParse(words[1], &entry.value, 16))
      return entry;
  }

  // Encrypted lines are only decrypted once all lines of a code are known.
  const std::vector<std::string> groups = SplitString(line, '-');
  if (groups.size() == 3 && groups[0].size() == 4 && groups[1].size() == 4 && groups[2].size() == 5)
    return groups[0] + groups[1] + groups[2];

  return std::monostate{};
}

std::string SerializeLine(const AREntry& op)
{
  return fmt::format("{:08X} {:08X}", op.cmd_addr, op.value);
}

void EnableSelfLogging(bool enable)
{
  s_trace_enabled.store(enable, std::memory_order_relaxed);
}

std::vector<std::string> GetSelfLog()
{
  std::lock_guard lock(s_mutex);
  return s_trace;
}

void ClearSelfLog()
{
  std::lock_guard lock(s_mutex);
  s_trace.clear();
}

bool IsSelfLogging()
{
  return s_trace_enabled.load(std::memory_order_relaxed);
}

void RunAllActive(const Core::CPUThreadGuard& cpu_guard)
{
  if (!Config::AreCheatsEnabled())
    return;

  std::lock_guard lock(s_mutex);
  std::erase_if(s_running, [&cpu_guard](const ARCode& code) {
    const bool keep = CodeRunner(cpu_guard, code).Run();
    LogStep("\n");
    return !keep;
  });
  s_trace_paused = true;
}

}  // namespace ActionReplay
