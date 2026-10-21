// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/QtUtils/AspectRatioWidget.h"
#include "DolphinQt/TAS/TASInputWindow.h"

#include "Core/HW/WiimoteEmu/ExtensionPort.h"

class QGroupBox;
class QHideEvent;
class QShowEvent;
class QSpinBox;
class TASCheckBox;
class TASSpinBox;

namespace WiimoteEmu
{
class Extension;
class Wiimote;
}  // namespace WiimoteEmu

namespace ControllerEmu
{
class Attachments;
}

// TAS input for one emulated Wii Remote. The controls shown follow the attached extension
// (none, Nunchuk or Classic Controller) and whether MotionPlus is attached.
class WiiTASInputWindow : public TASInputWindow
{
  Q_OBJECT
public:
  explicit WiiTASInputWindow(QWidget* parent, int num);

  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;

  void UpdateExtension(int extension);
  void UpdateMotionPlus(bool attached);

private:
  WiimoteEmu::Wiimote* GetWiimote();
  ControllerEmu::Attachments* GetAttachments();
  WiimoteEmu::Extension* GetExtension();

  struct MotionAxis;
  struct MotionRange;

  AspectRatioWidget* BuildIRBox();
  QGroupBox* BuildMotionBox(const QString& title, const char* group, InputOverrider* overrider,
                            const MotionRange& range, const MotionAxis (&axes)[3]);
  QGroupBox* BuildClassicTriggersBox();
  void BuildRemoteAndNunchukButtons();
  QGroupBox* BuildClassicButtonsBox();

  void LoadExtensionAndMotionPlus();
  void UpdateControlVisibility();
  void UpdateInputOverrideFunction();

  WiimoteEmu::ExtensionNumber m_active_extension;
  bool m_is_motion_plus_attached;
  int m_attachment_callback_id = -1;
  int m_motion_plus_callback_id = -1;
  int m_num;

  InputOverrider m_wiimote_overrider;
  InputOverrider m_nunchuk_overrider;
  InputOverrider m_classic_overrider;

  AspectRatioWidget* m_ir_box;
  AspectRatioWidget* m_nunchuk_stick_box;
  AspectRatioWidget* m_classic_left_stick_box;
  AspectRatioWidget* m_classic_right_stick_box;
  QGroupBox* m_remote_accelerometer_box;
  QGroupBox* m_remote_gyroscope_box;
  QGroupBox* m_nunchuk_accelerometer_box;
  QGroupBox* m_triggers_box;
  QGroupBox* m_remote_buttons_box;
  QGroupBox* m_nunchuk_buttons_box;
  QGroupBox* m_classic_buttons_box;
};
