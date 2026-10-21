// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/AboutDialog.h"

#include <QLabel>
#include <QStringList>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtGlobal>
#ifdef HAVE_SDL3
#include <SDL3/SDL_version.h>
#endif

#include "Common/Version.h"

#include "DolphinQt/Resources.h"

namespace
{
QString Paragraph(const QStringList& lines, const QString& style = {})
{
  const QString open = style.isEmpty() ? QStringLiteral("<p>") :
                                         QStringLiteral("<p style='%1'>").arg(style);
  return open + QLatin1Char('\n') + lines.join(QLatin1Char('\n')) + QStringLiteral("\n</p>");
}

QString Link(const QString& url, const QString& text)
{
  return QStringLiteral("<a href='%1'>%2</a>").arg(url, text);
}
}  // namespace

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent)
{
  // PixelU: our own window title (not translated).
  setWindowTitle(QStringLiteral("About PixelU Wii"));

  QString branch = QString::fromStdString(Common::GetScmBranchStr());
  if (const int ahead = Common::GetScmCommitsAheadMaster(); ahead > 0)
  {
    // i18n: how many version control commits this build has on top of another branch.
    const QString ahead_text =
        tr("%1 commit(s) ahead of %2").arg(ahead).arg(QStringLiteral("master"));
    branch = tr("%1 (%2)").arg(branch, ahead_text);
  }

#ifdef HAVE_SDL3
  const int sdl = SDL_GetVersion();
  const QString sdl_line = tr("Using SDL %1").arg(QStringLiteral("%1.%2.%3")
                                                      .arg(SDL_VERSIONNUM_MAJOR(sdl))
                                                      .arg(SDL_VERSIONNUM_MINOR(sdl))
                                                      .arg(SDL_VERSIONNUM_MICRO(sdl)));
#else
  const QString sdl_line = tr("SDL disabled");
#endif

  const QString version = QString::fromUtf8(Common::GetScmDescStr().c_str());
  const QString revision = QString::fromUtf8(Common::GetScmRevGitStr().c_str());

  // The page is a sequence of paragraphs separated by blank lines. The last one (the links) is
  // left open, as it always has been.
  QStringList page;
  page << QStringLiteral("<p style='font-size:38pt; font-weight:400;'>PixelU Wii</p>");
  page << QStringLiteral("<p style='font-size:18pt;'>Version %1</p>")
              .arg(version);
  // PixelU: what this build is and where its source lives.
  page << Paragraph({QStringLiteral("PixelU Wii is RetroPixels' GameCube and Wii engine. It began "
                                    "as a build of Dolphin<br>"),
                     QStringLiteral("and has been rewritten as RetroPixels' own code. The parts "
                                    "still from Dolphin<br>"),
                     QStringLiteral("and its community data are listed in COPYING. Thanks to the "
                                    "Dolphin Team.<br>"),
                     QStringLiteral("Source and changes: ") +
                         Link(QStringLiteral("https://github.com/shredder565/retropixels/tree/"
                                             "claude/wiiu-emulator-urpcen/pixelu/wii"),
                              QStringLiteral("github.com/shredder565/retropixels (pixelu/wii)"))});
  // i18n: "Branch" is the version control term.
  page << Paragraph({tr("Branch: %1").arg(branch) + QStringLiteral("<br>"),
                     tr("Revision: %1").arg(revision) + QStringLiteral("<br><br>"),
                     tr("Using Qt %1").arg(QStringLiteral(QT_VERSION_STR)) +
                         QStringLiteral("<br>"),
                     sdl_line},
                    QStringLiteral("font-size: small;"));
  page << Paragraph({QStringLiteral("Dolphin: ") + Link(QStringLiteral("https://dolphin-emu.org/"),
                                                        QStringLiteral("dolphin-emu.org"))});
  page << Paragraph(
      {tr("This software should not be used to play games you do not legally own.")});
  page << QStringLiteral("<p>\n") +
              Link(QStringLiteral("https://github.com/shredder565/retropixels/blob/"
                                  "claude/wiiu-emulator-urpcen/pixelu/wii/COPYING"),
                   tr("License")) +
              QStringLiteral(" |\n") +
              Link(QStringLiteral("https://github.com/shredder565/retropixels/issues"),
                   tr("Support"));

  auto* const info = new QLabel(QLatin1Char('\n') + page.join(QStringLiteral("\n\n")) +
                                QLatin1Char('\n'));
  info->setTextInteractionFlags(Qt::TextBrowserInteraction);
  info->setOpenExternalLinks(true);

  const QString notice =
      QStringLiteral("Contains code and data from Dolphin, \u00A9 2003-2024+ Dolphin Team. "
                     "\u201cGameCube\u201d and \u201cWii\u201d are trademarks of Nintendo. "
                     "PixelU Wii is not affiliated with Nintendo in any way.");
  auto* const footer = new QLabel(QStringLiteral("<small>%1</small>").arg(notice));

  auto* const logo = new QLabel();
  logo->setPixmap(Resources::GetAppIcon().pixmap(200, 200));
  logo->setContentsMargins(30, 0, 30, 0);

  auto* const outer = new QVBoxLayout;
  auto* const top_row = new QHBoxLayout;

  setLayout(outer);
  outer->setSizeConstraint(QLayout::SetFixedSize);
  outer->addLayout(top_row);
  outer->addWidget(footer);
  footer->setAlignment(Qt::AlignCenter);
  footer->setContentsMargins(0, 15, 0, 0);

  top_row->setAlignment(Qt::AlignLeft);
  top_row->addWidget(logo);
  top_row->addWidget(info);
}
