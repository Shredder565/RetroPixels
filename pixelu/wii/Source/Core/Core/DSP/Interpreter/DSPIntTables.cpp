// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The interpreter does not keep its own copy of the instruction encodings: each
// instruction word is decoded with the shared templates (DSPTables) and the
// template is then mapped to a handler, by mnemonic or, for the conditional
// families, by the family's base opcode.

#include "Core/DSP/Interpreter/DSPIntTables.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPTables.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"

namespace DSP::Interpreter
{
namespace
{
struct NamedHandler
{
  std::string_view mnemonic;
  InterpreterFunction function;
};

using I = Interpreter;

// Sorted by mnemonic (checked below) for binary search.
constexpr auto MAIN_HANDLERS = std::to_array<NamedHandler>({
    {"ABS", &I::abs},         {"ADD", &I::add},         {"ADDARN", &I::addarn},
    {"ADDAX", &I::addax},     {"ADDAXL", &I::addaxl},   {"ADDI", &I::addi},
    {"ADDIS", &I::addis},     {"ADDP", &I::addp},       {"ADDPAXZ", &I::addpaxz},
    {"ADDR", &I::addr},       {"ANDC", &I::andc},       {"ANDCF", &I::andcf},
    {"ANDF", &I::andf},       {"ANDI", &I::andi},       {"ANDR", &I::andr},
    {"ASL", &I::asl},         {"ASR", &I::asr},         {"ASR16", &I::asr16},
    {"ASRN", &I::asrn},       {"ASRNR", &I::asrnr},     {"ASRNRX", &I::asrnrx},
    {"BLOOP", &I::bloop},     {"BLOOPI", &I::bloopi},   {"CLR", &I::clr},
    {"CLR15", &I::srbith},    {"CLRL", &I::clrl},       {"CLRP", &I::clrp},
    {"CMP", &I::cmp},         {"CMPAXH", &I::cmpaxh},   {"CMPI", &I::cmpi},
    {"CMPIS", &I::cmpis},     {"DAR", &I::dar},         {"DEC", &I::dec},
    {"DECM", &I::decm},       {"HALT", &I::halt},       {"IAR", &I::iar},
    {"ILRR", &I::ilrr},       {"ILRRD", &I::ilrrd},     {"ILRRI", &I::ilrri},
    {"ILRRN", &I::ilrrn},     {"INC", &I::inc},         {"INCM", &I::incm},
    {"LOOP", &I::loop},       {"LOOPI", &I::loopi},     {"LR", &I::lr},
    {"LRI", &I::lri},         {"LRIS", &I::lris},       {"LRR", &I::lrr},
    {"LRRD", &I::lrrd},       {"LRRI", &I::lrri},       {"LRRN", &I::lrrn},
    {"LRS", &I::lrs},         {"LSL", &I::lsl},         {"LSL16", &I::lsl16},
    {"LSR", &I::lsr},         {"LSR16", &I::lsr16},     {"LSRN", &I::lsrn},
    {"LSRNR", &I::lsrnr},     {"LSRNRX", &I::lsrnrx},   {"M0", &I::srbith},
    {"M2", &I::srbith},       {"MADD", &I::madd},       {"MADDC", &I::maddc},
    {"MADDX", &I::maddx},     {"MOV", &I::mov},         {"MOVAX", &I::movax},
    {"MOVNP", &I::movnp},     {"MOVP", &I::movp},       {"MOVPZ", &I::movpz},
    {"MOVR", &I::movr},       {"MRR", &I::mrr},         {"MSUB", &I::msub},
    {"MSUBC", &I::msubc},     {"MSUBX", &I::msubx},     {"MUL", &I::mul},
    {"MULAC", &I::mulac},     {"MULAXH", &I::mulaxh},   {"MULC", &I::mulc},
    {"MULCAC", &I::mulcac},   {"MULCMV", &I::mulcmv},   {"MULCMVZ", &I::mulcmvz},
    {"MULMV", &I::mulmv},     {"MULMVZ", &I::mulmvz},   {"MULX", &I::mulx},
    {"MULXAC", &I::mulxac},   {"MULXMV", &I::mulxmv},   {"MULXMVZ", &I::mulxmvz},
    {"NEG", &I::neg},         {"NOP", &I::nop},         {"NOT", &I::notc},
    {"NX", &I::nx},           {"ORC", &I::orc},         {"ORI", &I::ori},
    {"ORR", &I::orr},         {"SBCLR", &I::sbclr},     {"SBSET", &I::sbset},
    {"SET15", &I::srbith},    {"SET16", &I::srbith},    {"SET40", &I::srbith},
    {"SI", &I::si},           {"SR", &I::sr},           {"SRR", &I::srr},
    {"SRRD", &I::srrd},       {"SRRI", &I::srri},       {"SRRN", &I::srrn},
    {"SRS", &I::srs},         {"SRSH", &I::srsh},       {"SUB", &I::sub},
    {"SUBARN", &I::subarn},   {"SUBAX", &I::subax},     {"SUBP", &I::subp},
    {"SUBR", &I::subr},       {"TST", &I::tst},         {"TSTAXH", &I::tstaxh},
    {"TSTPROD", &I::tstprod}, {"XORC", &I::xorc},       {"XORI", &I::xori},
    {"XORR", &I::xorr},
});

constexpr auto EXT_HANDLERS = std::to_array<NamedHandler>({
    {"DR", &I::dr},       {"IR", &I::ir},         {"L", &I::l},         {"LD", &I::ld},
    {"LDAX", &I::ldax},   {"LDAXM", &I::ldaxm},   {"LDAXN", &I::ldaxn}, {"LDAXNM", &I::ldaxnm},
    {"LDM", &I::ldm},     {"LDN", &I::ldn},       {"LDNM", &I::ldnm},   {"LN", &I::ln},
    {"LS", &I::ls},       {"LSM", &I::lsm},       {"LSN", &I::lsn},     {"LSNM", &I::lsnm},
    {"MV", &I::mv},       {"NR", &I::nr},         {"S", &I::s},         {"SL", &I::sl},
    {"SLM", &I::slm},     {"SLN", &I::sln},       {"SLNM", &I::slnm},   {"SN", &I::sn},
    {"XXX", &I::nop_ext},
});

// The conditional instructions have one template per condition code, all with
// the same handler; they are recognised by the family's base opcode.
struct ConditionalHandler
{
  u16 base;
  InterpreterFunction function;
};

constexpr std::array<ConditionalHandler, 7> CONDITIONAL_HANDLERS{{
    {0x0270, &I::ifcc},
    {0x0290, &I::jcc},
    {0x02b0, &I::call},
    {0x02d0, &I::ret},
    {0x02f0, &I::rti},
    {0x1700, &I::jmprcc},
    {0x1710, &I::callr},
}};

template <size_t N>
constexpr bool IsSorted(const std::array<NamedHandler, N>& table)
{
  for (size_t i = 1; i < N; ++i)
  {
    if (!(table[i - 1].mnemonic < table[i].mnemonic))
      return false;
  }
  return true;
}

static_assert(IsSorted(MAIN_HANDLERS));
static_assert(IsSorted(EXT_HANDLERS));

template <size_t N>
InterpreterFunction LookUp(const std::array<NamedHandler, N>& table, std::string_view mnemonic)
{
  const auto end = table.end();
  const auto it = std::lower_bound(
      table.begin(), end, mnemonic,
      [](const NamedHandler& entry, std::string_view key) { return entry.mnemonic < key; });
  if (it == end || it->mnemonic != mnemonic)
    return nullptr;
  return it->function;
}

InterpreterFunction MainHandler(const DSPOPCTemplate& op)
{
  if (op.opcode_mask == 0xffff || op.opcode_mask == 0xff1f)
  {
    for (const ConditionalHandler& family : CONDITIONAL_HANDLERS)
    {
      if ((op.opcode & 0xfff0) == family.base)
        return family.function;
    }
  }
  return LookUp(MAIN_HANDLERS, op.name);
}

std::array<InterpreterFunction, 0x10000> s_main_table;
std::array<InterpreterFunction, 0x100> s_ext_table;
bool s_tables_built = false;

// Calls lookup for every instruction word and fills table with the handler of
// the template it returns (or NOP for words nothing decodes).
template <size_t N, typename Lookup, typename Resolve>
void FillTable(std::array<InterpreterFunction, N>& table, Lookup lookup, Resolve resolve)
{
  const DSPOPCTemplate* previous = nullptr;
  InterpreterFunction handler = &Interpreter::nop;
  for (size_t word = 0; word < N; ++word)
  {
    const DSPOPCTemplate* op = lookup(static_cast<UDSPInstruction>(word));
    if (op != previous || word == 0)
    {
      previous = op;
      handler = nullptr;
      if (op != nullptr)
        handler = resolve(*op);
      if (handler == nullptr)
        handler = &Interpreter::nop;
    }
    table[word] = handler;
  }
}
}  // namespace

InterpreterFunction GetOp(UDSPInstruction inst)
{
  return s_main_table[inst];
}

InterpreterFunction GetExtOp(UDSPInstruction inst)
{
  const bool short_extension = (inst >> 12) == 0x3;
  return s_ext_table[short_extension ? (inst & 0x7f) : (inst & 0xff)];
}

void InitInstructionTables()
{
  if (s_tables_built)
    return;

  FillTable(s_ext_table, FindExtOpInfoByOpcode, [](const DSPOPCTemplate& op) {
    return LookUp(EXT_HANDLERS, op.name);
  });
  FillTable(s_main_table, FindOpInfoByOpcode, MainHandler);

  s_tables_built = true;
}
}  // namespace DSP::Interpreter
