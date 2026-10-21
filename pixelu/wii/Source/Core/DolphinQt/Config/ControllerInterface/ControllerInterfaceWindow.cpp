// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ControllerInterface/ControllerInterfaceWindow.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>

#if defined(CIFACE_USE_DUALSHOCKUDPCLIENT)
#include "DolphinQt/Config/ControllerInterface/DualShockUDPClientWidget.h"
#endif

ControllerInterfaceWindow::ControllerInterfaceWindow(QWidget* parent) : QDialog(parent)
{
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  m_tab_widget = new QTabWidget();
#if defined(CIFACE_USE_DUALSHOCKUDPCLIENT)
  m_dsuclient_widget = new DualShockUDPClientWidget();
  m_tab_widget->addTab(m_dsuclient_widget, tr("DSU Client"));
#endif

  auto* const column = new QVBoxLayout();
  if (m_tab_widget->count() == 0)
  {
    column->addWidget(new QLabel(tr("Nothing to configure")), 0,
                      Qt::AlignVCenter | Qt::AlignHCenter);
  }
  else
  {
    column->addWidget(m_tab_widget);
  }
  column->addWidget(m_button_box);
  setLayout(column);

  setWindowTitle(tr("Alternate Input Sources"));
}
