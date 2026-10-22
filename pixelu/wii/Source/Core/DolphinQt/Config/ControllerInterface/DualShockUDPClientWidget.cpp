// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ControllerInterface/DualShockUDPClientWidget.h"

#include <optional>

#include <fmt/format.h>

#include <QBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>

#include "DolphinQt/Config/ControllerInterface/DualShockUDPClientEditServerDialog.h"
#include "DolphinQt/Config/ControllerInterface/DualShockUDPSettings.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"

DualShockUDPClientWidget::DualShockUDPClientWidget()
{
  auto* const column = new QVBoxLayout;

  m_servers_enabled = new QCheckBox(tr("Enable"));
  m_servers_enabled->setChecked(DualShockUDPSettings::IsEnabled());
  column->addWidget(m_servers_enabled, 0, {});

  m_server_list = new QListWidget();
  column->addWidget(m_server_list);

  m_add_server = new NonDefaultQPushButton(tr("Add..."));
  m_edit_server = new NonDefaultQPushButton(tr("Edit"));
  m_remove_server = new NonDefaultQPushButton(tr("Remove"));

  auto* const buttons = new QHBoxLayout;
  buttons->addStretch();
  for (QPushButton* const button : {m_add_server, m_edit_server, m_remove_server})
    buttons->addWidget(button);
  column->addItem(buttons);

  auto* const about = new QLabel(
      tr("The DSU protocol enables the use of input and motion data from compatible sources, "
         "like PlayStation, Nintendo Switch and Steam controllers.<br><br>"
         "For setup instructions, "
         "<a href=\"https://wiki.dolphin-emu.org/index.php?title=DSU_Client\">refer to this "
         "page</a>."));
  about->setTextFormat(Qt::RichText);
  about->setWordWrap(true);
  about->setTextInteractionFlags(Qt::TextBrowserInteraction);
  about->setOpenExternalLinks(true);
  column->addWidget(about);

  setLayout(column);
  ReloadServers();

  connect(m_add_server, &QPushButton::clicked, this, [this] { EditServer(false); });
  connect(m_edit_server, &QPushButton::clicked, this, [this] { EditServer(true); });
  connect(m_remove_server, &QPushButton::clicked, this,
          &DualShockUDPClientWidget::RemoveSelectedServer);
  connect(m_server_list, &QListWidget::currentRowChanged, this,
          &DualShockUDPClientWidget::RefreshButtons);
  connect(m_servers_enabled, &QCheckBox::clicked, this,
          [this] { DualShockUDPSettings::SetEnabled(m_servers_enabled->isChecked()); });
}

// Edit and Remove act on the selected server.
void DualShockUDPClientWidget::RefreshButtons()
{
  const bool selected = m_server_list->currentRow() != -1;
  m_edit_server->setEnabled(selected);
  m_remove_server->setEnabled(selected);
}

void DualShockUDPClientWidget::ReloadServers()
{
  m_server_list->clear();
  for (const DualShockUDPServer& server : DualShockUDPSettings::GetServers())
  {
    const std::string line =
        fmt::format("{}:{} - {}", server.description, server.server_address, server.server_port);
    m_server_list->addItem(new QListWidgetItem(QString::fromStdString(line)));
  }
  RefreshButtons();
  emit ConfigChanged();
}

void DualShockUDPClientWidget::EditServer(bool existing)
{
  std::optional<size_t> index;
  if (existing)
    index = static_cast<size_t>(m_server_list->currentRow());
  DualShockUDPClientEditServerDialog dialog(this, index);
  connect(&dialog, &DualShockUDPClientEditServerDialog::accepted, this,
          &DualShockUDPClientWidget::ReloadServers);
  dialog.exec();
}

void DualShockUDPClientWidget::RemoveSelectedServer()
{
  DualShockUDPSettings::RemoveServer(static_cast<size_t>(m_server_list->currentRow()));
  ReloadServers();
}
