// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64/RegCache/RCMode.h"

// Index of a guest register within one register cache (GPR or FPR number).
using preg_t = size_t;

// Where one guest register currently lives. A register is in its home slot
// in ppcState, in a host register, or both (in which case the host copy is
// clean). Neither means its value was discarded.
class PPCCachedReg
{
public:
  PPCCachedReg() = default;
  explicit PPCCachedReg(Gen::OpArg default_location) : m_home(default_location) {}

  Gen::OpArg GetDefaultLocation() const { return m_home; }

  Gen::X64Reg GetHostRegister() const
  {
    ASSERT(m_bound);
    return m_host;
  }

  bool IsInDefaultLocation() const { return m_home_valid; }
  bool IsInHostRegister() const { return m_bound; }

  // The home slot is up to date; optionally keep the host copy.
  void SetFlushed(bool maintain_host_register)
  {
    ASSERT(!m_revertable);
    m_home_valid = true;
    if (!maintain_host_register)
      m_bound = false;
  }

  void SetInHostRegister(Gen::X64Reg xreg, bool dirty)
  {
    m_host = xreg;
    m_bound = true;
    if (dirty)
      m_home_valid = false;
  }

  void SetDirty() { m_home_valid = false; }

  void SetDiscarded()
  {
    ASSERT(!m_revertable);
    m_bound = false;
    m_home_valid = false;
  }

  // Revertable registers back a transaction: the home slot keeps the old
  // value until the transaction is committed or rolled back.
  bool IsRevertable() const { return m_revertable; }
  void SetRevertable()
  {
    ASSERT(m_bound);
    m_revertable = true;
  }
  void SetRevert()
  {
    ASSERT(m_revertable);
    m_revertable = false;
    SetFlushed(false);
  }
  void SetCommit()
  {
    ASSERT(m_revertable);
    m_revertable = false;
  }

  bool IsLocked() const { return m_lock_count != 0; }
  void Lock() { ++m_lock_count; }
  void Unlock()
  {
    ASSERT(m_lock_count != 0);
    --m_lock_count;
  }

private:
  Gen::OpArg m_home{};
  Gen::X64Reg m_host{};
  bool m_home_valid = true;
  bool m_bound = false;
  bool m_revertable = false;
  size_t m_lock_count = 0;
};

// Which guest register, if any, a host register holds.
class X64CachedReg
{
public:
  static constexpr preg_t NO_PREG = static_cast<preg_t>(Gen::INVALID_REG);

  preg_t Contents() const { return m_preg; }

  void SetBoundTo(preg_t preg)
  {
    m_preg = preg;
    m_bound = true;
  }

  void Unbind()
  {
    m_preg = NO_PREG;
    m_bound = false;
  }

  // Free for allocation: holds nothing and is not reserved as scratch.
  bool IsFree() const { return !m_bound && m_lock_count == 0; }

  bool IsLocked() const { return m_lock_count != 0; }
  void Lock() { ++m_lock_count; }
  void Unlock()
  {
    ASSERT(m_lock_count != 0);
    --m_lock_count;
  }

private:
  preg_t m_preg = NO_PREG;
  bool m_bound = false;
  size_t m_lock_count = 0;
};

// The requirements that the RCOpArg/RCX64Reg handles for one guest register
// have placed on it during the current instruction, and how they were met.
// Requirements accumulate until the register is realized; later requests
// must then be compatible with the realized location.
class RCConstraint
{
public:
  enum class RealizedLoc
  {
    Invalid,
    Bound,
    Imm,
    Mem,
  };

  // Locations a request accepts.
  enum class ConstraintLoc
  {
    Bound,
    BoundOrImm,
    BoundOrMem,
    Any,
  };

  bool IsRealized() const { return m_realized != RealizedLoc::Invalid; }
  bool IsActive() const { return IsRealized() || m_flags != 0; }

  bool ShouldLoad() const { return Has(READ); }
  bool ShouldDirty() const { return Has(WRITE); }
  bool ShouldBeRevertable() const { return Has(REVERTABLE); }
  bool ShouldKillImmediate() const { return Has(NOT_IMM); }
  bool ShouldKillMemory() const { return Has(NOT_MEM); }

  void Realized(RealizedLoc loc)
  {
    m_realized = loc;
    ASSERT(IsRealized());
  }

  void AddUse(RCMode mode) { Add(mode, ConstraintLoc::Any, false); }
  void AddUseNoImm(RCMode mode) { Add(mode, ConstraintLoc::BoundOrMem, false); }
  void AddBindOrImm(RCMode mode) { Add(mode, ConstraintLoc::BoundOrImm, false); }
  void AddBind(RCMode mode) { Add(mode, ConstraintLoc::Bound, false); }
  void AddRevertableBind(RCMode mode) { Add(mode, ConstraintLoc::Bound, true); }

private:
  enum : u8
  {
    READ = 1 << 0,
    WRITE = 1 << 1,
    NOT_IMM = 1 << 2,
    NOT_MEM = 1 << 3,
    REVERTABLE = 1 << 4,
  };

  bool Has(u8 flag) const { return (m_flags & flag) != 0; }

  static u8 ModeFlags(RCMode mode)
  {
    switch (mode)
    {
    case RCMode::Read:
      return READ;
    case RCMode::Write:
      return WRITE;
    case RCMode::ReadWrite:
      return READ | WRITE;
    }
    return 0;
  }

  static u8 LocationFlags(ConstraintLoc loc)
  {
    switch (loc)
    {
    case ConstraintLoc::Bound:
      return NOT_IMM | NOT_MEM;
    case ConstraintLoc::BoundOrImm:
      return NOT_MEM;
    case ConstraintLoc::BoundOrMem:
      return NOT_IMM;
    case ConstraintLoc::Any:
      return 0;
    }
    return 0;
  }

  bool Accepts(RealizedLoc where, ConstraintLoc loc) const
  {
    switch (loc)
    {
    case ConstraintLoc::Bound:
      return where == RealizedLoc::Bound;
    case ConstraintLoc::BoundOrImm:
      return where == RealizedLoc::Bound || where == RealizedLoc::Imm;
    case ConstraintLoc::BoundOrMem:
      return where == RealizedLoc::Bound || where == RealizedLoc::Mem;
    case ConstraintLoc::Any:
      return true;
    }
    return false;
  }

  void Add(RCMode mode, ConstraintLoc loc, bool revertable)
  {
    const u8 mode_flags = ModeFlags(mode);
    if (IsRealized())
    {
      // Too late to change anything: the request must already be satisfied.
      ASSERT((!revertable || Has(REVERTABLE)) && Accepts(m_realized, loc) &&
             (m_flags & mode_flags) == mode_flags);
      return;
    }

    m_flags = static_cast<u8>(m_flags | mode_flags | LocationFlags(loc) |
                              (revertable ? REVERTABLE : 0));
  }

  RealizedLoc m_realized = RealizedLoc::Invalid;
  u8 m_flags = 0;
};
