// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NKitWarningDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "DolphinQt/Resources.h"

bool NKitWarningDialog::ShowUnlessDisabled(QWidget* parent)
{
  if (!Config::Get(Config::MAIN_SKIP_NKIT_WARNING))
  {
    NKitWarningDialog warning(parent);
    return warning.exec() == QDialog::Accepted;
  }
  return true;
}

NKitWarningDialog::NKitWarningDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("NKit Warning"));
  setWindowIcon(Resources::GetAppIcon());

  // Each line break sits at the start of the line it begins.
  auto* const explanation = new QLabel(
      tr("You are about to run an NKit disc image. NKit disc images cause problems that "
         "don't happen with normal disc images. These problems include:"
         "\n"
         "\n• The emulated loading times are longer"
         "\n• You can't use NetPlay with people who have normal disc images"
         "\n• Input recordings are not compatible between NKit disc images and normal disc images"
         "\n• Savestates are not compatible between NKit disc images and normal disc images"
         "\n• Some games can crash, such as Super Paper Mario and Metal Gear Solid: The Twin Snakes"
         "\n• Wii games don't work at all in older versions of Dolphin and in many other programs"
         "\n"
         "\nAre you sure you want to continue anyway?"));
  explanation->setWordWrap(true);

  auto* const understood = new QCheckBox(tr("I am aware of the risks and want to continue"));
  auto* const never_again = new QCheckBox(tr("Don't show this again"));

  auto* const ok_button = new QPushButton(tr("OK"));
  auto* const cancel_button = new QPushButton(tr("Cancel"));
  auto* const buttons_row = new QHBoxLayout;
  buttons_row->addWidget(ok_button);
  buttons_row->addWidget(cancel_button);

  auto* const text_column = new QVBoxLayout;
  text_column->addWidget(explanation);
  text_column->addWidget(understood);
  text_column->addWidget(never_again);
  text_column->addLayout(buttons_row);

  auto* const warning_icon = new QLabel;
  warning_icon->setPixmap(
      QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(100));
  warning_icon->setAlignment(Qt::AlignTop);

  auto* const row = new QHBoxLayout;
  row->addWidget(warning_icon);
  row->addSpacing(10);
  row->addLayout(text_column);
  setLayout(row);

  connect(ok_button, &QPushButton::clicked, this, &QDialog::accept);
  connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);

  // OK stays greyed out until the risks have been acknowledged.
  ok_button->setEnabled(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(understood, &QCheckBox::checkStateChanged,
          [ok_button](Qt::CheckState now) { ok_button->setEnabled(now == Qt::Checked); });
#else
  connect(understood, &QCheckBox::stateChanged,
          [ok_button](int now) { ok_button->setEnabled(now == Qt::Checked); });
#endif

  // The opt-out is only remembered when the user goes ahead.
  connect(this, &QDialog::accepted, [never_again] {
    Config::SetBase(Config::MAIN_SKIP_NKIT_WARNING, never_again->isChecked());
  });
}
