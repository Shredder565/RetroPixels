// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Updater.h"

#include <cstdlib>
#include <utility>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "Common/Version.h"

#include "DolphinQt/QtUtils/RunOnObject.h"
#include "DolphinQt/Settings.h"

// docs/autoupdate_overview.md describes the whole auto-update flow.

Updater::Updater(QWidget* parent, std::string update_track, std::string hash_override)
    : m_parent(parent), m_update_track(std::move(update_track)),
      m_hash_override(std::move(hash_override))
{
  connect(this, &QThread::finished, this, &QObject::deleteLater);
}

void Updater::run()
{
  AutoUpdateChecker::CheckForUpdate(m_update_track, m_hash_override, CheckType::Automatic);
}

void Updater::CheckForUpdate()
{
  AutoUpdateChecker::CheckForUpdate(m_update_track, m_hash_override, CheckType::Manual);
}

void Updater::CloseParent()
{
  RunOnObject(m_parent, [this] {
    m_parent->close();
    return 0;
  });
}

std::optional<int> Updater::AskUser(const NewVersionInformation& info, bool* install_on_exit)
{
  return RunOnObject(m_parent, [this, &info, install_on_exit] {
    auto* const dialog = new QDialog(m_parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setWindowTitle(tr("Update available"));

    auto* const column = new QVBoxLayout;
    dialog->setLayout(column);

    auto* const headline = new QLabel(
        tr("<h2>A new version of Dolphin is available!</h2>Dolphin %1 is available for "
           "download. "
           "You are running %2.<br> Would you like to update?<br><h4>Release Notes:</h4>")
            .arg(QString::fromStdString(info.new_shortrev))
            .arg(QString::fromStdString(Common::GetScmDescStr())));
    headline->setTextFormat(Qt::RichText);
    column->addWidget(headline);

    auto* const notes = new QTextBrowser;
    notes->setHtml(QString::fromStdString(info.changelog_html));
    notes->setOpenExternalLinks(true);
    notes->setMinimumWidth(400);
    column->addWidget(notes);

    auto* const defer = new QCheckBox(tr("Update after closing Dolphin"));
    connect(defer, &QCheckBox::toggled, [install_on_exit](bool on) { *install_on_exit = on; });
    column->addWidget(defer);

    auto* const choices = new QDialogButtonBox;
    QPushButton* const never =
        choices->addButton(tr("Never Auto-Update"), QDialogButtonBox::DestructiveRole);
    choices->addButton(tr("Remind Me Later"), QDialogButtonBox::RejectRole);
    choices->addButton(tr("Install Update"), QDialogButtonBox::AcceptRole);
    column->addWidget(choices);

    // "Never" switches auto-update off by clearing the track, then dismisses the dialog.
    connect(never, &QPushButton::clicked, [dialog] {
      Settings::Instance().SetAutoUpdateTrack(QString{});
      dialog->reject();
    });
    connect(choices, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(choices, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

    return dialog->exec();
  });
}

void Updater::OnUpdateAvailable(const NewVersionInformation& info)
{
  // With a custom update server (used for testing the updater) the update is applied without
  // asking.
  if (std::getenv("DOLPHIN_UPDATE_SERVER_URL") != nullptr)
  {
    TriggerUpdate(info, RestartMode::RESTART_AFTER_UPDATE);
    CloseParent();
    return;
  }

  bool install_on_exit = false;
  const std::optional<int> answer = AskUser(info, &install_on_exit);
  if (answer != QDialog::Accepted)
    return;

  if (install_on_exit)
  {
    TriggerUpdate(info, RestartMode::NO_RESTART_AFTER_UPDATE);
    return;
  }

  TriggerUpdate(info, RestartMode::RESTART_AFTER_UPDATE);
  CloseParent();
}
