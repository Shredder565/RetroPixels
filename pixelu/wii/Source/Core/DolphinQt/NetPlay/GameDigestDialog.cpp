// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NetPlay/GameDigestDialog.h"

#include <algorithm>
#include <functional>

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"

#include "DolphinQt/Settings.h"

namespace
{
// Looks the pid up in the current client's player list; falls back to a placeholder.
QString DescribePlayer(int pid)
{
  QString result = QObject::tr("Invalid Player ID");
  const auto net_client = Settings::Instance().GetNetPlayClient();
  if (net_client)
  {
    const auto players = net_client->GetPlayers();
    const auto match =
        std::ranges::find_if(players, [pid](const auto* p) { return p->pid == pid; });
    if (match != players.end())
      result = QString::fromStdString((*match)->name);
  }
  return result;
}

template <typename Widget> void DropAll(QVBoxLayout* layout, std::map<int, Widget*>& widgets)
{
  for (const auto& entry : widgets)
  {
    layout->removeWidget(entry.second);
    entry.second->deleteLater();
  }
}
}  // namespace

GameDigestDialog::GameDigestDialog(QWidget* owner) : QDialog(owner)
{
  CreateWidgets();
  ConnectWidgets();
  setWindowTitle(tr("SHA1 Digest"));
  // Attached to the lobby window (a sheet where the platform has them) and blocking only it.
  setWindowFlags(Qt::Sheet | Qt::Dialog);
  setWindowModality(Qt::WindowModal);
}

void GameDigestDialog::CreateWidgets()
{
  // Per-player rows are added to this box by show().
  m_progress_layout = new QVBoxLayout;
  m_progress_box = new QGroupBox;
  m_progress_box->setLayout(m_progress_layout);

  m_check_label = new QLabel;
  m_button_box = new QDialogButtonBox(QDialogButtonBox::NoButton);

  m_main_layout = new QVBoxLayout;
  for (QWidget* const part :
       {static_cast<QWidget*>(m_progress_box), static_cast<QWidget*>(m_check_label),
        static_cast<QWidget*>(m_button_box)})
  {
    m_main_layout->addWidget(part);
  }
  setLayout(m_main_layout);
}

void GameDigestDialog::ConnectWidgets()
{
  connect(m_button_box, &QDialogButtonBox::rejected, this, &GameDigestDialog::reject);
}

void GameDigestDialog::UseOnlyButton(int standard_button)
{
  const auto button_id = static_cast<QDialogButtonBox::StandardButton>(standard_button);
  m_button_box->setStandardButtons(button_id);
  QPushButton* const only = m_button_box->button(button_id);
  only->setAutoDefault(false);
  only->setDefault(false);
}

QLabel* GameDigestDialog::StatusLabelFor(int pid) const
{
  const auto found = m_status_labels.find(pid);
  return found == m_status_labels.end() ? nullptr : found->second;
}

void GameDigestDialog::show(const QString& title)
{
  m_progress_box->setTitle(title);

  // Forget whatever the previous run left behind.
  DropAll(m_progress_layout, m_progress_bars);
  DropAll(m_progress_layout, m_status_labels);
  m_progress_bars.clear();
  m_status_labels.clear();
  m_results.clear();
  m_check_label->setText(QString::fromStdString(""));

  const auto net_client = Settings::Instance().GetNetPlayClient();
  if (!net_client)
    return;

  // Only the host can abort hashing; clients just get a way to dismiss the window.
  UseOnlyButton(Settings::Instance().GetNetPlayServer() ? QDialogButtonBox::Cancel :
                                                          QDialogButtonBox::Close);

  for (const auto* player : net_client->GetPlayers())
  {
    QProgressBar*& bar = m_progress_bars[player->pid];
    bar = new QProgressBar;
    QLabel*& status = m_status_labels[player->pid];
    status = new QLabel;
    m_progress_layout->addWidget(bar);
    m_progress_layout->addWidget(status);
  }

  QDialog::show();
}

void GameDigestDialog::SetProgress(int pid, int progress)
{
  const QString who = DescribePlayer(pid);
  QLabel* const status = StatusLabelFor(pid);
  if (status == nullptr)
    return;

  status->setText(tr("%1[%2]: %3 %").arg(who, QString::number(pid), QString::number(progress)));
  m_progress_bars[pid]->setValue(progress);
}

void GameDigestDialog::SetResult(int pid, const std::string& result)
{
  const QString who = DescribePlayer(pid);
  QLabel* const status = StatusLabelFor(pid);
  if (status == nullptr)
    return;

  status->setText(tr("%1[%2]: %3").arg(who, QString::number(pid), QString::fromStdString(result)));
  m_results.push_back(result);

  const auto net_client = Settings::Instance().GetNetPlayClient();
  if (!net_client || m_results.size() < net_client->GetPlayers().size())
    return;

  // Everyone has answered: the digests agree when no neighbouring pair differs.
  const bool all_equal =
      std::ranges::adjacent_find(m_results, std::ranges::not_equal_to{}) == m_results.end();
  m_check_label->setText(all_equal ? tr("The hashes match!") : tr("The hashes do not match!"));

  UseOnlyButton(QDialogButtonBox::Close);
}

void GameDigestDialog::reject()
{
  if (const auto net_server = Settings::Instance().GetNetPlayServer())
    net_server->AbortGameDigest();

  QDialog::reject();
}
