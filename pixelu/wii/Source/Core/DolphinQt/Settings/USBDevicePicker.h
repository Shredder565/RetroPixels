// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <QDialog>

#include "Core/USBUtils.h"

class QDialogButtonBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
class QVBoxLayout;

// Lets the user type a USB VID/PID pair or pick one from the devices currently plugged in
// (optionally filtered). The list refreshes itself once a second.
class USBDevicePicker final : public QDialog
{
  Q_OBJECT
public:
  using FilterFunctionType = std::function<bool(const USBUtils::DeviceInfo&)>;
  explicit USBDevicePicker(QWidget* parent, FilterFunctionType filter);

  // Shows the picker modally; returns the entered/selected device if accepted.
  static std::optional<USBUtils::DeviceInfo> Run(
      QWidget* parent, const QString& title,
      const FilterFunctionType filter = [](const USBUtils::DeviceInfo&) { return true; });

private:
  QVBoxLayout* BuildContents();
  void PollDevices();
  void CopySelectionToFields();
  std::optional<USBUtils::DeviceInfo> EnteredDevice() const;

  FilterFunctionType m_filter;
  std::vector<USBUtils::DeviceInfo> m_listed;

  QLineEdit* m_vid_field = nullptr;
  QLineEdit* m_pid_field = nullptr;
  QListWidget* m_device_list = nullptr;
  QTimer* m_poll_timer = nullptr;
};
