// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

class QLineEdit;

// Asks for the single address-like setting that a broadband adapter / modem mode needs
// (MAC, tapserver destination, DNS server or XLink Kai client IP) and stores it on OK.
class BroadbandAdapterSettingsDialog final : public QDialog
{
  Q_OBJECT
public:
  enum class Type
  {
    Ethernet,
    XLinkKai,
    TapServer,
    BuiltIn,
    ModemTapServer
  };

  explicit BroadbandAdapterSettingsDialog(QWidget* target, Type bba_type);

private:
  struct Wording;

  Wording DescribeMode() const;
  void BuildLayout();
  void Store();

  const Type m_mode;
  QLineEdit* m_value_edit = nullptr;
};
