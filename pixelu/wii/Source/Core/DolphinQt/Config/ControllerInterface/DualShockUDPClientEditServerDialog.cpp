// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ControllerInterface/DualShockUDPClientEditServerDialog.h"

#include <fmt/format.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QWidget>

#include "DolphinQt/Config/ControllerInterface/DualShockUDPSettings.h"
#include "DolphinQt/Config/ControllerInterface/ServerStringValidator.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"

DualShockUDPClientEditServerDialog::DualShockUDPClientEditServerDialog(
    QWidget* parent, std::optional<size_t> existing_index)
    : QDialog(parent), m_existing_index(existing_index)
{
  const bool editing = m_existing_index.has_value();
  setWindowTitle(tr(editing ? "Edit DSU Server" : "Add New DSU Server"));

  auto* const grid = new QGridLayout;

  // Text fields refuse the characters used to separate stored servers.
  const auto separator_free_field = [](const QString& text) {
    auto* const field = new QLineEdit(text);
    field->setValidator(new ServerStringValidator(field));
    return field;
  };

  // A new server starts from the defaults; an existing one from its entry.
  DualShockUDPServer shown{"", ciface::DualShockUDPClient::DEFAULT_SERVER_ADDRESS,
                           ciface::DualShockUDPClient::DEFAULT_SERVER_PORT};
  if (editing)
    shown = DualShockUDPSettings::GetServers()[*m_existing_index];

  m_description = separator_free_field(QString::fromStdString(shown.description));
  m_description->setPlaceholderText(tr("BetterJoy, DS4Windows, etc"));
  m_server_address = separator_free_field(QString::fromStdString(shown.server_address));
  m_server_port = new QSpinBox();
  m_server_port->setMaximum(65535);
  m_server_port->setValue(shown.server_port);

  // Row 0 stays empty.
  const std::pair<QString, QWidget*> fields[] = {
      {tr("Description"), m_description},
      {tr("Server IP Address"), m_server_address},
      {tr("Server Port"), m_server_port},
  };
  int row = 1;
  for (const auto& [caption, field] : fields)
  {
    grid->addWidget(new QLabel(caption), row, 0);
    grid->addWidget(field, row, 1);
    ++row;
  }

  m_buttonbox = new QDialogButtonBox();
  auto* const confirm = new QPushButton(tr(editing ? "Apply" : "Add"));
  auto* const cancel = new QPushButton(tr("Cancel"));
  m_buttonbox->addButton(confirm, QDialogButtonBox::AcceptRole);
  m_buttonbox->addButton(cancel, QDialogButtonBox::RejectRole);
  connect(confirm, &QPushButton::clicked, this, &DualShockUDPClientEditServerDialog::Save);
  connect(cancel, &QPushButton::clicked, this, &DualShockUDPClientEditServerDialog::reject);
  confirm->setDefault(true);

  grid->addWidget(m_buttonbox, 4, 0, 1, 2);
  setLayout(grid);
}

void DualShockUDPClientEditServerDialog::Save()
{
  const DualShockUDPServer server{m_description->text().toStdString(),
                                  m_server_address->text().toStdString(), m_server_port->value()};
  if (m_existing_index)
    DualShockUDPSettings::ReplaceServer(*m_existing_index, server);
  else
    DualShockUDPSettings::AddServer(server);
  accept();
}
