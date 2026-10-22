// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NANDRepairDialog.h"

#include <string>

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/TitleDatabase.h"
#include "Core/WiiUtils.h"
#include "DiscIO/WiiSaveBanner.h"
#include "DolphinQt/Resources.h"

namespace
{
QLabel* WrappedLabel(const QString& text)
{
  auto* const label = new QLabel(text);
  label->setWordWrap(true);
  return label;
}

// One line per title: its ID, then the best name available. The title database is asked
// first; failing that, the save banner gives a name and, when it has any text, a description.
std::string DescribeTitles(const WiiUtils::NANDCheckResult& result)
{
  Core::TitleDatabase database;
  const DiscIO::Language language = SConfig::GetInstance().GetCurrentLanguage(true);

  std::string listing;
  for (const u64 id : result.titles_to_remove)
  {
    std::string line = fmt::format("{:016x}", id);

    const std::string known_name = database.GetChannelName(id, language);
    if (known_name.empty())
    {
      const DiscIO::WiiSaveBanner banner(id);
      if (banner.IsValid())
      {
        line += " - " + banner.GetName();
        const std::string about = banner.GetDescription();
        if (!StripWhitespace(about).empty())
          line += " - " + about;
      }
    }
    else
    {
      line += " - " + known_name;
    }

    listing += line;
    listing += '\n';
  }
  return listing;
}
}  // namespace

NANDRepairDialog::NANDRepairDialog(const WiiUtils::NANDCheckResult& result, QWidget* parent)
    : QDialog(parent)
{
  setWindowTitle(tr("NAND Check"));
  setWindowIcon(Resources::GetAppIcon());

  // Text and buttons on the right.
  auto* const text_column = new QVBoxLayout();
  text_column->addWidget(
      WrappedLabel(tr("The emulated NAND is damaged. System titles such as the Wii Menu and "
                      "the Wii Shop Channel may not work correctly.")));

  if (!result.titles_to_remove.empty())
  {
    text_column->addWidget(
        WrappedLabel(tr("WARNING: Fixing this NAND requires the deletion of titles that have "
                        "incomplete data on the NAND, including all associated save data. "
                        "By continuing, the following title(s) will be removed:")));

    auto* const doomed_titles = new QPlainTextEdit(QString::fromStdString(DescribeTitles(result)));
    doomed_titles->setReadOnly(true);
    text_column->addWidget(doomed_titles);

    text_column->addWidget(WrappedLabel(tr("Launching these titles may also fix the issues.")));
  }

  text_column->addWidget(WrappedLabel(tr("Do you want to try to repair the NAND?")));

  auto* const answers = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No);
  text_column->addWidget(answers);

  // A large warning icon on the left, pinned to the top.
  auto* const warning_icon = new QLabel;
  warning_icon->setPixmap(
      QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(100));
  warning_icon->setAlignment(Qt::AlignTop);

  auto* const row = new QHBoxLayout();
  row->addWidget(warning_icon);
  row->addSpacing(10);
  row->addLayout(text_column);
  setLayout(row);

  resize(600, 400);

  connect(answers->button(QDialogButtonBox::Yes), &QPushButton::clicked, this, &QDialog::accept);
  connect(answers->button(QDialogButtonBox::No), &QPushButton::clicked, this, &QDialog::reject);
}
