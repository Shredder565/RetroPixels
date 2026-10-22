// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NetPlay/PadMappingDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>

#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"

#include "DolphinQt/Settings.h"

namespace
{
// Grid rows, top to bottom. Each port gets its own column.
enum GridRow : int
{
  ROW_GC_LABEL = 0,
  ROW_GC_CHOICE = 1,
  ROW_GBA_TOGGLE = 2,
  ROW_WII_LABEL = 3,
  ROW_WII_CHOICE = 4,
  ROW_BUTTONS = 5,
};
}  // namespace

PadMappingDialog::PadMappingDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Assign Controllers"));

  CreateWidgets();
  ConnectWidgets();
}

void PadMappingDialog::CreateWidgets()
{
  m_main_layout = new QGridLayout;
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Ok);

  for (unsigned int port = 0; port < m_wii_boxes.size(); ++port)
  {
    const int column = static_cast<int>(port);
    const unsigned int shown_number = port + 1;

    m_gc_boxes[port] = new QComboBox;
    m_gba_boxes[port] = new QCheckBox(tr("GBA Port %1").arg(shown_number));
    m_wii_boxes[port] = new QComboBox;

    m_main_layout->addWidget(new QLabel(tr("GC Port %1").arg(shown_number)), ROW_GC_LABEL, column);
    m_main_layout->addWidget(m_gc_boxes[port], ROW_GC_CHOICE, column);
#ifdef HAS_LIBMGBA
    m_main_layout->addWidget(m_gba_boxes[port], ROW_GBA_TOGGLE, column);
#endif
    m_main_layout->addWidget(new QLabel(tr("Wii Remote %1").arg(shown_number)), ROW_WII_LABEL,
                             column);
    m_main_layout->addWidget(m_wii_boxes[port], ROW_WII_CHOICE, column);
  }

  // The button row spans every column.
  m_main_layout->addWidget(m_button_box, ROW_BUTTONS, 0, 1, -1);

  setLayout(m_main_layout);
}

void PadMappingDialog::ConnectWidgets()
{
  connect(m_button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);

  for (QComboBox* const combo : m_gc_boxes)
    connect(combo, &QComboBox::currentIndexChanged, this, &PadMappingDialog::OnMappingChanged);
  for (QComboBox* const combo : m_wii_boxes)
    connect(combo, &QComboBox::currentIndexChanged, this, &PadMappingDialog::OnMappingChanged);

  for (QCheckBox* const toggle : m_gba_boxes)
  {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    connect(toggle, &QCheckBox::checkStateChanged, this, &PadMappingDialog::OnMappingChanged);
#else
    connect(toggle, &QCheckBox::stateChanged, this, &PadMappingDialog::OnMappingChanged);
#endif
  }
}

void PadMappingDialog::FillPlayerChoices(const QStringList& choices)
{
  // Signals stay blocked so that refilling a box does not overwrite the loaded mapping.
  // As before, the stored player id is used directly as the entry to select.
  for (size_t port = 0; port < m_gc_boxes.size(); ++port)
  {
    QComboBox* const combo = m_gc_boxes[port];
    const QSignalBlocker quiet(combo);
    combo->clear();
    combo->addItems(choices);
    combo->setCurrentIndex(m_pad_mapping[port]);
  }
  for (size_t port = 0; port < m_wii_boxes.size(); ++port)
  {
    QComboBox* const combo = m_wii_boxes[port];
    const QSignalBlocker quiet(combo);
    combo->clear();
    combo->addItems(choices);
    combo->setCurrentIndex(m_wii_mapping[port]);
  }
}

int PadMappingDialog::exec()
{
  const auto net_client = Settings::Instance().GetNetPlayClient();
  const auto net_server = Settings::Instance().GetNetPlayServer();

  // Start from what the server currently uses.
  m_players = net_client->GetPlayers();
  m_pad_mapping = net_server->GetPadMapping();
  m_gba_config = net_server->GetGBAConfig();
  m_wii_mapping = net_server->GetWiimoteMapping();

  // Entry 0 means "nobody"; entry n is the n-th player in the list.
  QStringList choices;
  choices.append(tr("None"));
  for (const NetPlay::Player* player : m_players)
  {
    choices.append(
        QStringLiteral("%1 (%2)").arg(QString::fromStdString(player->name)).arg(player->pid));
  }

  FillPlayerChoices(choices);

  for (size_t port = 0; port < m_gba_boxes.size(); ++port)
  {
    const QSignalBlocker quiet(m_gba_boxes[port]);
    m_gba_boxes[port]->setChecked(m_gba_config[port].enabled);
  }

  return QDialog::exec();
}

NetPlay::PadMappingArray PadMappingDialog::GetGCPadArray()
{
  return m_pad_mapping;
}

NetPlay::GBAConfigArray PadMappingDialog::GetGBAArray()
{
  return m_gba_config;
}

NetPlay::PadMappingArray PadMappingDialog::GetWiimoteArray()
{
  return m_wii_mapping;
}

NetPlay::PlayerId PadMappingDialog::PlayerAt(int combo_index) const
{
  if (combo_index <= 0)
    return 0;
  return m_players[static_cast<size_t>(combo_index - 1)]->pid;
}

void PadMappingDialog::OnMappingChanged()
{
  for (size_t port = 0; port < m_wii_boxes.size(); ++port)
  {
    const int gc_choice = m_gc_boxes[port]->currentIndex();
    const int wii_choice = m_wii_boxes[port]->currentIndex();

    m_pad_mapping[port] = PlayerAt(gc_choice);
    m_gba_config[port].enabled = m_gba_boxes[port]->isChecked();
    m_wii_mapping[port] = PlayerAt(wii_choice);
  }
}
