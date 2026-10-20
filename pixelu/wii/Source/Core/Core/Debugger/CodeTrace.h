// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

struct InstructionAttributes
{
  u32 address = 0;
  std::string instruction = "";
  std::string reg0 = "";
  std::string reg1 = "";
  std::string reg2 = "";
  std::string reg3 = "";
  std::optional<u32> memory_target = std::nullopt;
  u32 memory_target_size = 4;
  bool is_store = false;
  bool is_load = false;
};

struct TraceOutput
{
  u32 address = 0;
  std::optional<u32> memory_target = std::nullopt;
  std::string instruction;
};

struct AutoStepResults
{
  std::vector<std::string> reg_tracked;
  std::set<u32> mem_tracked;
  u32 count = 0;
  bool timed_out = false;
  bool trackers_empty = false;
};

enum class HitType : u32
{
  SKIP = (1 << 0),       // irrelevant to the tracked value
  OVERWRITE = (1 << 1),  // tracked location replaced by an untracked value
  MOVED = (1 << 2),      // copied unchanged to another register
  SAVELOAD = (1 << 3),   // stored to or loaded from memory (wins over POINTER)
  POINTER = (1 << 4),    // used as the address or index of an access
  PASSIVE = (1 << 5),    // read (compares and the like) without being changed
  ACTIVE = (1 << 6),     // used to compute a new value in another register
  UPDATED = (1 << 7),    // modified in place in the same register
};

class CodeTrace
{
public:
  enum class AutoStop
  {
    Always,
    Used,
    Changed
  };

  void SetRegTracked(const std::string& reg);
  AutoStepResults AutoStepping(const Core::CPUThreadGuard& guard, bool continue_previous = false,
                               AutoStop stop_on = AutoStop::Always);

private:
  InstructionAttributes GetInstructionAttributes(const TraceOutput& line) const;
  TraceOutput SaveCurrentInstruction(const Core::CPUThreadGuard& guard) const;
  HitType TraceLogic(const TraceOutput& current_instr, bool first_hit = false);

  bool m_recording = false;
  std::vector<std::string> m_reg_autotrack;
  std::set<u32> m_mem_autotrack;
};
