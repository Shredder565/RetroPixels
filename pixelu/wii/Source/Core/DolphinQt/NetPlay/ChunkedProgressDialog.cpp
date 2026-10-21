// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NetPlay/ChunkedProgressDialog.h"

#include <cmath>

#include <fmt/format.h>

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/Contains.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"

#include "DolphinQt/Settings.h"

namespace
{
QString NameOfPlayer(int pid)
{
  QString name = QObject::tr("Invalid Player ID");
  const auto net_client = Settings::Instance().GetNetPlayClient();
  if (!net_client)
    return name;

  for (const auto* player : net_client->GetPlayers())
  {
    if (player->pid != pid)
      continue;
    name = QString::fromStdString(player->name);
    break;
  }
  return name;
}

float ToMebibytes(u64 bytes)
{
  return static_cast<float>(bytes) / 1024.0f / 1024.0f;
}

QString TwoDecimals(float value)
{
  return QString::fromStdString(fmt::format("{:.2f}", value));
}
}  // namespace

ChunkedProgressDialog::ChunkedProgressDialog(QWidget* owner) : QDialog(owner)
{
  CreateWidgets();
  ConnectWidgets();
  setWindowTitle(tr("Data Transfer"));
}

void ChunkedProgressDialog::CreateWidgets()
{
  // One bar and one status line per player are added to this box by show().
  m_progress_layout = new QVBoxLayout;
  m_progress_box = new QGroupBox;
  m_progress_box->setLayout(m_progress_layout);

  // The real button is chosen in show(), once it is known whether we are hosting.
  m_button_box = new QDialogButtonBox(QDialogButtonBox::NoButton);

  m_main_layout = new QVBoxLayout;
  m_main_layout->addWidget(m_progress_box);
  m_main_layout->addWidget(m_button_box);
  setLayout(m_main_layout);
}

void ChunkedProgressDialog::ConnectWidgets()
{
  connect(m_button_box, &QDialogButtonBox::rejected, this, &ChunkedProgressDialog::reject);
}

void ChunkedProgressDialog::ClearPlayerRows()
{
  // Bars go first, then the status lines; both are deleted once control returns to the loop.
  for (const auto& [pid, bar] : m_progress_bars)
  {
    m_progress_layout->removeWidget(bar);
    bar->deleteLater();
  }
  for (const auto& [pid, label] : m_status_labels)
  {
    m_progress_layout->removeWidget(label);
    label->deleteLater();
  }
  m_progress_bars.clear();
  m_status_labels.clear();
}

void ChunkedProgressDialog::ShowSingleButton(bool is_host)
{
  // The host may cancel the transfer; everybody else can only close the window.
  const auto which = is_host ? QDialogButtonBox::Cancel : QDialogButtonBox::Close;
  m_button_box->setStandardButtons(which);
  QPushButton* const button = m_button_box->button(which);
  button->setAutoDefault(false);
  button->setDefault(false);
}

void ChunkedProgressDialog::show(const QString& title, const u64 data_size,
                                 std::span<const int> players)
{
  m_progress_box->setTitle(title);
  m_data_size = data_size;

  ClearPlayerRows();

  const auto net_client = Settings::Instance().GetNetPlayClient();
  if (!net_client)
    return;

  ShowSingleButton(Settings::Instance().GetNetPlayServer() != nullptr);

  for (const auto* player : net_client->GetPlayers())
  {
    if (!Common::Contains(players, player->pid))
      continue;

    auto* const bar = new QProgressBar;
    auto* const status = new QLabel;
    m_progress_bars[player->pid] = bar;
    m_status_labels[player->pid] = status;
    m_progress_layout->addWidget(bar);
    m_progress_layout->addWidget(status);
  }

  QDialog::show();
}

void ChunkedProgressDialog::SetProgress(const int pid, const u64 progress)
{
  const QString name = NameOfPlayer(pid);

  const auto status = m_status_labels.find(pid);
  if (status == m_status_labels.end())
    return;

  const float fraction = static_cast<float>(progress) / static_cast<float>(m_data_size);
  const int percent = static_cast<int>(std::lround(fraction * 100.0f));

  status->second->setText(tr("%1[%2]: %3/%4 MiB")
                              .arg(name, QString::number(pid), TwoDecimals(ToMebibytes(progress)),
                                   TwoDecimals(ToMebibytes(m_data_size))));
  m_progress_bars[pid]->setValue(percent);
}

void ChunkedProgressDialog::reject()
{
  if (const auto net_server = Settings::Instance().GetNetPlayServer())
    net_server->AbortGameStart();

  QDialog::reject();
}
