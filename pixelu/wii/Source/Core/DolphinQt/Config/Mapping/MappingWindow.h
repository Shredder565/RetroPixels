// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>
#include <QString>

namespace ControllerEmu
{
class EmulatedController;
}

class InputConfig;
class MappingButton;
class MappingWidget;

class QAction;
class QComboBox;
class QGroupBox;
class QPushButton;
class QTabWidget;
class QWidget;

// The controller configuration window: device selection, reset buttons and profiles on top,
// and one tab per mapping page of the chosen controller type.
class MappingWindow final : public QDialog
{
  Q_OBJECT
public:
  enum class Type
  {
    // GameCube
    MAPPING_GC_BONGOS,
    MAPPING_GC_DANCEMAT,
    MAPPING_GC_GBA,
    MAPPING_GC_KEYBOARD,
    MAPPING_GCPAD,
    MAPPING_GC_STEERINGWHEEL,
    MAPPING_GC_MICROPHONE,
    // Wii
    MAPPING_WIIMOTE_EMU,
    // Hotkeys
    MAPPING_HOTKEYS,
    // Freelook
    MAPPING_FREELOOK,
    // Triforce
    MAPPING_AM_BASEBOARD,
  };

  explicit MappingWindow(QWidget* parent, Type type, int port_num);

  int GetPort() const;
  ControllerEmu::EmulatedController* GetController() const;
  bool IsCreateOtherDeviceMappingsEnabled() const;
  bool IsWaitForAlternateMappingsEnabled() const;
  bool IsIterativeMappingEnabled() const;
  void ShowExtensionMotionTabs(bool show);
  void ActivateExtensionTab();

signals:
  // The configuration changed; pages re-read it.
  void ConfigChanged();
  // Ticks at the screen refresh rate so the live indicators can redraw.
  void Update();
  void Save();

  void UnQueueInputDetection(MappingButton*);
  void QueueInputDetection(MappingButton*);
  void CancelMapping();

private:
  QGroupBox* BuildDeviceBox();
  QGroupBox* BuildProfileBox();
  QGroupBox* BuildResetBox();
  void BuildWindow(QGroupBox* devices, QGroupBox* reset, QGroupBox* profiles);
  void WireUp();
  void StartIndicatorTimer();

  // Creates the pages of `type`, then loads its configuration and profiles.
  void AddPages(Type type);
  QWidget* AddPage(const QString& title, QWidget* page);
  QString MotionSimulationTabTitle() const;
  QString MotionInputTabTitle() const;

  // Devices
  void FillDeviceList();
  void DeviceChosen();

  // Profiles
  void FillProfileList();
  void MatchTypedProfileName();
  void RefreshProfileButtons();
  void LoadProfile();
  void SaveProfile();
  void DeleteProfile();
  void OpenProfileFolder();
  void ShowMissingProfileError(const QString& name);

  // Reset
  void ResetToDefaults();
  void ClearMappings();
  void ReloadMappings();

  const int m_port;
  ControllerEmu::EmulatedController* m_controller = nullptr;
  InputConfig* m_config = nullptr;

  QComboBox* m_devices = nullptr;
  QAction* m_map_other_devices = nullptr;
  QAction* m_wait_for_alternates = nullptr;
  QAction* m_iterative_mapping = nullptr;

  QComboBox* m_profiles = nullptr;
  QPushButton* m_load_profile = nullptr;
  QPushButton* m_save_profile = nullptr;
  QAction* m_delete_profile = nullptr;
  QAction* m_open_profile_folder = nullptr;

  QPushButton* m_reset_defaults = nullptr;
  QPushButton* m_reset_clear = nullptr;

  QTabWidget* m_tabs = nullptr;
  // The Nunchuk motion tabs, which are only in the tab bar while a Nunchuk is attached.
  QWidget* m_motion_simulation_tab = nullptr;
  QWidget* m_motion_input_tab = nullptr;
};
