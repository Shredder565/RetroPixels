// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/AnalyticsPrompt.h"

#include <QMessageBox>
#include <QObject>
#include <QWidget>

#include "Core/Config/MainSettings.h"
#include "Core/DolphinAnalytics.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Settings.h"

void ShowAnalyticsPrompt(QWidget* parent)
{
  const QString title = QObject::tr("Allow Usage Statistics Reporting");
  const QString question =
      QObject::tr("Do you authorize Dolphin to report information to Dolphin's developers?");
  const QString details =
      QObject::tr("If authorized, Dolphin can collect data on its performance, feature usage, "
                  "and configuration, as well as data on your system's hardware and operating "
                  "system.\n\nNo private data is ever collected. This data helps us understand how "
                  "people and emulated games use Dolphin and prioritize our efforts. It also "
                  "helps us identify rare configurations that are causing bugs, performance and "
                  "stability issues.\nThis authorization can be revoked at any time through "
                  "Dolphin's settings.");

  ModalMessageBox box(parent);
  box.setIcon(QMessageBox::Question);
  box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
  box.setWindowTitle(title);
  box.setText(question);
  box.setInformativeText(details);

  const bool allowed = box.exec() == QMessageBox::Yes;

  // Whatever the answer was, the question counts as asked.
  Config::SetBase(Config::MAIN_ANALYTICS_PERMISSION_ASKED, true);
  Settings::Instance().SetAnalyticsEnabled(allowed);
  DolphinAnalytics::Instance().ReloadConfig();
}
