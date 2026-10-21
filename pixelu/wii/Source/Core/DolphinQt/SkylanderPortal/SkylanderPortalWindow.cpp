// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/SkylanderPortal/SkylanderPortalWindow.h"

#include <memory>
#include <string>

#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/System.h"

#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/SkylanderPortal/SkylanderModifyDialog.h"

namespace
{
using FigureDump = std::array<u8, 0x40 * 0x10>;

const auto& KnownFigures()
{
  return IOS::HLE::USB::list_skylanders;
}

// A group box holding a frameless scroll area, the way every filter on the finder side looks.
struct FilterPanel
{
  QGroupBox* box;
  QVBoxLayout* box_layout;
  QScrollArea* scroller;
  QFrame* contents;
};

FilterPanel StartFilterPanel(const QString& title)
{
  FilterPanel panel;
  panel.box = new QGroupBox(title);
  panel.box_layout = new QVBoxLayout();
  panel.scroller = new QScrollArea();
  panel.scroller->setContentsMargins(0, 0, 0, 0);
  panel.scroller->setFrameStyle(QFrame::NoFrame);
  panel.contents = new QFrame();
  panel.contents->setContentsMargins(0, 0, 0, 0);
  return panel;
}

void FinishFilterPanel(const FilterPanel& panel, QLayout* contents_layout, bool centered,
                       QVBoxLayout* filters)
{
  panel.contents->setLayout(contents_layout);
  panel.scroller->setWidget(panel.contents);
  if (centered)
    panel.scroller->setAlignment(Qt::AlignHCenter);
  panel.box_layout->addWidget(panel.scroller);
  panel.box->setLayout(panel.box_layout);
  panel.box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  filters->addWidget(panel.box);
}

QFrame* SlotSeparator()
{
  auto* rule = new QFrame();
  rule->setFrameShape(QFrame::HLine);
  rule->setFrameShadow(QFrame::Sunken);
  return rule;
}

// The "id" property of whichever radio button is checked, or the fallback if none is.
template <size_t N> int CheckedRadioId(const std::array<QRadioButton*, N>& radios, int fallback)
{
  for (const QRadioButton* radio : radios)
  {
    if (radio->isChecked())
      return radio->property("id").toInt();
  }
  return fallback;
}

QStringList FigureFilePatterns()
{
  return {QStringLiteral("*.sky"), QStringLiteral("*.bin"), QStringLiteral("*.dmp"),
          QStringLiteral("*.dump")};
}

// Packs a figure's ID and variant into the value stored on its list entry.
uint PackIds(u16 id, u16 var)
{
  return (static_cast<uint>(id) << 16) | var;
}
}  // namespace

SkylanderPortalWindow::SkylanderPortalWindow(QWidget* parent) : QWidget(parent)
{
  setWindowTitle(tr("Skylanders Manager"));
  setWindowIcon(Resources::GetAppIcon());
  setObjectName(QStringLiteral("skylanders_manager"));
  setMinimumSize(QSize(650, 500));

  // Created up front: the list refresh during BuildLayout() already looks at it.
  m_only_show_collection = new QCheckBox(tr("Only Show Collection"));

  BuildLayout();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &SkylanderPortalWindow::OnEmulationStateChanged);
  installEventFilter(this);
  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));

  connect(m_skylander_list, &QListWidget::itemSelectionChanged, this,
          &SkylanderPortalWindow::UpdateCurrentIDs);

  SetUpCollectionFolder();
}

SkylanderPortalWindow::~SkylanderPortalWindow() = default;

void SkylanderPortalWindow::SetUpCollectionFolder()
{
  // Without a configured collection, use (and remember) a Skylanders folder in the user directory.
  const QString default_folder =
      QString::fromStdString(File::GetUserPath(D_USER_IDX)) + QStringLiteral("Skylanders");
  QDir folder;
  if (Config::Get(Config::MAIN_SKYLANDERS_PATH).empty())
  {
    Config::SetBase(Config::MAIN_SKYLANDERS_PATH, default_folder.toStdString());
    folder = QDir(default_folder);
  }
  else
  {
    folder = QDir(QString::fromStdString(Config::Get(Config::MAIN_SKYLANDERS_PATH)));
  }

  if (!folder.exists())
    folder.mkdir(folder.path());

  m_collection_path = QDir::toNativeSeparators(folder.path()) + QDir::separator();
  m_last_skylander_path = m_collection_path;
  m_path_edit->setText(m_collection_path);
}

void SkylanderPortalWindow::BuildLayout()
{
  auto* outer = new QVBoxLayout();
  auto* columns = new QHBoxLayout;

  // Tall enough that the element filter fits without a scroll bar.
  setMinimumWidth(770);
  setMinimumHeight(756);

  columns->addLayout(BuildSlotColumn());
  columns->addLayout(BuildFinderColumn());
  columns->setStretch(0, 1);
  columns->setStretch(1, 2);
  outer->addLayout(columns);

  // The row of actions along the bottom.
  QBoxLayout* actions = new QHBoxLayout;
  m_command_buttons = new QFrame();
  actions->setAlignment(Qt::AlignCenter);
  const std::pair<QString, std::function<void()>> commands[] = {
      {tr("Customize"), [this] { CreateSkylanderAdvanced(); }},
      {tr("Load File"), [this] { LoadFromFile(); }},
      {tr("Clear Slot"), [this] { ClearSlot(GetCurrentSlot()); }},
      {tr("Load Slot"), [this] { LoadSelected(); }},
      {tr("Modify Slot"), [this] { ModifySkylander(); }},
  };
  for (const auto& [caption, handler] : commands)
  {
    auto* button = new QPushButton(caption);
    connect(button, &QAbstractButton::clicked, this, handler);
    actions->addWidget(button);
  }
  m_command_buttons->setLayout(actions);
  outer->addWidget(m_command_buttons);

  setLayout(outer);

  RefreshList();
  m_skylander_list->setCurrentItem(m_skylander_list->item(0), QItemSelectionModel::Select);
  UpdateSlotNames();
}

QVBoxLayout* SkylanderPortalWindow::BuildSlotColumn()
{
  auto* column = new QVBoxLayout();

  auto* toggle_row = new QVBoxLayout();
  m_enabled_checkbox = new QCheckBox(tr("Emulate Skylander Portal"), this);
  m_enabled_checkbox->setChecked(Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL));
  m_emulating = Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL);
  connect(m_enabled_checkbox, &QCheckBox::toggled, this, &SkylanderPortalWindow::EmulatePortal);
  toggle_row->addWidget(m_enabled_checkbox);
  column->addLayout(toggle_row);

  // One row per portal slot: a radio button to pick it, its caption and the figure's name.
  auto* slots_box = new QGroupBox(tr("Portal Slots"));
  auto* slots_box_layout = new QVBoxLayout();
  m_group_skylanders = new QFrame();
  auto* slot_rows = new QVBoxLayout();
  auto* scroller = new QScrollArea();

  for (size_t slot = 0; slot < MAX_SKYLANDERS; ++slot)
  {
    if (slot > 0)
      slot_rows->addWidget(SlotSeparator());

    auto* caption = new QLabel(tr("Skylander %1").arg(static_cast<int>(slot) + 1));
    auto* figure_name = new QLineEdit();
    figure_name->setEnabled(false);
    m_edit_skylanders[slot] = figure_name;

    auto* picker = new QRadioButton;
    picker->setProperty("id", static_cast<int>(slot));
    m_slot_radios[slot] = picker;

    auto* row = new QHBoxLayout();
    row->addWidget(picker);
    row->addWidget(caption);
    row->addWidget(figure_name);
    slot_rows->addLayout(row);
  }
  m_slot_radios[0]->setChecked(true);

  m_group_skylanders->setLayout(slot_rows);
  m_group_skylanders->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
  scroller->setWidget(m_group_skylanders);
  scroller->setWidgetResizable(true);
  scroller->setFrameStyle(QFrame::NoFrame);
  m_group_skylanders->setVisible(Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL));
  slots_box_layout->addWidget(scroller);
  slots_box->setLayout(slots_box_layout);
  column->addWidget(slots_box);

  return column;
}

QVBoxLayout* SkylanderPortalWindow::BuildFinderColumn()
{
  auto* column = new QVBoxLayout();

  m_skylander_list = new QListWidget;
  connect(m_skylander_list, &QListWidget::itemDoubleClicked, this,
          &SkylanderPortalWindow::LoadSelected);

  // Collection folder.
  auto* folder_row = new QHBoxLayout();
  folder_row->addWidget(new QLabel(tr("Skylander Collection Path:")));
  m_path_edit = new QLineEdit;
  folder_row->addWidget(m_path_edit);
  m_path_select = new QPushButton(tr("Choose"));
  connect(m_path_edit, &QLineEdit::editingFinished, this,
          &SkylanderPortalWindow::OnCollectionPathChanged);
  connect(m_path_select, &QAbstractButton::clicked, this,
          &SkylanderPortalWindow::SelectCollectionPath);
  folder_row->addWidget(m_path_select);
  column->addLayout(folder_row);

  auto* finder = new QVBoxLayout();

  // Name search.
  auto* search_row = new QHBoxLayout;
  m_sky_search = new QLineEdit;
  m_sky_search->setClearButtonEnabled(true);
  connect(m_sky_search, &QLineEdit::textChanged, this, &SkylanderPortalWindow::RefreshList);
  search_row->addWidget(new QLabel(tr("Search:")));
  search_row->addWidget(m_sky_search);
  finder->addLayout(search_row);

  auto* list_row = new QHBoxLayout();
  auto* filters = new QVBoxLayout();

  // Which games' figures to list; all of them to begin with.
  const FilterPanel games = StartFilterPanel(tr("Game"));
  auto* game_column = new QVBoxLayout();
  for (QCheckBox*& game : m_game_filters)
  {
    game = new QCheckBox(this);
    game->setChecked(true);
    connect(game, &QCheckBox::toggled, this, &SkylanderPortalWindow::RefreshList);
    game_column->addWidget(game);
  }
  const std::pair<Game, QString> game_names[] = {
      // i18n: Figures for the game Skylanders: Spyro's Adventure. The game has the same title in
      // all countries it was released in, except Japan, where it's named スカイランダーズ
      // スパイロの大冒険.
      {Game::SpyrosAdv, tr("Spyro's Adventure")},
      // i18n: Figures for the game Skylanders: Giants. The game has the same title in all
      // countries it was released in. It was not released in Japan.
      {Game::Giants, tr("Giants")},
      // i18n: Figures for the game Skylanders: Swap Force. The game has the same title in all
      // countries it was released in. It was not released in Japan.
      {Game::SwapForce, tr("Swap Force")},
      // i18n: Figures for the game Skylanders: Trap Team. The game has the same title in all
      // countries it was released in. It was not released in Japan.
      {Game::TrapTeam, tr("Trap Team")},
      // i18n: Figures for the games Skylanders: SuperChargers (not available for the Wii) and
      // Skylanders: SuperChargers Racing (available for the Wii). The games have the same titles
      // in all countries they were released in. They were not released in Japan.
      {Game::Superchargers, tr("SuperChargers")},
  };
  for (const auto& [game, name] : game_names)
    m_game_filters[static_cast<size_t>(game)]->setText(name);
  FinishFilterPanel(games, game_column, false, filters);

  // Radio choices are owned by the window while they are made, and split over two columns:
  // even ids on the left, odd ones on the right. The first one starts checked.
  const auto make_choices = [this](auto& choices, QVBoxLayout* left, QVBoxLayout* right) {
    for (size_t i = 0; i < choices.size(); ++i)
    {
      auto* choice = new QRadioButton(this);
      choice->setProperty("id", static_cast<int>(i));
      if (i == 0)
        choice->setChecked(true);
      connect(choice, &QRadioButton::toggled, this, &SkylanderPortalWindow::RefreshList);
      choices[i] = choice;
      (i % 2 == 0 ? left : right)->addWidget(choice);
    }
  };

  // i18n: Elements are a trait of Skylanders figures. For official translations of this term,
  // check the Skylanders SuperChargers manual at https://support.activision.com/manuals
  const FilterPanel elements = StartFilterPanel(tr("Element"));
  auto* element_columns = new QHBoxLayout();
  auto* element_left = new QVBoxLayout();
  auto* element_right = new QVBoxLayout();
  make_choices(m_element_filter, element_left, element_right);
  element_columns->addLayout(element_left);
  element_columns->addSpacing(35);
  element_columns->addLayout(element_right);

  m_element_filter[0]->setText(tr("All"));
  m_element_filter[0]->setChecked(true);
  const std::pair<Element, QString> element_names[] = {
      // i18n: One of the elements in the Skylanders games. Japanese: まほう. For official
      // translations in other languages, check the SuperChargers manual at
      // https://support.activision.com/manuals
      {Element::Magic, tr("Magic")},
      // i18n: One of the elements in the Skylanders games. Japanese: 水. For official
      // translations in other languages, check the SuperChargers manual at
      // https://support.activision.com/manuals
      {Element::Water, tr("Water")},
      // i18n: One of the elements in the Skylanders games. Japanese: マシン. For official
      // translations in other languages, check the SuperChargers manual at
      // https://support.activision.com/manuals
      {Element::Tech, tr("Tech")},
      // i18n: One of the elements in the Skylanders games. Japanese: 火. For official
      // translations in other languages, check the SuperChargers manual at
      // https://support.activision.com/manuals
      {Element::Fire, tr("Fire")},
      // i18n: One of the elements in the Skylanders games. Japanese: 土. For official
      // translations in other languages, check the SuperChargers manual at
      // https://support.activision.com/manuals
      {Element::Earth, tr("Earth")},
      // i18n: One of the elements in the Skylanders games. Japanese: ライフ. For official
      // translations in other languages, check the SuperChargers manual at
      // https://support.activision.com/manuals
      {Element::Life, tr("Life")},
      // i18n: One of the elements in the Skylanders games. Japanese: 風. For official
      // translations in other languages, check the SuperChargers manual at
      // https://support.activision.com/manuals
      {Element::Air, tr("Air")},
      // i18n: One of the elements in the Skylanders games. Japanese: アンデッド. For official
      // translations in other languages, check the SuperChargers manual at
      // https://support.activision.com/manuals
      {Element::Undead, tr("Undead")},
      // i18n: One of the elements in the Skylanders games. For official translations in other
      // languages, check the SuperChargers manual at https://support.activision.com/manuals
      {Element::Dark, tr("Dark")},
      // i18n: One of the elements in the Skylanders games. For official translations in other
      // languages, check the SuperChargers manual at https://support.activision.com/manuals
      {Element::Light, tr("Light")},
      {Element::Other, tr("Other")},
  };
  for (const auto& [element, name] : element_names)
    m_element_filter[static_cast<size_t>(element)]->setText(name);
  FinishFilterPanel(elements, element_columns, true, filters);

  const FilterPanel types = StartFilterPanel(tr("Figure type"));
  auto* type_columns = new QHBoxLayout();
  auto* type_left = new QVBoxLayout();
  auto* type_right = new QVBoxLayout();
  make_choices(m_type_filter, type_left, type_right);
  type_columns->addLayout(type_left);
  type_columns->addLayout(type_right);

  m_type_filter[0]->setText(tr("All"));
  const std::pair<Type, QString> type_names[] = {
      // i18n: One of the figure types in the Skylanders games.
      {Type::Skylander, tr("Skylander")},
      // i18n: One of the figure types in the Skylanders games.
      {Type::Giant, tr("Giant")},
      // i18n: One of the figure types in the Skylanders games.
      {Type::Swapper, tr("Swapper")},
      // i18n: One of the figure types in the Skylanders games.
      {Type::TrapMaster, tr("Trap Master")},
      // i18n: One of the figure types in the Skylanders games.
      {Type::Mini, tr("Mini")},
      // i18n: One of the figure types in the Skylanders games.
      {Type::Item, tr("Item")},
      // i18n: One of the figure types in the Skylanders games.
      {Type::Trophy, tr("Trophy")},
      // i18n: One of the figure types in the Skylanders games. For official translations in
      // other languages, check the SuperChargers manual at https://support.activision.com/manuals
      {Type::Vehicle, tr("Vehicle")},
      // i18n: One of the figure types in the Skylanders games.
      {Type::Trap, tr("Trap")},
  };
  for (const auto& [type, name] : type_names)
    m_type_filter[static_cast<size_t>(type)]->setText(name);
  FinishFilterPanel(types, type_columns, true, filters);

  // Anything else.
  auto* other_box = new QGroupBox(tr("Other"));
  auto* other_layout = new QVBoxLayout;
  connect(m_only_show_collection, &QCheckBox::toggled, this, &SkylanderPortalWindow::RefreshList);
  other_layout->addWidget(m_only_show_collection);
  other_box->setLayout(other_layout);
  filters->addWidget(other_box);

  // Every filter is as wide as the element choices plus a little room for a scroll bar.
  const int filter_width = elements.contents->width() + 20;
  games.scroller->setMinimumWidth(filter_width);
  elements.scroller->setMinimumWidth(filter_width);
  other_box->setMinimumWidth(filter_width);

  filters->addStretch(1);
  list_row->addLayout(filters);
  list_row->addWidget(m_skylander_list);
  finder->addLayout(list_row);
  column->addLayout(finder);

  return column;
}

bool SkylanderPortalWindow::eventFilter(QObject*, QEvent* event)
{
  // Escape hides the window; the key is never swallowed.
  if (event->type() == QEvent::KeyPress &&
      static_cast<QKeyEvent*>(event)->matches(QKeySequence::Cancel))
  {
    hide();
  }
  return false;
}

void SkylanderPortalWindow::closeEvent(QCloseEvent*)
{
  hide();
}

void SkylanderPortalWindow::EmulatePortal(bool emulate)
{
  Config::SetBaseOrCurrent(Config::MAIN_EMULATE_SKYLANDER_PORTAL, emulate);
  m_group_skylanders->setVisible(emulate);
  m_command_buttons->setVisible(emulate);
  m_emulating = emulate;
}

void SkylanderPortalWindow::SelectCollectionPath()
{
  QString folder = QDir::toNativeSeparators(DolphinFileDialog::getExistingDirectory(
      this, tr("Select Skylander Collection"), m_collection_path));
  if (!folder.isEmpty())
  {
    folder += QDir::separator();
    m_path_edit->setText(folder);
    m_collection_path = folder;
  }
  // Stored even when the picker was cancelled.
  Config::SetBase(Config::MAIN_SKYLANDERS_PATH, folder.toStdString());

  if (m_only_show_collection->isChecked())
    RefreshList();
}

QString SkylanderPortalWindow::SuggestedFileName(const QString& folder) const
{
  const auto known = KnownFigures().find(std::make_pair(m_sky_id, m_sky_var));
  if (known != KnownFigures().end())
    return folder + QString::fromStdString(std::string(known->second.name) + ".sky");

  // i18n: This is used to create a file name. The string must end in ".sky".
  const QString pattern = tr("Unknown(%1 %2).sky");
  // The variant lands in the field-width argument, so %2 is left as it is; the file names this
  // produces are kept as they always were.
  return folder + pattern.arg(m_sky_id, static_cast<int>(m_sky_var));
}

void SkylanderPortalWindow::LoadSelected()
{
  if (!m_emulating)
    return;

  const u8 slot = GetCurrentSlot();

  QString file_path;
  if (!m_only_show_collection->isChecked())
  {
    file_path = GetFilePath(m_sky_id, m_sky_var);
  }
  else
  {
    const QListWidgetItem* picked = m_skylander_list->currentItem();
    if (!picked)
      return;
    file_path = m_collection_path + picked->text() + QStringLiteral(".sky");
  }

  if (!file_path.isEmpty())
  {
    m_last_skylander_path = QFileInfo(file_path).absolutePath() + QLatin1Char{'/'};
    LoadSkyfilePath(slot, file_path);
    return;
  }

  const auto answer =
      QMessageBox::question(this, tr("Create Skylander File"),
                            tr("Skylander not found in this collection. Create new file?"),
                            QMessageBox::Yes | QMessageBox::No);
  if (answer == QMessageBox::Yes)
    CreateSkyfile(SuggestedFileName(m_collection_path), true);
}

void SkylanderPortalWindow::LoadFromFile()
{
  const u8 slot = GetCurrentSlot();
  const QString file_path =
      DolphinFileDialog::getOpenFileName(this, tr("Select Skylander File"), m_last_skylander_path,
                                         tr("Skylander (*.sky *.bin *.dmp *.dump);;All Files (*)"));
  if (file_path.isEmpty())
    return;

  m_last_skylander_path =
      QDir::toNativeSeparators(QFileInfo(file_path).absolutePath()) + QDir::separator();
  LoadSkyfilePath(slot, file_path);
}

void SkylanderPortalWindow::CreateSkylanderAdvanced()
{
  // A small free-standing dialog for typing in any ID and variant.
  auto* dialog = new QDialog;
  auto* layout = new QVBoxLayout;

  auto* fields = new QHBoxLayout();
  auto* id_caption = new QLabel(tr("ID:"));
  auto* var_caption = new QLabel(tr("Variant:"));
  auto* id_field = new QLineEdit(tr("0"));
  auto* var_field = new QLineEdit(tr("0"));
  auto* digits_only =
      new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d*")), this);
  id_field->setValidator(digits_only);
  var_field->setValidator(digits_only);
  fields->addWidget(id_caption);
  fields->addWidget(id_field);
  fields->addWidget(var_caption);
  fields->addWidget(var_field);
  layout->addLayout(fields);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
  layout->addWidget(buttons);
  dialog->setLayout(layout);

  connect(buttons, &QDialogButtonBox::accepted, this, [this, dialog, id_field, var_field] {
    // Each value is taken even when it doesn't parse (it is then 0).
    bool parsed = false;
    m_sky_id = id_field->text().toUShort(&parsed);
    if (!parsed)
    {
      QMessageBox::warning(this, tr("Error converting value"), tr("ID entered is invalid!"),
                           QMessageBox::Ok);
      return;
    }
    m_sky_var = var_field->text().toUShort(&parsed);
    if (!parsed)
    {
      QMessageBox::warning(this, tr("Error converting value"), tr("Variant entered is invalid!"),
                           QMessageBox::Ok);
      return;
    }

    const QString target = DolphinFileDialog::getSaveFileName(
        this, tr("Create Skylander File"), SuggestedFileName(m_last_skylander_path),
        tr("Skylander (*.sky);;All Files (*)"));
    if (target.isEmpty())
      return;

    CreateSkyfile(target, true);
    dialog->accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

  dialog->show();
  dialog->raise();
}

void SkylanderPortalWindow::ModifySkylander()
{
  const std::optional<Skylander> occupant = m_sky_slots[GetCurrentSlot()];
  if (!occupant)
  {
    QMessageBox::warning(this, tr("Failed to modify Skylander!"),
                         tr("Make sure there is a Skylander in slot %1!").arg(GetCurrentSlot()),
                         QMessageBox::Ok);
    return;
  }
  // The dialog shows itself (or explains why it can't) and lives as a child of this window.
  new SkylanderModifyDialog(this, occupant->portal_slot);
}

void SkylanderPortalWindow::ClearSlot(u8 slot)
{
  const std::optional<Skylander> occupant = m_sky_slots[slot];
  if (!occupant)
    return;

  if (!Core::System::GetInstance().GetSkylanderPortal().RemoveSkylander(occupant->portal_slot))
  {
    QMessageBox::warning(this, tr("Failed to clear Skylander!"),
                         tr("Failed to clear the Skylander from slot %1!").arg(slot),
                         QMessageBox::Ok);
    return;
  }

  m_sky_slots[slot].reset();
  if (m_only_show_collection->isChecked())
    RefreshList();
  UpdateSlotNames();
}

void SkylanderPortalWindow::OnCollectionPathChanged()
{
  m_collection_path = m_path_edit->text();
  Config::SetBase(Config::MAIN_SKYLANDERS_PATH, m_collection_path.toStdString());
  RefreshList();
}

void SkylanderPortalWindow::OnEmulationStateChanged(Core::State state)
{
  // The portal can only be plugged in or out while nothing is running.
  m_enabled_checkbox->setEnabled(state == Core::State::Uninitialized);
}

void SkylanderPortalWindow::UpdateCurrentIDs()
{
  const uint packed = m_skylander_list->currentItem()->data(1).toUInt();
  if (packed == 0xFFFFFFFF)
    return;
  m_sky_id = static_cast<u16>(packed >> 16);
  m_sky_var = static_cast<u16>(packed & 0xFFFF);
}

void SkylanderPortalWindow::ScanCollection(const CollectionVisitor& visit) const
{
  auto& portal = Core::System::GetInstance().GetSkylanderPortal();
  for (const QFileInfo& file : QDir(m_collection_path).entryInfoList(FigureFilePatterns()))
  {
    // Files that can't be opened, or are too short to be a dump, are skipped.
    File::IOFile dump_file(file.filePath().toStdString(), "r+b");
    if (!dump_file)
      continue;
    FigureDump dump;
    if (!dump_file.ReadBytes(dump.data(), dump.size()))
      continue;
    if (!visit(file, portal.CalculateIDs(dump)))
      return;
  }
}

void SkylanderPortalWindow::AddListEntry(const QString& name, std::pair<const u16, const u16> ids,
                                         bool dark_theme)
{
  auto* entry = new QListWidgetItem(name);
  entry->setBackground(GetBaseColor(ids, dark_theme));
  entry->setForeground(QBrush(dark_theme ? QColor(220, 220, 220) : QColor(0, 0, 0)));
  entry->setData(1, PackIds(ids.first, ids.second));
  m_skylander_list->addItem(entry);
}

void SkylanderPortalWindow::RefreshList()
{
  const bool dark_theme = Settings::Instance().IsThemeDark();

  const int previous_row = m_skylander_list->currentRow();
  m_skylander_list->clear();

  if (!m_only_show_collection->isChecked())
  {
    for (const auto& [ids, info] : KnownFigures())
    {
      const QString name = tr(info.name);
      if (PassesFilter(name, ids.first, ids.second))
        AddListEntry(name, ids, dark_theme);
    }
  }
  else
  {
    ScanCollection([&](const QFileInfo& file, std::pair<u16, u16> ids) {
      if (PassesFilter(file.baseName(), ids.first, ids.second))
        AddListEntry(file.baseName(), ids, dark_theme);
      return true;
    });
  }

  // Put the selection back on the same row; if the list got shorter than that, on its last row.
  const int count = m_skylander_list->count();
  int row = previous_row;
  if (count < previous_row)
  {
    if (count == 0)
      return;
    row = count - 1;
  }
  m_skylander_list->setCurrentItem(m_skylander_list->item(row), QItemSelectionModel::Select);
}

void SkylanderPortalWindow::CreateSkyfile(const QString& path, bool load_after)
{
  {
    // Write the blank figure and close the file again before it gets loaded.
    IOS::HLE::USB::SkylanderFigure figure(path.toStdString());
    if (!figure.Create(m_sky_id, m_sky_var))
    {
      QMessageBox::warning(
          this, tr("Failed to create Skylander file!"),
          tr("Failed to create Skylander file:\n%1\n\nThe Skylander may already be on the portal.")
              .arg(path),
          QMessageBox::Ok);
      return;
    }
    figure.Close();
  }
  m_last_skylander_path = QFileInfo(path).absolutePath() + QLatin1Char{'/'};

  if (load_after)
    LoadSkyfilePath(GetCurrentSlot(), path);
}

void SkylanderPortalWindow::LoadSkyfilePath(u8 slot, const QString& path)
{
  File::IOFile dump_file(path.toStdString(), "r+b");
  if (!dump_file)
  {
    QMessageBox::warning(this, tr("Failed to open the Skylander file!"),
                         tr("Failed to open the Skylander file:\n%1\n\nThe file may already be in "
                            "use on the portal.")
                             .arg(path),
                         QMessageBox::Ok);
    return;
  }
  FigureDump dump;
  if (!dump_file.ReadBytes(dump.data(), dump.size()))
  {
    QMessageBox::warning(
        this, tr("Failed to read the Skylander file!"),
        tr("Failed to read the Skylander file:\n%1\n\nThe file was too small.").arg(path),
        QMessageBox::Ok);
    return;
  }

  // Whatever was in the slot goes first.
  ClearSlot(slot);

  auto& portal = Core::System::GetInstance().GetSkylanderPortal();
  const std::pair<u16, u16> ids = portal.CalculateIDs(dump);
  const u8 portal_slot =
      portal.LoadSkylander(std::make_unique<IOS::HLE::USB::SkylanderFigure>(std::move(dump_file)));
  if (portal_slot == 0xFF)
  {
    QMessageBox::warning(this, tr("Failed to load the Skylander file!"),
                         tr("Failed to load the Skylander file:\n%1").arg(path), QMessageBox::Ok);
    return;
  }

  m_sky_slots[slot] = Skylander{portal_slot, ids.first, ids.second};
  RefreshList();
  UpdateSlotNames();
}

void SkylanderPortalWindow::UpdateSlotNames()
{
  for (size_t slot = 0; slot < MAX_SKYLANDERS; ++slot)
  {
    const std::optional<Skylander>& occupant = m_sky_slots[slot];
    QString shown;
    if (!occupant)
    {
      shown = tr("None");
    }
    else if (const auto known = KnownFigures().find({occupant->m_sky_id, occupant->m_sky_var});
             known != KnownFigures().end())
    {
      shown = QString::fromStdString(known->second.name);
    }
    else
    {
      // i18n: "Var" is short for "variant"
      shown = tr("Unknown (Id:%1 Var:%2)").arg(occupant->m_sky_id).arg(occupant->m_sky_var);
    }
    m_edit_skylanders[slot]->setText(shown);
  }
}

bool SkylanderPortalWindow::PassesFilter(const QString& name, u16 id, u16 var) const
{
  // Only known figures are ever listed.
  const auto known = KnownFigures().find(std::make_pair(id, var));
  if (known == KnownFigures().end())
    return false;
  const IOS::HLE::USB::SkyData& info = known->second;

  const auto game = static_cast<size_t>(info.game);
  if (game >= NUM_SKYLANDER_GAMES || !m_game_filters[game]->isChecked())
    return false;

  if (!name.contains(m_sky_search->text(), Qt::CaseInsensitive))
    return false;

  // Id 0 on the element and type filters means "All".
  const int element = GetElementRadio();
  if (element != 0 && static_cast<Element>(element) != info.element)
    return false;

  const int type = GetTypeRadio();
  if (type != 0 && static_cast<Type>(type) != info.type)
    return false;

  return true;
}

QString SkylanderPortalWindow::GetFilePath(u16 id, u16 var) const
{
  QString match;
  ScanCollection([&](const QFileInfo& file, std::pair<u16, u16> ids) {
    if (ids.first != id || ids.second != var)
      return true;
    match = file.filePath();
    return false;
  });
  return match;
}

u8 SkylanderPortalWindow::GetCurrentSlot() const
{
  return static_cast<u8>(CheckedRadioId(m_slot_radios, 0));
}

int SkylanderPortalWindow::GetElementRadio() const
{
  return CheckedRadioId(m_element_filter, -1);
}

int SkylanderPortalWindow::GetTypeRadio() const
{
  return CheckedRadioId(m_type_filter, -1);
}

QBrush SkylanderPortalWindow::GetBaseColor(std::pair<const u16, const u16> ids, bool dark_theme)
{
  // Each game's figures get their own tint, in a dark and a light version.
  struct Tint
  {
    QColor dark;
    QColor light;
  };
  const Tint neutral{QColor(32, 32, 32), QColor(255, 255, 255)};

  Tint tint = neutral;
  const auto known = KnownFigures().find(ids);
  if (known != KnownFigures().end())
  {
    switch (known->second.game)
    {
    case Game::SpyrosAdv:
      tint = {QColor(10, 42, 90), QColor(240, 255, 240)};
      break;
    case Game::Giants:
      tint = {QColor(120, 16, 12), QColor(255, 240, 215)};
      break;
    case Game::SwapForce:
      tint = {QColor(28, 45, 12), QColor(240, 245, 255)};
      break;
    case Game::TrapTeam:
      tint = {QColor(0, 56, 76), QColor(255, 240, 240)};
      break;
    case Game::Superchargers:
      tint = {QColor(90, 12, 12), QColor(247, 228, 215)};
      break;
    }
  }
  return QBrush(dark_theme ? tint.dark : tint.light);
}
