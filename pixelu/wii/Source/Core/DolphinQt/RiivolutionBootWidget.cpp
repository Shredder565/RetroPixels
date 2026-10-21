// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/RiivolutionBootWidget.h"

#include <memory>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMetaType>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "DiscIO/GameModDescriptor.h"
#include "DiscIO/RiivolutionParser.h"
#include "DiscIO/RiivolutionPatcher.h"
#include "DolphinQt/Config/HardcoreWarningWidget.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

// Stored with each combo box entry: which choice of which option it stands for. Choice 0 is
// "Disabled"; the XML's own choices follow from 1.
struct GuiRiivolutionPatchIndex
{
  size_t disc;
  size_t section;
  size_t option;
  size_t choice;
};

Q_DECLARE_METATYPE(GuiRiivolutionPatchIndex);

namespace
{
// A real SD card holds the XML one folder below the card's root, so that folder is assumed here
// too. The user's Riivolution folder is the fallback when there is no folder above.
std::string GuessSDRoot(const std::string& xml_path)
{
  QDir folder = QFileInfo(QString::fromStdString(xml_path)).dir();
  if (!folder.cdUp())
    return File::GetUserPath(D_RIIVOLUTION_IDX);
  return folder.absolutePath().toStdString();
}
}  // namespace

RiivolutionBootWidget::RiivolutionBootWidget(std::string game_id, std::optional<u16> revision,
                                             std::optional<u8> disc, std::string base_game_path,
                                             QWidget* parent)
    : QDialog(parent), m_game_id(std::move(game_id)), m_revision(revision), m_disc_number(disc),
      m_base_game_path(std::move(base_game_path))
{
  setWindowTitle(tr("Start with Riivolution Patches"));

  BuildLayout();
  ForwardAchievementRequests();
  AddXMLsFromUserFolder();

  resize(QSize(400, 600));
}

RiivolutionBootWidget::~RiivolutionBootWidget() = default;

void RiivolutionBootWidget::BuildLayout()
{
#ifdef USE_RETRO_ACHIEVEMENTS
  m_hc_warning = new HardcoreWarningWidget(this);
#endif  // USE_RETRO_ACHIEVEMENTS

  auto* const open_button = new QPushButton(tr("Open Riivolution XML..."));
  auto* const start_button = new QPushButton(tr("Start"));
  start_button->setDefault(true);
  auto* const preset_button = new QPushButton(tr("Save as Preset..."));

  // Every opened XML gets a box in a scrollable column, kept packed at the top.
  m_files_layout = new QVBoxLayout();
  auto* const column = new QVBoxLayout();
  column->addLayout(m_files_layout);
  column->addStretch();
  auto* const files_box = new QGroupBox();
  files_box->setLayout(column);
  auto* const scroller = new QScrollArea();
  scroller->setWidget(files_box);
  scroller->setWidgetResizable(true);

  auto* const button_row = new QHBoxLayout();
  button_row->addStretch();
  for (QPushButton* const button : {open_button, preset_button, start_button})
    button_row->addWidget(button, 0, Qt::AlignRight);

  auto* const page = new QVBoxLayout();
#ifdef USE_RETRO_ACHIEVEMENTS
  page->addWidget(m_hc_warning);
#endif  // USE_RETRO_ACHIEVEMENTS
  page->addWidget(scroller);
  page->addLayout(button_row);
  setLayout(page);

  connect(open_button, &QPushButton::clicked, this, &RiivolutionBootWidget::AskForXMLs);
  connect(start_button, &QPushButton::clicked, this, &RiivolutionBootWidget::Start);
  connect(preset_button, &QPushButton::clicked, this, &RiivolutionBootWidget::ExportPreset);
}

void RiivolutionBootWidget::ForwardAchievementRequests()
{
#ifdef USE_RETRO_ACHIEVEMENTS
  // Opening the achievement settings from the warning also closes this dialog.
  connect(m_hc_warning, &HardcoreWarningWidget::OpenAchievementSettings, this,
          &RiivolutionBootWidget::OpenAchievementSettings);
  connect(m_hc_warning, &HardcoreWarningWidget::OpenAchievementSettings, this,
          &RiivolutionBootWidget::reject);
#endif  // USE_RETRO_ACHIEVEMENTS
}

void RiivolutionBootWidget::AddXMLsFromUserFolder()
{
  // The user's Riivolution folder plays the SD root here, with the XMLs in its riivolution/.
  const std::string& sd_root = File::GetUserPath(D_RIIVOLUTION_IDX);
  const std::optional<DiscIO::Riivolution::Config> saved = ReadSavedChoices(sd_root);

  for (const std::string& xml : Common::DoFileSearch(sd_root + "riivolution", ".xml"))
  {
    std::optional<DiscIO::Riivolution::Disc> disc = DiscIO::Riivolution::ParseFile(xml);
    if (!disc.has_value())
      continue;
    if (!disc->IsValidForGame(m_game_id, m_revision, m_disc_number))
      continue;

    if (saved.has_value())
      DiscIO::Riivolution::ApplyConfigDefaults(&disc.value(), saved.value());
    AddPatchFile(xml, sd_root, std::move(disc.value()));
  }
}

void RiivolutionBootWidget::AskForXMLs()
{
  const QStringList picked = QFileDialog::getOpenFileNames(
      this, tr("Select Riivolution XML file"),
      QString::fromStdString(File::GetUserPath(D_RIIVOLUTION_IDX)),
      QStringLiteral("%1 (*.xml);;%2 (*)").arg(tr("Riivolution XML files")).arg(tr("All Files")));

  for (const QString& picked_path : picked)
  {
    std::string xml = picked_path.toStdString();

    std::optional<DiscIO::Riivolution::Disc> disc = DiscIO::Riivolution::ParseFile(xml);
    if (!disc.has_value())
    {
      ModalMessageBox::warning(
          this, tr("Failed loading XML."),
          tr("Did not recognize %1 as a valid Riivolution XML file.").arg(picked_path));
      continue;
    }
    if (!disc->IsValidForGame(m_game_id, m_revision, m_disc_number))
    {
      ModalMessageBox::warning(
          this, tr("Invalid game."),
          tr("The patches in %1 are not for the selected game or game revision.").arg(picked_path));
      continue;
    }

    std::string sd_root = GuessSDRoot(xml);
    const std::optional<DiscIO::Riivolution::Config> saved = ReadSavedChoices(sd_root);
    if (saved.has_value())
      DiscIO::Riivolution::ApplyConfigDefaults(&disc.value(), saved.value());
    AddPatchFile(std::move(xml), std::move(sd_root), std::move(disc.value()));
  }
}

void RiivolutionBootWidget::AddPatchFile(std::string xml_path, std::string sd_root,
                                         DiscIO::Riivolution::Disc disc)
{
  const size_t file_number = m_files.size();
  m_files.push_back(PatchFile{std::move(disc), std::move(sd_root), std::move(xml_path)});
  const PatchFile& file = m_files.back();

  auto* const file_box = new QGroupBox(QFileInfo(QString::fromStdString(file.xml_path)).fileName());
  auto* const file_column = new QVBoxLayout();
  file_box->setLayout(file_column);

  // The SD root, shown read-only, with a button to point it elsewhere.
  auto* const root_row = new QHBoxLayout();
  auto* const root_display = new QLineEdit(QString::fromStdString(file.sd_root));
  root_display->setReadOnly(true);
  auto* const root_browse = new QPushButton(tr("..."));
  root_row->addWidget(new QLabel(tr("SD Root:")), 0);
  root_row->addWidget(root_display, 0);
  root_row->addWidget(root_browse, 0);
  file_column->addLayout(root_row);

  connect(root_browse, &QPushButton::clicked, this, [this, root_display, file_number] {
    const QString chosen = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(
        this, tr("Select the Virtual SD Card Root"), root_display->text()));
    if (chosen.isEmpty())
      return;
    root_display->setText(chosen);
    m_files[file_number].sd_root = chosen.toStdString();
  });

  // One box per section, one combo box per option.
  const auto& sections = file.disc.m_sections;
  for (size_t s = 0; s < sections.size(); ++s)
  {
    auto* const section_box = new QGroupBox(QString::fromStdString(sections[s].m_name));
    auto* const option_grid = new QGridLayout();
    section_box->setLayout(option_grid);

    const auto& options = sections[s].m_options;
    for (size_t o = 0; o < options.size(); ++o)
    {
      auto* const name = new QLabel(QString::fromStdString(options[o].m_name));
      auto* const choices = new QComboBox();

      choices->addItem(tr("Disabled"),
                       QVariant::fromValue(GuiRiivolutionPatchIndex{file_number, s, o, 0}));
      const auto& option_choices = options[o].m_choices;
      for (size_t c = 0; c < option_choices.size(); ++c)
      {
        choices->addItem(QString::fromStdString(option_choices[c].m_name),
                         QVariant::fromValue(GuiRiivolutionPatchIndex{file_number, s, o, c + 1}));
      }

      // Show the saved or default choice; out-of-range values leave "Disabled" showing.
      const u32 preselected = options[o].m_selected_choice;
      if (preselected <= option_choices.size())
        choices->setCurrentIndex(static_cast<int>(preselected));

      // Connected only now, so showing the initial choice above writes nothing back.
      connect(choices, &QComboBox::currentIndexChanged, this, [this, choices](int) {
        const auto where = choices->currentData().value<GuiRiivolutionPatchIndex>();
        DiscIO::Riivolution::Option& target =
            m_files[where.disc].disc.m_sections[where.section].m_options[where.option];
        target.m_selected_choice = static_cast<u32>(where.choice);
      });

      const int grid_row = static_cast<int>(o);
      option_grid->addWidget(name, grid_row, 0, 1, 1);
      option_grid->addWidget(choices, grid_row, 1, 1, 1);
    }

    file_column->addWidget(section_box);
  }

  m_files_layout->addWidget(file_box);
}

bool RiivolutionBootWidget::HasConfigFile() const
{
  return m_game_id.size() == 4 || m_game_id.size() == 6;
}

std::string RiivolutionBootWidget::ConfigFilePath(const std::string& sd_root) const
{
  // Riivolution keys its saved choices on the first four characters of the game ID.
  return fmt::format("{}/riivolution/config/{}.xml", sd_root, m_game_id.substr(0, 4));
}

std::optional<DiscIO::Riivolution::Config>
RiivolutionBootWidget::ReadSavedChoices(const std::string& sd_root)
{
  if (!HasConfigFile())
    return std::nullopt;
  return DiscIO::Riivolution::ParseConfigFile(ConfigFilePath(sd_root));
}

void RiivolutionBootWidget::WriteSavedChoices()
{
  if (!HasConfigFile())
    return;

  // XMLs sharing an SD root share a config file, so gather the choices per root first.
  std::unordered_map<std::string, DiscIO::Riivolution::Config> per_root;
  for (const PatchFile& file : m_files)
  {
    std::vector<DiscIO::Riivolution::ConfigOption>& saved = per_root[file.sd_root].m_options;
    for (const auto& section : file.disc.m_sections)
    {
      for (const auto& option : section.m_options)
      {
        // Options without an ID are keyed on their section and option names run together.
        std::string key = option.m_id;
        if (key.empty())
          key = section.m_name + option.m_name;
        saved.push_back(DiscIO::Riivolution::ConfigOption{std::move(key), option.m_selected_choice});
      }
    }
  }

  for (const auto& [sd_root, config] : per_root)
    DiscIO::Riivolution::WriteConfigFile(ConfigFilePath(sd_root), config);
}

void RiivolutionBootWidget::Start()
{
  WriteSavedChoices();

  // Each patch reads its files from the host, relative to its XML's SD root.
  m_patches.clear();
  for (const PatchFile& file : m_files)
  {
    for (DiscIO::Riivolution::Patch& patch : file.disc.GeneratePatches(m_game_id))
    {
      patch.m_file_data_loader = std::make_shared<DiscIO::Riivolution::FileDataLoaderHostFS>(
          file.sd_root, file.disc.m_xml_path, patch.m_root);
      m_patches.push_back(std::move(patch));
    }
  }

  m_should_boot = true;
  close();
}

void RiivolutionBootWidget::ExportPreset()
{
  DiscIO::GameModDescriptor preset;
  preset.base_file = m_base_game_path;

  DiscIO::GameModDescriptorRiivolution riivolution;
  for (const PatchFile& file : m_files)
  {
    // An XML whose current choices produce no patches would add nothing to the preset.
    if (file.disc.GeneratePatches(m_game_id).empty())
      continue;

    DiscIO::GameModDescriptorRiivolutionPatch& entry = riivolution.patches.emplace_back();
    entry.xml = file.xml_path;
    entry.root = file.sd_root;
    for (const auto& section : file.disc.m_sections)
    {
      for (const auto& option : section.m_options)
      {
        DiscIO::GameModDescriptorRiivolutionPatchOption& choice = entry.options.emplace_back();
        choice.section_name = section.m_name;
        if (option.m_id.empty())
          choice.option_name = option.m_name;
        else
          choice.option_id = option.m_id;
        choice.choice = option.m_selected_choice;
      }
    }
  }
  if (!riivolution.patches.empty())
    preset.riivolution = std::move(riivolution);

  const QString start_folder =
      QFileInfo(QString::fromStdString(m_base_game_path)).dir().absolutePath();
  const QString target = QFileDialog::getSaveFileName(this, tr("Save Preset"), start_folder,
                                                      QStringLiteral("%1 (*.json);;%2 (*)")
                                                          .arg(tr("Dolphin Game Mod Preset"))
                                                          .arg(tr("All Files")));
  if (target.isEmpty())
    return;

  // The preset is named after its file, without the extension.
  std::string name = QFileInfo(target).fileName().toStdString();
  if (const size_t dot = name.rfind('.'); dot != std::string::npos)
    name.resize(dot);
  preset.display_name = std::move(name);

  DiscIO::WriteGameModDescriptorFile(target.toStdString(), preset, true);
}
