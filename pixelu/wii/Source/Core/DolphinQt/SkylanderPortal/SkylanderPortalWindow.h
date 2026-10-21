// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <utility>

#include <QBrush>
#include <QFrame>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "Core/Core.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"

class QCheckBox;
class QFileInfo;
class QGroupBox;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QListWidget;

using Element = IOS::HLE::USB::Element;
using Game = IOS::HLE::USB::Game;
using Type = IOS::HLE::USB::Type;

// The element filter has an extra "All" choice in front of the real elements.
constexpr u8 NUM_SKYLANDER_ELEMENTS_RADIO = NUM_SKYLANDER_ELEMENTS + 1;

// What the window remembers about a figure it put on the portal.
struct Skylander
{
  u8 portal_slot;
  u16 m_sky_id;
  u16 m_sky_var;
};

// Manager for the emulated Skylanders portal: which figure file sits in each of the sixteen
// slots, plus a filterable list of known figures (or of the files in a collection folder) to
// load, create or edit.
class SkylanderPortalWindow : public QWidget
{
  Q_OBJECT
public:
  explicit SkylanderPortalWindow(QWidget* parent = nullptr);
  ~SkylanderPortalWindow() override;

  void RefreshList();

protected:
  std::array<QLineEdit*, MAX_SKYLANDERS> m_edit_skylanders;
  std::array<std::optional<Skylander>, MAX_SKYLANDERS> m_sky_slots;

private:
  // Building the window.
  void BuildLayout();
  QVBoxLayout* BuildSlotColumn();
  QVBoxLayout* BuildFinderColumn();
  void SetUpCollectionFolder();
  void closeEvent(QCloseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) final;

  // Button and widget handlers.
  void EmulatePortal(bool emulate);
  void SelectCollectionPath();
  void LoadSelected();
  void LoadFromFile();
  void ClearSlot(u8 slot);
  void CreateSkylanderAdvanced();
  void ModifySkylander();

  // State updates.
  void OnEmulationStateChanged(Core::State state);
  void OnCollectionPathChanged();
  void UpdateCurrentIDs();
  void CreateSkyfile(const QString& path, bool load_after);
  void LoadSkyfilePath(u8 slot, const QString& path);
  void UpdateSlotNames();

  // Queries.
  bool PassesFilter(const QString& name, u16 id, u16 var) const;
  QString GetFilePath(u16 id, u16 var) const;
  QString SuggestedFileName(const QString& folder) const;
  // Calls visit with each readable figure file in the collection folder and its IDs, until it
  // returns false.
  using CollectionVisitor = std::function<bool(const QFileInfo&, std::pair<u16, u16>)>;
  void ScanCollection(const CollectionVisitor& visit) const;
  void AddListEntry(const QString& name, std::pair<const u16, const u16> ids, bool dark_theme);
  u8 GetCurrentSlot() const;
  int GetElementRadio() const;
  int GetTypeRadio() const;
  static QBrush GetBaseColor(std::pair<const u16, const u16> ids, bool dark_theme);

  bool m_emulating;
  QCheckBox* m_enabled_checkbox;
  QFrame* m_group_skylanders;
  QFrame* m_command_buttons;
  std::array<QRadioButton*, MAX_SKYLANDERS> m_slot_radios;

  // Folder of the figure file used last, with a trailing separator. Native file pickers don't
  // always remember it themselves.
  QString m_last_skylander_path;

  QString m_collection_path;
  QLineEdit* m_path_edit;
  QPushButton* m_path_select;

  std::array<QCheckBox*, NUM_SKYLANDER_GAMES> m_game_filters;
  std::array<QRadioButton*, NUM_SKYLANDER_ELEMENTS_RADIO> m_element_filter;
  std::array<QRadioButton*, NUM_SKYLANDER_TYPES> m_type_filter;
  QCheckBox* m_only_show_collection;
  QLineEdit* m_sky_search;
  QListWidget* m_skylander_list;
  // The figure picked in the list (or typed into the Customize dialog).
  u16 m_sky_id = 0;
  u16 m_sky_var = 0;
};
