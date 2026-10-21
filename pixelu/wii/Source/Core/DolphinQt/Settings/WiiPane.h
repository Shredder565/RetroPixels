// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

#include "Common/CommonTypes.h"

class ConfigBool;
template <typename T>
class ConfigChoiceMap;
class ConfigChoiceU32;
class ConfigSliderU32;
class ConfigUserPath;
class QGroupBox;
class QListWidget;
class QPushButton;
class QVBoxLayout;

// "Wii" settings page: SYSCONF options, the emulated SD card, the USB passthrough whitelist and
// Wii Remote settings.
class WiiPane : public QWidget
{
  Q_OBJECT
public:
  explicit WiiPane(QWidget* parent = nullptr);

private:
  QGroupBox* BuildSystemGroup();
  QGroupBox* BuildSDCardGroup();
  QGroupBox* BuildPassthroughGroup();
  QGroupBox* BuildRemoteGroup();
  void WireUp();

  void LockForEmulation(bool running);
  void RefreshRemoveButton();
  void RefreshPassthroughList();
  void AddPassthroughDevice();
  void RemovePassthroughDevice();

  void PickSDImage();
  void PickSDSyncFolder();
  // Packs the sync folder into the image (pack == true) or the other way round.
  void ConvertSDCard(bool pack);

  // System
  ConfigBool* m_pal60;
  ConfigBool* m_screen_saver;
  ConfigBool* m_wiilink;
  ConfigBool* m_usb_keyboard;
  ConfigChoiceMap<bool>* m_widescreen;
  ConfigChoiceU32* m_language;
  ConfigChoiceU32* m_sound;

  // SD card
  ConfigUserPath* m_sd_image_path;
  ConfigUserPath* m_sd_folder_path;
  QPushButton* m_sd_pack;
  QPushButton* m_sd_unpack;

  // USB passthrough
  QListWidget* m_passthrough_list;
  QPushButton* m_passthrough_add;
  QPushButton* m_passthrough_remove;

  // Wii Remote
  ConfigChoiceMap<u32>* m_sensor_bar_position;
  ConfigSliderU32* m_ir_sensitivity;
  ConfigSliderU32* m_speaker_volume;
  ConfigBool* m_rumble;
};
