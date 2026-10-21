// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QColor>
#include <QString>
#include <QStyledItemDelegate>
#include <QWidget>

#include "Common/CommonTypes.h"

class QFont;
class QPoint;
class QScrollBar;

namespace AddressSpace
{
enum class Type;
}

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

class PPCSymbolDB;

// Reports what the user typed into a table cell instead of storing it in the model, so the
// memory view can write it to emulated memory itself.
class TableEditDelegate : public QStyledItemDelegate
{
  Q_OBJECT

public:
  explicit TableEditDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

  void setModelData(QWidget* editor, QAbstractItemModel* model,
                    const QModelIndex& index) const override;

signals:
  void editFinished(const int row, const int column, const QString& text) const;
};

// A hex-editor style view of emulated memory. The view has no ends: it scrolls around the whole
// 32-bit address space, so it uses its own always-centred scroll bar.
class MemoryViewWidget final : public QWidget
{
  Q_OBJECT
public:
  enum class Type : int
  {
    Null = 0,
    Hex8 = 1,
    Hex16,
    Hex32,
    Hex64,
    HexString,
    Unsigned8,
    Unsigned16,
    Unsigned32,
    Signed8,
    Signed16,
    Signed32,
    ASCII,
    Float32,
    Double
  };

  enum class BPType
  {
    ReadWrite,
    ReadOnly,
    WriteOnly
  };

  enum class UpdateType
  {
    Full,
    Addresses,
    Values,
    Symbols,
    Auto,
  };

  enum class EditSymbolType
  {
    AddNote,
    EditNote,
    EditRegion,
  };

  explicit MemoryViewWidget(Core::System& system, QWidget* parent = nullptr);

  void CreateTable();
  void UpdateDispatcher(UpdateType type = UpdateType::Addresses);
  void Update();
  void UpdateSymbols();
  void UpdateOnFrameEnd();
  void GetValues();
  void UpdateFont(const QFont& font);
  void ToggleBreakpoint(u32 addr, bool row);

  std::vector<u8> ConvertTextToBytes(Type type, QStringView input_text) const;
  void SetAddressSpace(AddressSpace::Type address_space);
  AddressSpace::Type GetAddressSpace() const;
  void SetDisplay(Type type, int bytes_per_row, int alignment, bool dual_view);
  void ToggleHighlights(bool enabled);
  void SetHighlightColor();
  void SetBPType(BPType type);
  void SetAddress(u32 address);
  void SetFocus() const;
  void ShowSymbols(bool enable);

  void SetBPLoggingEnabled(bool enabled);

signals:
  void AutoUpdate();
  void ShowCode(u32 address);
  void RequestWatch(QString name, u32 address);
  void ActivateSearch();

private:
  class Grid;
  using ValueMap = std::map<u32, std::optional<QString>>;

  void ScrollRows(s64 rows);
  void WriteTypedText(int row, int column, const QString& text);
  void ShowCellMenu(const QPoint& pos);
  void EditLabel(EditSymbolType kind, u32 address);
  void CopyAddress(u32 address) const;
  void CopyHex(u32 address) const;
  void RefreshBreakpointMarks();
  void RefreshValueCells();
  void ScrollBarAction();
  void RecentreScrollBar();
  [[nodiscard]] int DataSpan() const;
  [[nodiscard]] std::optional<Type> DualViewType() const;

  std::optional<QString> ValueToString(const Core::CPUThreadGuard& guard, u32 address, Type type);

  Core::System& m_system;
  PPCSymbolDB& m_symbol_db;

  Grid* m_grid = nullptr;
  QScrollBar* m_scroll = nullptr;

  AddressSpace::Type m_address_space{};
  Type m_type = Type::Hex32;
  BPType m_bp_type = BPType::ReadWrite;
  bool m_log_hits = true;
  bool m_dual_view = false;
  bool m_show_symbols = true;

  u32 m_address = 0x80000000;
  u32 m_highlight_address = 0;
  // First address shown and one past the last.
  std::pair<u32, u32> m_shown_range{};
  int m_bytes_per_row = 16;
  int m_alignment = 16;
  int m_value_columns = 0;

  int m_char_width = 0;
  int m_line_height = 0;
  // Alpha 100 while change highlighting is on, 0 while it is off.
  QColor m_highlight = QColor(120, 255, 255, 100);

  ValueMap m_values;
  ValueMap m_dual_values;
  // Held while the table or the captured values are being changed; the CPU thread only tries it.
  std::mutex m_busy;
};
