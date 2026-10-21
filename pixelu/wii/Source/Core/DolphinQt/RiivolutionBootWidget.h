// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include <QDialog>

#include "Common/CommonTypes.h"
#include "DiscIO/RiivolutionParser.h"

#ifdef USE_RETRO_ACHIEVEMENTS
class HardcoreWarningWidget;
#endif  // USE_RETRO_ACHIEVEMENTS
class QPushButton;
class QVBoxLayout;

// Lets the user pick Riivolution patch options for one game before starting it. XMLs for the
// game are picked up from the user's Riivolution folder automatically, and more can be opened by
// hand. The chosen options are remembered per SD root and can also be saved as a game mod preset.
class RiivolutionBootWidget : public QDialog
{
  Q_OBJECT
public:
  explicit RiivolutionBootWidget(std::string game_id, std::optional<u16> revision,
                                 std::optional<u8> disc, std::string base_game_path,
                                 QWidget* parent = nullptr);
  ~RiivolutionBootWidget() override;

  bool ShouldBoot() const { return m_should_boot; }
  std::vector<DiscIO::Riivolution::Patch>& GetPatches() { return m_patches; }

#ifdef USE_RETRO_ACHIEVEMENTS
signals:
  void OpenAchievementSettings();
#endif  // USE_RETRO_ACHIEVEMENTS

private:
  // One opened XML, with the folder that stands in for the root of the SD card it expects.
  struct PatchFile
  {
    DiscIO::Riivolution::Disc disc;
    std::string sd_root;
    std::string xml_path;
  };

  void BuildLayout();
  void ForwardAchievementRequests();

  void AddXMLsFromUserFolder();
  void AskForXMLs();
  void AddPatchFile(std::string xml_path, std::string sd_root, DiscIO::Riivolution::Disc disc);

  // Only standard 4- and 6-character game IDs have a place in Riivolution's config folder.
  bool HasConfigFile() const;
  std::string ConfigFilePath(const std::string& sd_root) const;
  std::optional<DiscIO::Riivolution::Config> ReadSavedChoices(const std::string& sd_root);
  void WriteSavedChoices();

  void Start();
  void ExportPreset();

#ifdef USE_RETRO_ACHIEVEMENTS
  HardcoreWarningWidget* m_hc_warning = nullptr;
#endif  // USE_RETRO_ACHIEVEMENTS
  std::string m_game_id;
  std::optional<u16> m_revision;
  std::optional<u8> m_disc_number;
  std::string m_base_game_path;

  bool m_should_boot = false;
  std::vector<PatchFile> m_files;
  std::vector<DiscIO::Riivolution::Patch> m_patches;

  QVBoxLayout* m_files_layout = nullptr;
};
