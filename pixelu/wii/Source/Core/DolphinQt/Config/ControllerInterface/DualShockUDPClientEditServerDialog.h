// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>

#include <QDialog>

class QDialogButtonBox;
class QGridLayout;
class QLineEdit;
class QSpinBox;

// Adds a DSU server, or edits the one at existing_index.
class DualShockUDPClientEditServerDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit DualShockUDPClientEditServerDialog(QWidget* parent,
                                              std::optional<size_t> existing_index);

private:
  void Save();

  const std::optional<size_t> m_existing_index;
  QDialogButtonBox* m_buttonbox;
  QLineEdit* m_description;
  QLineEdit* m_server_address;
  QSpinBox* m_server_port;
};
