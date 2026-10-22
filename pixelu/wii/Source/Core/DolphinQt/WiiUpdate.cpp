// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/WiiUpdate.h"

#include <functional>
#include <future>
#include <utility>

#include <QCloseEvent>
#include <QObject>
#include <QProgressDialog>
#include <QPushButton>

#include "Common/Assert.h"
#include "Common/Flag.h"

#include "Core/WiiUtils.h"

#include "DiscIO/NANDImporter.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"

namespace WiiUpdate
{
namespace
{
using Result = WiiUtils::UpdateResult;
using UpdateJob = std::function<Result(WiiUtils::UpdateCallback)>;

// A half-finished update leaves mismatched system software behind, so the dialog cannot be
// dismissed (Esc and the close button both end up in reject()); only the cancel button, which
// asks the update to stop at the next title, is offered.
class UpdateProgressDialog final : public QProgressDialog
{
public:
  using QProgressDialog::QProgressDialog;

protected:
  void reject() override {}
};

void ReportFailure(QWidget* parent, const QString& text)
{
  ModalMessageBox::critical(parent, QObject::tr("Update failed"), text);
}

// After a successful (or unnecessary) update the NAND's certificates are extracted again.
void ReportSuccess(QWidget* parent, const QString& text)
{
  ModalMessageBox::information(parent, QObject::tr("Update completed"), text);
  DiscIO::NANDImporter().ExtractCertificates();
}

void ShowResult(QWidget* parent, Result result)
{
  switch (result)
  {
  case Result::Succeeded:
    ReportSuccess(parent, QObject::tr("The emulated Wii console has been updated."));
    return;
  case Result::AlreadyUpToDate:
    ReportSuccess(parent, QObject::tr("The emulated Wii console is already up-to-date."));
    return;
  case Result::ServerFailed:
    ReportFailure(parent, QObject::tr("Could not download update information from Nintendo. "
                                      "Please check your Internet connection and try again."));
    return;
  case Result::DownloadFailed:
    ReportFailure(parent, QObject::tr("Could not download update files from Nintendo. "
                                      "Please check your Internet connection and try again."));
    return;
  case Result::ImportFailed:
    ReportFailure(parent, QObject::tr("Could not install an update to the Wii system memory. "
                                      "Please refer to logs for more information."));
    return;
  case Result::Cancelled:
    ModalMessageBox::warning(
        parent, QObject::tr("Update cancelled"),
        QObject::tr("The update has been cancelled. It is strongly recommended to "
                    "finish it in order to avoid inconsistent system software versions."));
    return;
  case Result::RegionMismatch:
    ReportFailure(parent, QObject::tr("The game's region does not match your console's. "
                                      "To avoid issues with the system menu, it is not possible "
                                      "to update the emulated console using this disc."));
    return;
  case Result::MissingUpdatePartition:
  case Result::DiscReadFailed:
    ReportFailure(parent, QObject::tr("The game disc does not contain any usable "
                                      "update information."));
    return;
  default:
    ASSERT(false);
    return;
  }
}

// Runs the update on a worker thread while a modal progress dialog shows which title is being
// installed. The worker hears about a cancel through the callback's return value.
Result RunWithProgress(QWidget* parent, const UpdateJob& job)
{
  UpdateProgressDialog progress{parent};
  progress.setLabelText(QObject::tr("Preparing to update...\nThis can take a while."));
  progress.setWindowTitle(QObject::tr("Updating"));
  // Without this QProgressDialog comes up too small for its text.
  progress.setMinimumSize(360, 150);

  // A button of our own, so it can be greyed out once pressed. setCancelButton() hands it to the
  // dialog; the stock canceled() handling (which would reset the dialog) is replaced.
  auto* const stop = new QPushButton(QObject::tr("&Cancel"), parent);
  progress.setCancelButton(stop);
  Common::Flag stopping;
  QObject::disconnect(&progress, &QProgressDialog::canceled, nullptr, nullptr);
  QObject::connect(&progress, &QProgressDialog::canceled, [&] {
    progress.setLabelText(QObject::tr("Finishing the update...\nThis can take a while."));
    stop->setEnabled(false);
    stopping.Set();
  });

  // Called on the worker for each title. Progress is shown on the dialog's thread; once a cancel
  // has been asked for, the label keeps saying so.
  const WiiUtils::UpdateCallback on_title = [&](size_t done, size_t total, u64 title_id) {
    QueueOnObject(&progress, [&progress, &stopping, done, total, title_id] {
      if (stopping.IsSet())
        return;
      progress.setRange(0, static_cast<int>(total));
      progress.setValue(static_cast<int>(done));
      progress.setLabelText(QObject::tr("Updating title %1...\nThis can take a while.")
                                .arg(title_id, 16, 16, QLatin1Char('0')));
    });
    return !stopping.IsSet();
  };

  std::future<Result> outcome = std::async(std::launch::async, [&] {
    const Result finished = job(on_title);
    QueueOnObject(&progress, [&progress] { progress.done(0); });
    return finished;
  });

  progress.exec();
  return outcome.get();
}
}  // namespace

void PerformOnlineUpdate(const std::string& region, QWidget* parent)
{
  const int answer = ModalMessageBox::question(
      parent, QObject::tr("Confirm"),
      QObject::tr("Connect to the Internet and perform an online system update?"));
  if (answer != QMessageBox::Yes)
    return;

  const Result result = RunWithProgress(parent, [&region](WiiUtils::UpdateCallback callback) {
    return WiiUtils::DoOnlineUpdate(std::move(callback), region);
  });
  ShowResult(parent, result);
}

void PerformDiscUpdate(const std::string& file_path, QWidget* parent)
{
  const Result result = RunWithProgress(parent, [&file_path](WiiUtils::UpdateCallback callback) {
    return WiiUtils::DoDiscUpdate(std::move(callback), file_path);
  });
  ShowResult(parent, result);
}
}  // namespace WiiUpdate
