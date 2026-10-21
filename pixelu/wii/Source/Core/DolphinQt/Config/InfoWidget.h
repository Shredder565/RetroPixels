// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>

#include <QWidget>

#include "UICommon/GameFile.h"

namespace DiscIO
{
class Volume;
}

class QComboBox;
class QFormLayout;
class QGroupBox;
class QLineEdit;
class QPixmap;
class QTextEdit;

// The Info tab of the game properties: file, game and banner details.
class InfoWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit InfoWidget(const UICommon::GameFile& game);
  ~InfoWidget() override;

private:
  QGroupBox* FileSection();
  QGroupBox* GameSection();
  QGroupBox* BannerSection();
  QWidget* BannerPreview(const QPixmap& image);
  void BuildLanguageBox();
  void ShowBannerText();
  void ExportBanner();

  QLineEdit* ReadOnlyField(const QString& text);
  QLineEdit* ReadOnlyField(const std::string& text = "");

  std::unique_ptr<DiscIO::Volume> m_volume;
  UICommon::GameFile m_game;
  QComboBox* m_language;
  QLineEdit* m_banner_name = nullptr;
  QLineEdit* m_banner_maker = nullptr;
  QTextEdit* m_banner_description = nullptr;
};
