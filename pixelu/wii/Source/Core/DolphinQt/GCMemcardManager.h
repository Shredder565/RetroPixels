// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <span>
#include <vector>

#include <QDialog>

#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Core/HW/EXI/EXI.h"

namespace Memcard
{
class GCMemcard;
class GCMemcardErrorCode;
struct Savefile;
enum class ReadSavefileErrorCode;
enum class SavefileFormat;
}  // namespace Memcard

class QAction;
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPixmap;
class QPushButton;
class QString;
class QTableWidget;
class QTimer;
class QToolButton;

// Tools > Memory Card Manager: two memory card images side by side, with copying, importing,
// exporting and deleting of saves, and checksum repair.
class GCMemcardManager : public QDialog
{
  Q_OBJECT
public:
  explicit GCMemcardManager(QWidget* parent = nullptr);
  ~GCMemcardManager() override;

  static QString GetErrorMessagesForErrorCode(const Memcard::GCMemcardErrorCode& code);
  static QString GetErrorMessageForErrorCode(Memcard::ReadSavefileErrorCode code);

private:
  // A save's icon: its frames, and which frame shows at each tick of the 15 Hz animation timer.
  struct AnimatedIcon;

  template <typename T>
  using PerSlot = Common::EnumMap<T, ExpansionInterface::MAX_MEMCARD_SLOT>;

  void BuildWidgets();
  void ConnectWidgets();
  void OpenConfiguredCards();

  void UpdateButtons();
  void FillTable(ExpansionInterface::Slot slot);
  void OpenCard(ExpansionInterface::Slot slot, const QString& path);
  void BrowseForCard(ExpansionInterface::Slot slot);
  void SelectSlot(ExpansionInterface::Slot slot);

  std::vector<u8> SelectedSaves();

  void ImportInto(ExpansionInterface::Slot slot, std::span<const Memcard::Savefile> saves);

  void CopyToOtherSlot();
  void ImportFromFiles();
  void DeleteSaves();
  void ExportSaves(Memcard::SavefileFormat format);
  void FixChecksums();
  void CreateCard(ExpansionInterface::Slot slot);
  void AnimateIcons();

  QPixmap BannerOf(u8 file_index, ExpansionInterface::Slot slot);
  AnimatedIcon IconOf(u8 file_index, ExpansionInterface::Slot slot);

  // Buttons between the two slots.
  QPushButton* m_switch_button = nullptr;
  QPushButton* m_copy_button = nullptr;
  QToolButton* m_export_button = nullptr;
  QMenu* m_export_menu = nullptr;
  QAction* m_export_gci = nullptr;
  QAction* m_export_gcs = nullptr;
  QAction* m_export_sav = nullptr;
  QPushButton* m_import_button = nullptr;
  QPushButton* m_delete_button = nullptr;
  QPushButton* m_fix_button = nullptr;

  // Per slot. The icons are keyed by directory index.
  PerSlot<std::map<u8, AnimatedIcon>> m_icons;
  PerSlot<std::unique_ptr<Memcard::GCMemcard>> m_cards;
  PerSlot<QGroupBox*> m_groups;
  PerSlot<QLineEdit*> m_paths;
  PerSlot<QPushButton*> m_open_buttons;
  PerSlot<QPushButton*> m_create_buttons;
  PerSlot<QTableWidget*> m_tables;
  PerSlot<QLabel*> m_free_space;

  ExpansionInterface::Slot m_active_slot = ExpansionInterface::Slot::A;
  u64 m_tick = 0;

  QDialogButtonBox* m_close_box = nullptr;
  QTimer* m_animation_timer = nullptr;
};
