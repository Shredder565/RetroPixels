// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

#include "Common/Assert.h"
#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64/RegCache/CachedReg.h"
#include "Core/PowerPC/Jit64/RegCache/RCMode.h"

using namespace Gen;

namespace
{
using WideContents = std::variant<std::monostate, X64Reg, u32, preg_t>;

// Widens an RCX64Reg's contents to the RCOpArg variant.
WideContents Widen(const std::variant<std::monostate, X64Reg, preg_t>& narrow)
{
  return std::visit([](const auto& value) -> WideContents { return value; }, narrow);
}
}  // namespace

// ---------------------------------------------------------------------------
// RCOpArg

RCOpArg::RCOpArg() = default;

RCOpArg::RCOpArg(u32 imm) : contents(imm)
{
}

RCOpArg::RCOpArg(X64Reg xr) : contents(xr)
{
}

RCOpArg::RCOpArg(RegCache* rc_, preg_t preg) : rc(rc_), contents(preg)
{
  rc->Lock(preg);
}

RCOpArg RCOpArg::Imm32(u32 imm)
{
  return RCOpArg{imm};
}

RCOpArg RCOpArg::R(X64Reg xr)
{
  return RCOpArg{xr};
}

RCOpArg::~RCOpArg()
{
  Unlock();
}

RCOpArg::RCOpArg(RCOpArg&& other) noexcept
    : rc(std::exchange(other.rc, nullptr)),
      contents(std::exchange(other.contents, std::monostate{}))
{
}

RCOpArg::RCOpArg(RCX64Reg&& other) noexcept
    : rc(std::exchange(other.rc, nullptr)),
      contents(Widen(std::exchange(other.contents, std::monostate{})))
{
}

RCOpArg& RCOpArg::operator=(RCOpArg&& other) noexcept
{
  Unlock();
  contents = std::exchange(other.contents, std::monostate{});
  rc = std::exchange(other.rc, nullptr);
  return *this;
}

RCOpArg& RCOpArg::operator=(RCX64Reg&& other) noexcept
{
  Unlock();
  contents = Widen(std::exchange(other.contents, std::monostate{}));
  rc = std::exchange(other.rc, nullptr);
  return *this;
}

void RCOpArg::Realize()
{
  if (std::holds_alternative<preg_t>(contents))
    rc->Realize(std::get<preg_t>(contents));
}

OpArg RCOpArg::Location() const
{
  if (std::holds_alternative<preg_t>(contents))
  {
    const preg_t preg = std::get<preg_t>(contents);
    ASSERT(rc->IsRealized(preg));
    return rc->R(preg);
  }
  if (std::holds_alternative<X64Reg>(contents))
    return Gen::R(std::get<X64Reg>(contents));
  if (std::holds_alternative<u32>(contents))
    return Gen::Imm32(std::get<u32>(contents));

  ASSERT(false);
  return {};
}

void RCOpArg::Unlock()
{
  if (std::holds_alternative<preg_t>(contents))
  {
    ASSERT(rc);
    rc->Unlock(std::get<preg_t>(contents));
  }
  else if (std::holds_alternative<X64Reg>(contents))
  {
    // Only a register that came from an RCX64Reg carries a lock; RCOpArg::R
    // leaves rc null.
    if (rc != nullptr)
      rc->UnlockX(std::get<X64Reg>(contents));
  }
  else
  {
    ASSERT(!rc);
  }

  contents = std::monostate{};
  rc = nullptr;
}

// ---------------------------------------------------------------------------
// RCX64Reg

RCX64Reg::RCX64Reg() = default;

RCX64Reg::RCX64Reg(RegCache* cache, preg_t guest) : rc(cache), contents(guest)
{
  rc->Lock(guest);
}

RCX64Reg::RCX64Reg(RegCache* cache, X64Reg host) : rc(cache), contents(host)
{
  rc->LockX(host);
}

RCX64Reg::~RCX64Reg()
{
  Unlock();
}

RCX64Reg::RCX64Reg(RCX64Reg&& donor) noexcept
    : rc(std::exchange(donor.rc, nullptr)),
      contents(std::exchange(donor.contents, std::monostate{}))
{
}

RCX64Reg& RCX64Reg::operator=(RCX64Reg&& donor) noexcept
{
  Unlock();
  contents = std::exchange(donor.contents, std::monostate{});
  rc = std::exchange(donor.rc, nullptr);
  return *this;
}

void RCX64Reg::Realize()
{
  if (std::holds_alternative<preg_t>(contents))
    rc->Realize(std::get<preg_t>(contents));
}

RCX64Reg::operator X64Reg() const&
{
  if (std::holds_alternative<X64Reg>(contents))
    return std::get<X64Reg>(contents);
  if (std::holds_alternative<preg_t>(contents))
  {
    const preg_t preg = std::get<preg_t>(contents);
    ASSERT(rc->IsRealized(preg));
    return rc->RX(preg);
  }

  ASSERT(false);
  return {};
}

RCX64Reg::operator OpArg() const&
{
  const X64Reg reg = *this;
  return Gen::R(reg);
}

void RCX64Reg::Unlock()
{
  if (std::holds_alternative<preg_t>(contents))
  {
    ASSERT(rc);
    rc->Unlock(std::get<preg_t>(contents));
  }
  else if (std::holds_alternative<X64Reg>(contents))
  {
    ASSERT(rc);
    rc->UnlockX(std::get<X64Reg>(contents));
  }
  else
  {
    ASSERT(!rc);
  }

  contents = std::monostate{};
  rc = nullptr;
}

// ---------------------------------------------------------------------------
// RCForkGuard

RCForkGuard::RCForkGuard(RegCache& rc_) : rc(&rc_), m_regs(rc_.m_regs), m_xregs(rc_.m_xregs)
{
  ASSERT(!rc->IsAnyConstraintActive());
}

RCForkGuard::RCForkGuard(RCForkGuard&& other) noexcept
    : rc(std::exchange(other.rc, nullptr)), m_regs(other.m_regs), m_xregs(other.m_xregs)
{
}

void RCForkGuard::EndFork()
{
  if (rc == nullptr)
    return;

  ASSERT(!rc->IsAnyConstraintActive());
  rc->m_xregs = m_xregs;
  rc->m_regs = m_regs;
  rc = nullptr;
}

// ---------------------------------------------------------------------------
// RegCache: public interface

RegCache::RegCache(Jit64& jit) : m_jit{jit}
{
}

void RegCache::Start()
{
  m_xregs.fill(X64CachedReg{});
  for (preg_t preg = 0; preg < m_regs.size(); ++preg)
    m_regs[preg] = PPCCachedReg{GetDefaultLocation(preg)};
}

void RegCache::SetEmitter(XEmitter* emitter)
{
  m_emitter = emitter;
}

bool RegCache::SanityCheck() const
{
  for (preg_t preg = 0; preg < m_regs.size(); ++preg)
  {
    const PPCCachedReg& reg = m_regs[preg];
    if (!reg.IsInHostRegister())
      continue;

    // Between instructions nothing may be locked or mid-transaction, and the
    // host side must agree about the binding.
    const X64CachedReg& host = m_xregs[reg.GetHostRegister()];
    const bool consistent = !reg.IsLocked() && !reg.IsRevertable() && !host.IsLocked() &&
                            host.Contents() == preg;
    if (!consistent)
      return false;
  }
  return true;
}

RCOpArg RegCache::Use(preg_t guest, RCMode mode)
{
  m_constraints[guest].AddUse(mode);
  return RCOpArg{this, guest};
}

RCOpArg RegCache::UseNoImm(preg_t guest, RCMode mode)
{
  m_constraints[guest].AddUseNoImm(mode);
  return RCOpArg{this, guest};
}

RCOpArg RegCache::BindOrImm(preg_t guest, RCMode mode)
{
  m_constraints[guest].AddBindOrImm(mode);
  return RCOpArg{this, guest};
}

RCX64Reg RegCache::Bind(preg_t guest, RCMode mode)
{
  m_constraints[guest].AddBind(mode);
  return RCX64Reg{this, guest};
}

RCX64Reg RegCache::RevertableBind(preg_t guest, RCMode mode)
{
  m_constraints[guest].AddRevertableBind(mode);
  return RCX64Reg{this, guest};
}

RCX64Reg RegCache::Scratch()
{
  const X64Reg xr = GetFreeXReg();
  return Scratch(xr);
}

RCX64Reg RegCache::Scratch(X64Reg xr)
{
  FlushX(xr);
  return RCX64Reg{this, xr};
}

RCForkGuard RegCache::Fork()
{
  return RCForkGuard{*this};
}

void RegCache::AssertNothingLocked(const char* operation) const
{
  const bool any_host_locked = std::ranges::any_of(m_xregs, &X64CachedReg::IsLocked);
  ASSERT_MSG(DYNA_REC, !any_host_locked, "{}: a host register is still locked", operation);
}

void RegCache::Discard(BitSet32 pregs)
{
  AssertNothingLocked("Discard");
  for (const int index : pregs)
  {
    const auto preg = static_cast<preg_t>(index);
    ASSERT_MSG(DYNA_REC, !m_regs[preg].IsLocked(), "Discard: guest register {} is still locked",
               preg);
    ASSERT_MSG(DYNA_REC, !m_regs[preg].IsRevertable(),
               "Discard: guest register {} is inside a transaction", preg);
    DiscardRegister(preg);
  }
}

void RegCache::Flush(BitSet32 pregs, FlushMode mode,
                     IgnoreDiscardedRegisters ignore_discarded_registers)
{
  AssertNothingLocked("Flush");
  for (const int index : pregs)
  {
    const auto preg = static_cast<preg_t>(index);
    // An Undirty flush keeps bindings, so it is allowed while handles exist.
    ASSERT_MSG(DYNA_REC, mode != FlushMode::Full || !m_regs[preg].IsLocked(),
               "Flush: guest register {} is still locked", preg);
    ASSERT_MSG(DYNA_REC, !m_regs[preg].IsRevertable(),
               "Flush: guest register {} is inside a transaction", preg);
    StoreFromRegister(preg, mode, ignore_discarded_registers);
  }
}

void RegCache::Reset(BitSet32 pregs)
{
  for (const int index : pregs)
  {
    const auto preg = static_cast<preg_t>(index);
    ASSERT_MSG(DYNA_REC, !m_regs[preg].IsInHostRegister(),
               "Reset: guest register {} is bound; it needs a flush instead", preg);
    m_regs[preg].SetFlushed(false);
  }
}

void RegCache::Revert()
{
  ASSERT(IsAllUnlocked());
  for (PPCCachedReg& reg : m_regs)
  {
    if (reg.IsRevertable())
      reg.SetRevert();
  }
}

void RegCache::Commit()
{
  ASSERT(IsAllUnlocked());
  for (PPCCachedReg& reg : m_regs)
  {
    if (reg.IsRevertable())
      reg.SetCommit();
  }
}

bool RegCache::IsAllUnlocked() const
{
  if (std::ranges::any_of(m_regs, &PPCCachedReg::IsLocked))
    return false;
  if (std::ranges::any_of(m_xregs, &X64CachedReg::IsLocked))
    return false;
  return !IsAnyConstraintActive();
}

void RegCache::PreloadRegisters(BitSet32 to_preload)
{
  for (const int index : to_preload)
  {
    const auto preg = static_cast<preg_t>(index);
    // Leave room for the instruction's own scratch needs.
    if (NumFreeRegisters() < 2)
      return;
    if (!IsImm(preg))
      BindToRegister(preg, true, false);
  }
}

BitSet32 RegCache::RegistersInUse() const
{
  BitSet32 in_use;
  for (size_t index = 0; index < m_xregs.size(); ++index)
  {
    if (!m_xregs[index].IsFree())
      in_use[index] = true;
  }
  return in_use;
}

// ---------------------------------------------------------------------------
// RegCache: allocation

void RegCache::FlushX(X64Reg reg)
{
  ASSERT_MSG(DYNA_REC, reg < m_xregs.size(), "FlushX: no host register {}",
             std::to_underlying(reg));
  const X64CachedReg& host = m_xregs[reg];
  ASSERT(!host.IsLocked());
  if (!host.IsFree())
    StoreFromRegister(host.Contents());
}

void RegCache::DiscardRegister(preg_t preg)
{
  PPCCachedReg& reg = m_regs[preg];
  if (reg.IsInHostRegister())
    m_xregs[reg.GetHostRegister()].Unbind();
  reg.SetDiscarded();
}

void RegCache::BindToRegister(preg_t preg, bool doLoad, bool makeDirty)
{
  PPCCachedReg& reg = m_regs[preg];
  if (reg.IsInHostRegister())
  {
    if (makeDirty)
      reg.SetDirty();
  }
  else
  {
    const X64Reg xr = GetFreeXReg();
    ASSERT_MSG(DYNA_REC, !m_xregs[xr].IsLocked(), "BindToRegister: allocator returned a locked reg");
    ASSERT_MSG(DYNA_REC, !reg.IsRevertable(), "BindToRegister: unexpected transaction state");

    m_xregs[xr].SetBoundTo(preg);
    if (doLoad)
      LoadRegister(preg, xr);

    const bool already_used = std::ranges::any_of(m_regs, [xr](const PPCCachedReg& other) {
      return other.IsInHostRegister() && other.GetHostRegister() == xr;
    });
    ASSERT_MSG(DYNA_REC, !already_used, "BindToRegister: host register {} is already bound",
               std::to_underlying(xr));

    reg.SetInHostRegister(xr, makeDirty);
  }

  // A register that is about to be written no longer has a known constant.
  if (makeDirty)
    DiscardImm(preg);

  ASSERT_MSG(DYNA_REC, !m_xregs[RX(preg)].IsLocked(),
             "BindToRegister: guest register {} landed in locked host register {}", preg,
             std::to_underlying(RX(preg)));
}

void RegCache::StoreFromRegister(preg_t preg, FlushMode mode,
                                 IgnoreDiscardedRegisters ignore_discarded_registers)
{
  PPCCachedReg& reg = m_regs[preg];
  // Writing back mid-transaction would destroy the value a revert restores.
  ASSERT_MSG(DYNA_REC, !reg.IsRevertable(), "StoreFromRegister: {} is inside a transaction",
             preg);

  if (!reg.IsInDefaultLocation())
    StoreRegister(preg, GetDefaultLocation(preg), ignore_discarded_registers);

  const bool release = mode == FlushMode::Full;
  if (release && reg.IsInHostRegister())
    m_xregs[reg.GetHostRegister()].Unbind();
  reg.SetFlushed(!release);
}

X64Reg RegCache::GetFreeXReg()
{
  const std::span<const X64Reg> order = GetAllocationOrder();
  const auto free_reg =
      std::ranges::find_if(order, [this](X64Reg xr) { return m_xregs[xr].IsFree(); });
  if (free_reg != order.end())
    return *free_reg;
  return EvictForAllocation(order);
}

X64Reg RegCache::EvictForAllocation(std::span<const X64Reg> order)
{
  // Evict the cheapest unlocked binding; ties go to the earlier register in
  // allocation order.
  X64Reg victim = INVALID_REG;
  preg_t victim_preg = 0;
  float victim_score = std::numeric_limits<float>::max();
  for (const X64Reg xr : order)
  {
    if (m_xregs[xr].IsLocked())
      continue;
    const preg_t preg = m_xregs[xr].Contents();
    if (m_regs[preg].IsLocked())
      continue;

    const float score = ScoreRegister(xr);
    if (score < victim_score)
    {
      victim_score = score;
      victim = xr;
      victim_preg = preg;
    }
  }

  if (victim == INVALID_REG)
  {
    ASSERT_MSG(DYNA_REC, false, "Register cache has no register left to evict");
    return INVALID_REG;
  }

  StoreFromRegister(victim_preg);
  return victim;
}

int RegCache::NumFreeRegisters() const
{
  const std::span<const X64Reg> order = GetAllocationOrder();
  const auto free_count =
      std::ranges::count_if(order, [this](X64Reg xr) { return m_xregs[xr].IsFree(); });
  return static_cast<int>(free_count);
}

// Cost of evicting the guest register held in `xreg`; higher is worse.
float RegCache::ScoreRegister(X64Reg xreg) const
{
  const preg_t preg = m_xregs[xreg].Contents();
  float score = 0;

  // A dirty register costs a store. A weight of 2 balances extra stores
  // against extra reloads.
  if (!m_regs[preg].IsInDefaultLocation())
    score += 2;

  // Registers the rest of the block still reads are worth more, the more so
  // the sooner they are needed: count how many other registers are read
  // before this one, looking at most 64 instructions ahead.
  if (GetRegUtilization()[preg])
  {
    const u32 lookahead = static_cast<u32>(std::min(m_jit.js.instructionsLeft, 64));
    const u32 others_first = CountRegsIn(preg, lookahead).Count();
    score += 1 + 2 * (5 - std::log2(1 + static_cast<float>(others_first)));
  }

  return score;
}

X64Reg RegCache::RX(preg_t preg) const
{
  ASSERT_MSG(DYNA_REC, m_regs[preg].IsInHostRegister(), "Guest register {} is not bound", preg);
  return m_regs[preg].GetHostRegister();
}

// ---------------------------------------------------------------------------
// RegCache: locking and realization

void RegCache::Lock(preg_t preg)
{
  m_regs[preg].Lock();
}

void RegCache::Unlock(preg_t preg)
{
  m_regs[preg].Unlock();
  // The last handle is gone: the instruction is done with this register.
  if (!m_regs[preg].IsLocked())
    m_constraints[preg] = RCConstraint{};
}

void RegCache::LockX(X64Reg xr)
{
  m_xregs[xr].Lock();
}

void RegCache::UnlockX(X64Reg xr)
{
  m_xregs[xr].Unlock();
}

bool RegCache::IsRealized(preg_t preg) const
{
  return m_constraints[preg].IsRealized();
}

void RegCache::Realize(preg_t preg)
{
  RCConstraint& constraint = m_constraints[preg];
  if (constraint.IsRealized())
    return;

  const bool load = constraint.ShouldLoad();
  const bool dirty = constraint.ShouldDirty();
  const auto bind = [&] {
    BindToRegister(preg, load, dirty);
    constraint.Realized(RCConstraint::RealizedLoc::Bound);
  };

  if (constraint.ShouldBeRevertable())
  {
    // The home slot must hold the pre-transaction value.
    StoreFromRegister(preg, FlushMode::Undirty);
    bind();
    m_regs[preg].SetRevertable();
    return;
  }

  if (IsImm(preg))
  {
    if (dirty || constraint.ShouldKillImmediate())
      bind();
    else
      constraint.Realized(RCConstraint::RealizedLoc::Imm);
    return;
  }

  if (!m_regs[preg].IsInHostRegister() && !constraint.ShouldKillMemory())
  {
    constraint.Realized(RCConstraint::RealizedLoc::Mem);
    return;
  }

  bind();
}

bool RegCache::IsAnyConstraintActive() const
{
  return std::ranges::any_of(m_constraints, &RCConstraint::IsActive);
}
