// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/PropertiesDialog.h"

#include <memory>

#include <QDialogButtonBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"

#include "DolphinQt/Config/ARCodeWidget.h"
#include "DolphinQt/Config/FilesystemWidget.h"
#include "DolphinQt/Config/GameConfigWidget.h"
#include "DolphinQt/Config/GeckoCodeWidget.h"
#include "DolphinQt/Config/GraphicsModListWidget.h"
#include "DolphinQt/Config/InfoWidget.h"
#include "DolphinQt/Config/PatchesWidget.h"
#include "DolphinQt/Config/VerifyWidget.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"

#include "UICommon/GameFile.h"

PropertiesDialog::PropertiesDialog(QWidget* parent, const UICommon::GameFile& game)
    : StackedSettingsWindow{parent}, m_filepath(game.GetFilePath())
{
  const auto q = [](const std::string& s) { return QString::fromStdString(s); };
  setWindowTitle(QStringLiteral("%1: %2 - %3")
                     .arg(q(game.GetFileName()), q(game.GetGameID()), q(game.GetLongName())));

  auto* const info = new InfoWidget(game);
  auto* const ar_codes = new ARCodeWidget(game.GetGameID(), game.GetRevision());
  auto* const gecko_codes =
      new GeckoCodeWidget(game.GetGameID(), game.GetGameTDBID(), game.GetRevision());
  auto* const patches = new PatchesWidget(game);
  auto* const game_config = new GameConfigWidget(game);
  auto* const mods = new GraphicsModListWidget(game);

  connect(gecko_codes, &GeckoCodeWidget::OpenGeneralSettings, this,
          &PropertiesDialog::OpenGeneralSettings);
  connect(ar_codes, &ARCodeWidget::OpenGeneralSettings, this,
          &PropertiesDialog::OpenGeneralSettings);
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(ar_codes, &ARCodeWidget::OpenAchievementSettings, this,
          &PropertiesDialog::OpenAchievementSettings);
  connect(gecko_codes, &GeckoCodeWidget::OpenAchievementSettings, this,
          &PropertiesDialog::OpenAchievementSettings);
  connect(patches, &PatchesWidget::OpenAchievementSettings, this,
          &PropertiesDialog::OpenAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  connect(mods, &GraphicsModListWidget::OpenGraphicsSettings, this,
          &PropertiesDialog::OpenGraphicsSettings);

  // Only the info and mod pages scroll; that keeps the dialog's minimum
  // size reasonable.
  AddWrappedPane(info, tr("Info"));
  AddPane(game_config, tr("Game Config"));
  AddPane(patches, tr("Patches"));
  AddPane(ar_codes, tr("AR Codes"));
  AddPane(gecko_codes, tr("Gecko Codes"));
  AddWrappedPane(mods, tr("Graphics Mods"));

  // Disc images and WADs also get Verify, and discs a file browser.
  const DiscIO::Platform platform = game.GetPlatform();
  if (platform != DiscIO::Platform::ELFOrDOL)
  {
    const std::shared_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(game.GetFilePath());
    if (volume != nullptr)
    {
      QWidget* const verify_page = new VerifyWidget(volume);
      AddPane(verify_page, tr("Verify"));
      if (DiscIO::IsDisc(game.GetPlatform()))
      {
        QWidget* const files_page = new FilesystemWidget(volume);
        AddPane(files_page, tr("Filesystem"));
      }
    }
  }

  connect(this, &QDialog::rejected, mods, &GraphicsModListWidget::SaveToDisk);

  OnDoneCreatingPanes();
}
