// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <QTableWidget>

#include "Common/CommonTypes.h"
#include "Core/Debugger/CodeTrace.h"

class QFont;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;
class QShowEvent;

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

class PPCSymbolDB;

// Disassembly listing centred on an address, one instruction per row, with breakpoint icons,
// symbol/note columns and a lane of arrows showing where branches go.
class CodeViewWidget : public QTableWidget
{
  Q_OBJECT
public:
  enum class SetAddressUpdate
  {
    WithUpdate,
    WithoutUpdate,
    WithDetailedUpdate
  };

  explicit CodeViewWidget();
  ~CodeViewWidget() override;

  u32 GetAddress() const;
  void OnLockAddress(bool lock);
  u32 GetContextAddress() const;
  void SetAddress(u32 address, SetAddressUpdate update);

  // Recomputes row height and column widths from the current font. Call after a font change.
  void FontBasedSizing();
  void Update();
  void Update(const Core::CPUThreadGuard* guard);

  void ToggleBreakpoint();
  void AddBreakpoint();

  u32 AddressForRow(int row) const;

signals:
  void RequestPPCComparison(u32 address, bool translate_address);
  void ShowMemory(u32 address);
  void UpdateCodeWidget();
  void ActivateSearch();

private:
  // One branch to draw in the arrow column. lane is the horizontal slot it was given.
  struct Arrow
  {
    u32 from = 0;
    u32 to = 0;
    u32 lane = 0;
    bool links = false;
  };
  class ArrowDelegate;

  struct RowStyle;

  void resizeEvent(QResizeEvent*) override;
  void keyPressEvent(QKeyEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void showEvent(QShowEvent* event) override;

  void Scroll(s64 instructions);
  void FollowPCUnlessLocked();
  void FillRow(int row, const Core::CPUThreadGuard* guard, const RowStyle& style);
  void AssignArrowLanes();
  void MarkPCSelection();
  void ApplyDebugFont(const QFont& new_font);

  void ShowContextMenu();
  void RunUntil(CodeTrace::AutoStop condition);
  void FollowBranch();
  void CopyAddress();
  void CopyLine();
  void CopyHex();
  void CopyFunction();
  void CopyTarget();
  void ShowTarget();
  [[nodiscard]] std::optional<u32> ContextTarget() const;
  [[nodiscard]] std::string DisassembleContext() const;

  void AddFunctionSymbol();
  void EditFunctionSymbol();
  void DeleteFunctionSymbol();
  void AddNote();
  void EditNote();
  void DeleteNote();

  void WriteStub(u32 instruction);
  void EditInstruction(bool assemble);
  void RestoreInstruction();

  Core::System& m_system;
  PPCSymbolDB& m_symbol_db;

  u32 m_address = 0;
  u32 m_context_address = 0;
  bool m_lock_address = false;
  bool m_in_update = false;

  std::vector<Arrow> m_arrows;
};
