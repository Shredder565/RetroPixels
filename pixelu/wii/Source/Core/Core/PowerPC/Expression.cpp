// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Expression.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

// expr.h is C code compiled as C++; it calls isnan/isinf unqualified, which
// <cmath> is not required to put in the global namespace.
using std::isinf;
using std::isnan;
#include <expr.h>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
// ---------------------------------------------------------------------------
// Functions callable from a condition. All values are doubles inside the
// expression; T is the type the value is presented as, Raw the type that is
// actually read, written or reinterpreted.
// ---------------------------------------------------------------------------

double Argument(vec_expr_t* args, int i)
{
  return expr_eval(&vec_nth(args, i));
}

template <typename T, typename Raw = T>
double ReadMemory(expr_func*, vec_expr_t* args, void*)
{
  if (vec_len(args) != 1)
    return 0;
  const u32 address = static_cast<u32>(Argument(args, 0));
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  const Raw raw = PowerPC::MMU::HostRead<Raw>(guard, address);
  return static_cast<double>(std::bit_cast<T>(raw));
}

template <typename T, typename Raw = T>
double WriteMemory(expr_func*, vec_expr_t* args, void*)
{
  if (vec_len(args) != 2)
    return 0;
  const T value = static_cast<T>(Argument(args, 0));
  const u32 address = static_cast<u32>(Argument(args, 1));
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  PowerPC::MMU::HostWrite<Raw>(guard, std::bit_cast<Raw>(value), address);
  return static_cast<double>(value);
}

template <typename T, typename Raw = T>
double Reinterpret(expr_func*, vec_expr_t* args, void*)
{
  if (vec_len(args) != 1)
    return 0;
  return static_cast<double>(std::bit_cast<T>(static_cast<Raw>(Argument(args, 0))));
}

// callstack(address) / callstack("name"): whether the current call stack has
// a frame at that address, or a function whose name contains the text.
double InCallstack(expr_func*, vec_expr_t* args, void*)
{
  if (vec_len(args) != 1)
    return 0;

  std::vector<Dolphin_Debugger::CallstackEntry> frames;
  {
    Core::CPUThreadGuard guard(Core::System::GetInstance());
    if (!Dolphin_Debugger::GetCallstack(guard, frames))
      return 0;
  }

  expr* arg = &vec_nth(args, 0);
  const double number = expr_eval(arg);
  if (!std::isnan(number))
  {
    const u32 address = static_cast<u32>(number);
    return std::ranges::any_of(frames, [&](const auto& f) { return f.vAddress == address; });
  }

  if (const char* text = expr_get_str(arg))
    return std::ranges::any_of(frames, [&](const auto& f) { return f.Name.contains(text); });

  return 0;
}

// A string argument is either a literal or the address of a NUL-terminated
// string in guest memory.
std::optional<std::string> StringArgument(const Core::CPUThreadGuard& guard, expr* arg)
{
  const double number = expr_eval(arg);
  if (!std::isnan(number))
    return PowerPC::MMU::HostGetString(guard, static_cast<u32>(number));
  if (const char* text = expr_get_str(arg))
    return std::string(text);
  return std::nullopt;
}

double StringsEqual(expr_func*, vec_expr_t* args, void*)
{
  if (vec_len(args) != 2)
    return 0;

  Core::CPUThreadGuard guard(Core::System::GetInstance());
  const std::optional<std::string> first = StringArgument(guard, &vec_nth(args, 0));
  if (!first)
    return 0;
  const std::optional<std::string> second = StringArgument(guard, &vec_nth(args, 1));
  if (!second)
    return 0;
  return *first == *second;
}

// The library walks this list until it finds an entry without a name.
std::array<expr_func, 23> s_functions{{
    {"read_u8", ReadMemory<u8>},
    {"read_s8", ReadMemory<s8, u8>},
    {"read_u16", ReadMemory<u16>},
    {"read_s16", ReadMemory<s16, u16>},
    {"read_u32", ReadMemory<u32>},
    {"read_s32", ReadMemory<s32, u32>},
    {"read_f32", ReadMemory<float, u32>},
    {"read_f64", ReadMemory<double, u64>},
    {"write_u8", WriteMemory<u8>},
    {"write_u16", WriteMemory<u16>},
    {"write_u32", WriteMemory<u32>},
    {"write_f32", WriteMemory<float, u32>},
    {"write_f64", WriteMemory<double, u64>},
    {"u8", Reinterpret<u8>},
    {"s8", Reinterpret<s8, u8>},
    {"u16", Reinterpret<u16>},
    {"s16", Reinterpret<s16, u16>},
    {"u32", Reinterpret<u32>},
    {"s32", Reinterpret<s32, u32>},
    {"callstack", InCallstack},
    {"streq", StringsEqual},
    {},
}};

// ---------------------------------------------------------------------------
// Register names a condition may use.
// ---------------------------------------------------------------------------

using Register = Expression::Register;
using RegisterKind = Expression::RegisterKind;

std::map<std::string, Register, std::less<>> BuildRegisterNames()
{
  std::map<std::string, Register, std::less<>> names;
  for (int i = 0; i < 32; ++i)
  {
    names.emplace(fmt::format("r{}", i), Register{RegisterKind::GPR, i});
    names.emplace(fmt::format("f{}", i), Register{RegisterKind::FPR, i});
  }
  names.emplace("pc", Register{RegisterKind::PC, 0});
  names.emplace("msr", Register{RegisterKind::MSR, 0});

  const std::pair<const char*, int> sprs[] = {
      {"xer", SPR_XER},       {"lr", SPR_LR},         {"ctr", SPR_CTR},
      {"dsisr", SPR_DSISR},   {"dar", SPR_DAR},       {"dec", SPR_DEC},
      {"sdr1", SPR_SDR},      {"srr0", SPR_SRR0},     {"srr1", SPR_SRR1},
      {"tbl", SPR_TL},        {"tbu", SPR_TU},        {"pvr", SPR_PVR},
      {"sprg0", SPR_SPRG0},   {"sprg1", SPR_SPRG1},   {"sprg2", SPR_SPRG2},
      {"sprg3", SPR_SPRG3},   {"ear", SPR_EAR},       {"hid0", SPR_HID0},
      {"hid1", SPR_HID1},     {"hid2", SPR_HID2},     {"hid4", SPR_HID4},
      {"iabr", SPR_IABR},     {"dabr", SPR_DABR},     {"wpar", SPR_WPAR},
      {"dmau", SPR_DMAU},     {"dmal", SPR_DMAL},     {"ecid_u", SPR_ECID_U},
      {"ecid_m", SPR_ECID_M}, {"ecid_l", SPR_ECID_L}, {"usia", SPR_USIA},
      {"sia", SPR_SIA},       {"l2cr", SPR_L2CR},     {"ictc", SPR_ICTC},
      {"mmcr0", SPR_MMCR0},   {"mmcr1", SPR_MMCR1},   {"pmc1", SPR_PMC1},
      {"pmc2", SPR_PMC2},     {"pmc3", SPR_PMC3},     {"pmc4", SPR_PMC4},
      {"thrm1", SPR_THRM1},   {"thrm2", SPR_THRM2},   {"thrm3", SPR_THRM3},
  };
  for (const auto& [name, spr] : sprs)
    names.emplace(name, Register{RegisterKind::SPR, spr});

  // BATs: IBAT0-3 and DBAT0-3 are SPRs 528-543, IBAT4-7 and DBAT4-7 are
  // 560-575, each pair upper word first.
  for (int i = 0; i < 8; ++i)
  {
    const int ibat = (i < 4 ? SPR_IBAT0U : SPR_IBAT4U) + (i % 4) * 2;
    const int dbat = (i < 4 ? SPR_DBAT0U : SPR_DBAT4U) + (i % 4) * 2;
    names.emplace(fmt::format("ibat{}u", i), Register{RegisterKind::SPR, ibat});
    names.emplace(fmt::format("ibat{}l", i), Register{RegisterKind::SPR, ibat + 1});
    names.emplace(fmt::format("dbat{}u", i), Register{RegisterKind::SPR, dbat});
    names.emplace(fmt::format("dbat{}l", i), Register{RegisterKind::SPR, dbat + 1});
    names.emplace(fmt::format("gqr{}", i), Register{RegisterKind::SPR, SPR_GQR0 + i});
  }
  return names;
}

Register LookUpRegister(std::string_view name)
{
  static const auto names = BuildRegisterNames();
  const auto it = names.find(name);
  return it != names.end() ? it->second : Register{};
}

u32 ToRegisterValue(double value)
{
  return static_cast<u32>(static_cast<s64>(value));
}
}  // namespace

void ExprDeleter::operator()(expr* expression) const
{
  expr_destroy(expression, nullptr);
}

void ExprVarListDeleter::operator()(expr_var_list* vars) const
{
  // expr_destroy frees the variables in the list; the list head is ours.
  expr_destroy(nullptr, vars);
  delete vars;
}

Expression::Expression(std::string_view text, ExprPointer ex, ExprVarListPointer vars)
    : m_text(text), m_expr(std::move(ex)), m_vars(std::move(vars))
{
  for (const expr_var* var = m_vars->head; var != nullptr; var = var->next)
    m_registers.push_back(LookUpRegister(var->name));
}

std::optional<Expression> Expression::TryParse(std::string_view text)
{
  ExprVarListPointer vars{new expr_var_list{}};
  ExprPointer parsed{expr_create(text.data(), text.length(), vars.get(), s_functions.data())};
  if (!parsed)
    return std::nullopt;
  return Expression{text, std::move(parsed), std::move(vars)};
}

double Expression::Evaluate(Core::System& system) const
{
  LoadVariables(system);
  const double result = expr_eval(m_expr.get());
  StoreVariables(system);
  LogResult(result);
  return result;
}

void Expression::LoadVariables(Core::System& system) const
{
  const auto& state = system.GetPPCState();
  auto reg = m_registers.begin();
  for (expr_var* var = m_vars->head; var != nullptr; var = var->next, ++reg)
  {
    switch (reg->kind)
    {
    case RegisterKind::None:
      var->value = 0;
      break;
    case RegisterKind::GPR:
      var->value = static_cast<double>(state.gpr[reg->index]);
      break;
    case RegisterKind::FPR:
      var->value = state.ps[reg->index].PS0AsDouble();
      break;
    case RegisterKind::SPR:
      var->value = static_cast<double>(state.spr[reg->index]);
      break;
    case RegisterKind::PC:
      var->value = static_cast<double>(state.pc);
      break;
    case RegisterKind::MSR:
      var->value = static_cast<double>(state.msr.Hex);
      break;
    }
  }
}

void Expression::StoreVariables(Core::System& system) const
{
  // An expression may assign to registers (e.g. "r3 = 0, 1"). The PC cannot
  // be changed this way.
  auto& state = system.GetPPCState();
  auto reg = m_registers.begin();
  for (const expr_var* var = m_vars->head; var != nullptr; var = var->next, ++reg)
  {
    switch (reg->kind)
    {
    case RegisterKind::None:
    case RegisterKind::PC:
      break;
    case RegisterKind::GPR:
      state.gpr[reg->index] = ToRegisterValue(var->value);
      break;
    case RegisterKind::FPR:
      state.ps[reg->index].SetPS0(var->value);
      break;
    case RegisterKind::SPR:
      state.spr[reg->index] = ToRegisterValue(var->value);
      if (reg->index == SPR_SDR)
        system.GetMMU().SDRUpdated();
      break;
    case RegisterKind::MSR:
      state.msr.Hex = ToRegisterValue(var->value);
      system.GetPowerPC().MSRUpdated();
      break;
    }
  }
}

void Expression::LogResult(double result) const
{
  bool saw_nan = std::isnan(result);
  std::string variables;
  for (const expr_var* var = m_vars->head; var != nullptr; var = var->next)
  {
    saw_nan = saw_nan || std::isnan(var->value);
    fmt::format_to(std::back_inserter(variables), "  {}={}", var->name, var->value);
  }

  if (saw_nan)
  {
    variables += "\nBreakpoint condition encountered a NaN";
    Core::DisplayMessage("Breakpoint condition has encountered a NaN.", 2000);
  }

  if (saw_nan || result != 0.0)
    NOTICE_LOG_FMT(MEMMAP, "Breakpoint condition returned: {}. Vars:{}", result, variables);
}

std::string Expression::GetText() const
{
  return m_text;
}

void Expression::ComputeRegistersUsed()
{
  if (m_registers_used_known)
    return;

  for (const Register& reg : m_registers)
  {
    if (reg.kind == RegisterKind::GPR)
      m_gprs_used[reg.index] = true;
    else if (reg.kind == RegisterKind::FPR)
      m_fprs_used[reg.index] = true;
  }
  m_registers_used_known = true;
}
