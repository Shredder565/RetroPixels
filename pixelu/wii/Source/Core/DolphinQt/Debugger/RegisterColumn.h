// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QTableWidgetItem>

#include <functional>

#include "Common/CommonTypes.h"

// What a register cell holds; stored in the cell under DATA_TYPE.
enum class RegisterType
{
  gpr,         // r0-r31
  fpr,         // f0-f31 (paired-single slots shown as 64-bit)
  ibat,        // IBAT0-7, upper and lower halves together
  dbat,        // DBAT0-7, upper and lower halves together
  tb,          // time base
  pc,          // program counter
  lr,          // link register
  ctr,         // count register
  cr,          // condition register
  xer,         // fixed-point exception register
  fpscr,       // floating-point status and control
  msr,         // machine state
  srr,         // SRR0/SRR1
  sr,          // SR0-15
  gqr,         // GQR0-7
  hid,         // HID0, HID1, HID2, HID4
  exceptions,  // pending exception flags
  int_mask,    // processor interface interrupt mask
  int_cause,   // processor interface interrupt cause
  dsisr,       // DSISR
  dar,         // DAR
  pt_hashmask  // page table hash mask
};

// How a cell renders its value. The numbers are used as indexes by the register view's menu.
enum class RegisterDisplay : int
{
  Hex = 0,
  SInt32,
  UInt32,
  Float,
  Double
};

constexpr int DATA_TYPE = Qt::UserRole;

// One editable table cell bound to a register through a getter and an optional setter.
class RegisterColumn : public QTableWidgetItem
{
public:
  explicit RegisterColumn(RegisterType type, std::function<u64()> get,
                          std::function<void(u64)> set);

  void RefreshValue();

  RegisterDisplay GetDisplay() const;
  void SetDisplay(RegisterDisplay display);
  u64 GetValue() const;
  void SetValue();

private:
  void Redraw();
  int HexDigits() const;

  RegisterType m_kind;
  std::function<u64()> m_read;
  std::function<void(u64)> m_write;

  u64 m_shown = 0;
  RegisterDisplay m_format = RegisterDisplay::Hex;
};
