// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// A two-pass assembler for the GameCube/Wii audio DSP. Pass one sizes the
// program and collects labels; pass two emits the words.

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPDisassembler.h"
#include "Core/DSP/DSPTables.h"
#include "Core/DSP/LabelMap.h"

namespace DSP
{
enum class AssemblerError
{
  OK,
  Unknown,
  UnknownOpcode,
  NotEnoughParameters,
  TooManyParameters,
  WrongParameter,
  ExpectedParamStr,
  ExpectedParamVal,
  ExpectedParamReg,
  ExpectedParamMem,
  ExpectedParamImm,
  IncorrectBinary,
  IncorrectHex,
  IncorrectDecimal,
  LabelAlreadyExists,
  UnknownLabel,
  NoMatchingBrackets,
  CantExtendOpcode,
  ExtensionParamsOnNonExtendableOpcode,
  WrongParameterExpectedAccumulator,
  WrongParameterExpectedMidAccumulator,
  InvalidRegister,
  NumberOutOfRange,
  PCOutOfRange,
};

// Labels persist for the lifetime of the object (they are rebuilt at the start of
// every Assemble call), as does the include directory set with INCDIR.
class DSPAssembler
{
public:
  explicit DSPAssembler(const AssemblerSettings& settings);
  ~DSPAssembler();

  // On failure, GetErrorString describes the last error. line_numbers, if given,
  // is cleared; per-word line numbers are not produced.
  bool Assemble(const std::string& text, std::vector<u16>& code,
                std::vector<int>* line_numbers = nullptr);

  std::string GetErrorString() const { return m_last_error_str; }
  AssemblerError GetError() const { return m_last_error; }

private:
  static constexpr size_t MAX_OPERANDS = 10;
  static constexpr size_t LINE_BUFFER_SIZE = 1024;

  struct Operand
  {
    u32 value = 0;
    partype_t kind = P_NONE;
    std::string text;  // for string operands
  };
  using OperandList = std::array<Operand, MAX_OPERANDS>;

  enum class Segment
  {
    Code,
    Data,
    Overlay,
    Count,
  };

  enum class OpcodeType
  {
    Primary,
    Extension
  };

  // Where in the source the assembler is, for messages.
  struct SourcePosition
  {
    u32 line_num = 0;
    std::string line_text;
    std::optional<OpcodeType> opcode_type;
    std::optional<size_t> operand_number;
    std::optional<std::string> include_path;
  };

  bool RunPass(const std::string& text, int pass);
  void StartPass(int pass);
  bool HandleDirective(const char* name, const OperandList& operands, int pass);

  u32 ParseOperands(char* text, OperandList& operands);
  u32 EvaluateExpression(std::string expression);
  u32 EvaluateTerm(const std::string& term);
  std::optional<std::string> ExtractBracketGroup(std::string& expression, std::string& inner);

  const DSPOPCTemplate* LookUpOpcode(std::string name, size_t operand_count, OpcodeType type);
  void CheckOperands(const DSPOPCTemplate& op, const OperandList& operands, size_t count);
  void CheckRegisterOperand(const param2_t& field, const Operand& operand);
  void CheckNumberRange(const param2_t& field, const Operand& operand);
  void Encode(const DSPOPCTemplate& op, const OperandList& operands, size_t count);

  std::string DescribePosition() const;
  void Fail(AssemblerError error);
  template <typename... Args>
  void Fail(AssemblerError error, fmt::format_string<Args...> format, Args&&... args);
  template <typename... Args>
  void Warn(fmt::format_string<Args...> format, Args&&... args);
  void RecordError(AssemblerError error, const std::optional<std::string>& detail);

  const AssemblerSettings m_settings;

  std::vector<u16> m_output_buffer;
  std::string m_include_dir;

  u32 m_cur_addr = 0;
  int m_total_size = 0;
  int m_cur_pass = 0;

  LabelMap m_labels;
  std::map<std::string, std::string> m_aliases;

  Segment m_cur_segment = Segment::Code;
  std::array<u32, static_cast<size_t>(Segment::Count)> m_segment_addr{};

  bool m_failed = false;
  std::string m_last_error_str;
  AssemblerError m_last_error = AssemblerError::OK;

  SourcePosition m_position;
};
}  // namespace DSP
