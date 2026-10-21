// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <string>
#include <string_view>

#include <QWidget>

#include "Common/EnumMap.h"
#include "Core/HW/EXI/EXI.h"
#include "DolphinQt/MainWindow.h"

class ConfigBool;
class ConfigChoice;
class ConfigText;
class ConfigUserPath;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QString;
class QVBoxLayout;

// "GameCube" settings page: IPL options, the EXI devices in slots A/B/SP1 (with the
// memory card / GCI folder / GBA cartridge paths of slots A and B) and, with mGBA, the GBA
// BIOS, ROM and save locations.
class GameCubePane : public QWidget
{
  Q_OBJECT
public:
  explicit GameCubePane(MainWindow* main_window);

  // Asks for a GBA ROM; returns an empty string if cancelled.
  static std::string GetOpenGBARom(std::string_view title);

signals:
  void ShowTriforceWindow();

private:
  using Slot = ExpansionInterface::Slot;

  // Path editors shown under a memory card slot, depending on the selected device.
  struct CardSlotPaths
  {
    QLabel* memcard_label = nullptr;
    QLineEdit* memcard = nullptr;
    QLabel* cartridge_label = nullptr;
    QLineEdit* cartridge = nullptr;
    QLabel* gci_label = nullptr;
    QLabel* gci_override_warning = nullptr;
    QLineEdit* gci = nullptr;
  };

  QWidget* BuildIplBox();
  QWidget* BuildDeviceBox();
  void BuildCardSlotPaths(Slot slot);
  void FillDeviceCombos();
#ifdef HAS_LIBMGBA
  QWidget* BuildGbaBox();
#endif
  void WireUp();

  void ReadConfig();
  void WriteDevices();

  void OnEmulationStateChanged();

  void RefreshSlot(Slot slot);
  void ConfigureSlot(Slot slot);

  void PickMemcard(Slot slot);
  bool ApplyMemcard(Slot slot, const QString& filename);
  void PickGCIFolder(Slot slot);
  bool ApplyGCIFolder(Slot slot, const QString& path);
  void PickCartridge(Slot slot);
  void ApplyCartridge(Slot slot, const QString& filename);

#ifdef HAS_LIBMGBA
  void PickGBABios();
  void PickGBARom(size_t index);
  void OnSavesBesideRomToggled();
  void PickGBASaves();
#endif  // HAS_LIBMGBA

  ConfigBool* m_skip_ipl;
  ConfigChoice* m_ipl_language;

  Common::EnumMap<QComboBox*, Slot::SP1> m_device_combos;
  Common::EnumMap<QPushButton*, Slot::SP1> m_configure_buttons;
  Common::EnumMap<QHBoxLayout*, ExpansionInterface::MAX_MEMCARD_SLOT> m_memcard_rows;
  Common::EnumMap<QHBoxLayout*, ExpansionInterface::MAX_MEMCARD_SLOT> m_cartridge_rows;
  Common::EnumMap<QVBoxLayout*, ExpansionInterface::MAX_MEMCARD_SLOT> m_gci_rows;
  Common::EnumMap<CardSlotPaths, ExpansionInterface::MAX_MEMCARD_SLOT> m_paths;

  ConfigUserPath* m_gba_bios;
  QPushButton* m_gba_bios_browse;
  std::array<ConfigText*, 5> m_gba_roms;
  std::array<QPushButton*, 5> m_gba_rom_browse;
  ConfigBool* m_gba_saves_beside_rom;
  ConfigUserPath* m_gba_saves;
  QPushButton* m_gba_saves_browse;
};
