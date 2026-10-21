// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#if defined(CIFACE_USE_DUALSHOCKUDPCLIENT)
class DualShockUDPClientWidget;
#endif
class QTabWidget;
class QDialogButtonBox;

// "Alternate Input Sources": settings of input backends that need some
// (currently only the DSU client).
class ControllerInterfaceWindow final : public QDialog
{
  Q_OBJECT
public:
  explicit ControllerInterfaceWindow(QWidget* parent);

private:
  QTabWidget* m_tab_widget;
  QDialogButtonBox* m_button_box;

#if defined(CIFACE_USE_DUALSHOCKUDPCLIENT)
  DualShockUDPClientWidget* m_dsuclient_widget;
#endif
};
