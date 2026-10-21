// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/InfinityBase/InfinityBaseWindow.h"

#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>

#include "Common/IOFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/IOS/USB/Emulated/Infinity.h"
#include "Core/System.h"

#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

using FigureUIPosition = IOS::HLE::USB::FigureUIPosition;

namespace
{
// Native file pickers don't always remember where they were, so the folder of the last figure
// file opened or created is kept here (with a trailing slash) and offered next time.
QString s_figure_folder;

void RememberFolderOf(const QString& file)
{
  s_figure_folder = QFileInfo(file).absolutePath() + QLatin1Char('/');
}

QFrame* Separator()
{
  auto* rule = new QFrame();
  rule->setFrameShape(QFrame::HLine);
  rule->setFrameShadow(QFrame::Sunken);
  return rule;
}

constexpr bool Between(u32 value, u32 low, u32 high)
{
  return value > low && value < high;
}

// Infinity figure numbers come in ranges: characters, play sets and power discs, abilities.
bool FitsPosition(FigureUIPosition position, u32 figure)
{
  const bool character = figure < 0x1E847F;
  const bool play_set = Between(figure, 0x1E8480, 0x2DC6BF);
  const bool ability = Between(figure, 0x2DC6C0, 0x3D08FF);
  const bool power_disc = Between(figure, 0x3D0900, 0x4C4B3F);

  switch (position)
  {
  case FigureUIPosition::HexagonDiscOne:
    return play_set || power_disc;
  case FigureUIPosition::HexagonDiscTwo:
  case FigureUIPosition::HexagonDiscThree:
    return power_disc;
  case FigureUIPosition::PlayerOne:
  case FigureUIPosition::PlayerTwo:
    return character;
  case FigureUIPosition::P1AbilityOne:
  case FigureUIPosition::P1AbilityTwo:
  case FigureUIPosition::P2AbilityOne:
  case FigureUIPosition::P2AbilityTwo:
    return ability;
  }
  return false;
}
}  // namespace

InfinityBaseWindow::InfinityBaseWindow(QWidget* parent) : QWidget(parent)
{
  // i18n: Window for managing Disney Infinity figures
  setWindowTitle(tr("Infinity Manager"));
  setWindowIcon(Resources::GetAppIcon());
  setObjectName(QStringLiteral("infinity_manager"));
  setMinimumSize(QSize(700, 200));

  BuildLayout();

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &InfinityBaseWindow::OnEmulationStateChanged);

  installEventFilter(this);

  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

InfinityBaseWindow::~InfinityBaseWindow() = default;

QLineEdit* InfinityBaseWindow::FigureField(FigureUIPosition position) const
{
  return m_edit_figures[static_cast<u8>(position)];
}

void InfinityBaseWindow::BuildLayout()
{
  auto* column = new QVBoxLayout();

  auto* presence_box = new QGroupBox();
  auto* presence_row = new QHBoxLayout();
  presence_row->setAlignment(Qt::AlignHCenter);
  m_plugged_in = new QCheckBox(tr("Emulate Infinity Base"), this);
  m_plugged_in->setChecked(Config::Get(Config::MAIN_EMULATE_INFINITY_BASE));
  connect(m_plugged_in, &QCheckBox::toggled, this, &InfinityBaseWindow::EmulateBase);
  presence_row->addWidget(m_plugged_in);
  presence_box->setLayout(presence_row);
  column->addWidget(presence_box);

  m_positions_box = new QGroupBox(tr("Active Infinity Figures:"));
  auto* rows = new QVBoxLayout();
  auto* scroller = new QScrollArea();

  const std::pair<QString, FigureUIPosition> positions[] = {
      {tr("Play Set/Power Disc"), FigureUIPosition::HexagonDiscOne},
      {tr("Power Disc Two"), FigureUIPosition::HexagonDiscTwo},
      {tr("Power Disc Three"), FigureUIPosition::HexagonDiscThree},
      {tr("Player One"), FigureUIPosition::PlayerOne},
      {tr("Player One Ability One"), FigureUIPosition::P1AbilityOne},
      {tr("Player One Ability Two"), FigureUIPosition::P1AbilityTwo},
      {tr("Player Two"), FigureUIPosition::PlayerTwo},
      {tr("Player Two Ability One"), FigureUIPosition::P2AbilityOne},
      {tr("Player Two Ability Two"), FigureUIPosition::P2AbilityTwo},
  };
  bool first = true;
  for (const auto& [caption, position] : positions)
  {
    if (!first)
      rows->addWidget(Separator());
    first = false;
    AddPositionRow(rows, caption, position);
  }

  m_positions_box->setLayout(rows);
  scroller->setWidget(m_positions_box);
  scroller->setWidgetResizable(true);
  m_positions_box->setVisible(Config::Get(Config::MAIN_EMULATE_INFINITY_BASE));
  column->addWidget(scroller);
  setLayout(column);
}

void InfinityBaseWindow::AddPositionRow(QVBoxLayout* rows, const QString& caption,
                                        FigureUIPosition position)
{
  auto* row = new QHBoxLayout();

  auto* name = new QLabel(caption);
  auto* clear = new QPushButton(tr("Clear"));
  auto* create = new QPushButton(tr("Create"));
  auto* load = new QPushButton(tr("Load"));

  // Shows the loaded figure's name; read-only.
  auto* field = new QLineEdit();
  m_edit_figures[static_cast<u8>(position)] = field;
  field->setEnabled(false);
  field->setText(tr("None"));

  connect(clear, &QAbstractButton::clicked, this, [this, position] { ClearFigure(position); });
  connect(create, &QAbstractButton::clicked, this, [this, position] { CreateFigure(position); });
  connect(load, &QAbstractButton::clicked, this, [this, position] { LoadFigure(position); });

  for (QWidget* part :
       {static_cast<QWidget*>(name), static_cast<QWidget*>(field), static_cast<QWidget*>(clear),
        static_cast<QWidget*>(create), static_cast<QWidget*>(load)})
  {
    row->addWidget(part);
  }
  rows->addLayout(row);
}

void InfinityBaseWindow::ClearFigure(FigureUIPosition slot)
{
  Core::System& system = Core::System::GetInstance();
  FigureField(slot)->setText(tr("None"));
  system.GetInfinityBase().RemoveFigure(slot);
}

void InfinityBaseWindow::LoadFigure(FigureUIPosition slot)
{
  const QString picked = DolphinFileDialog::getOpenFileName(
      this, tr("Select Figure File"), s_figure_folder, QStringLiteral("Infinity Figure (*.bin);;"));
  if (picked.isEmpty())
    return;

  RememberFolderOf(picked);
  LoadFigurePath(slot, picked);
}

void InfinityBaseWindow::CreateFigure(FigureUIPosition slot)
{
  CreateFigureDialog creator(this, slot);
  if (creator.exec() != CreateFigureDialog::Accepted)
    return;
  LoadFigurePath(slot, creator.GetFilePath());
}

void InfinityBaseWindow::LoadFigurePath(FigureUIPosition slot, const QString& path)
{
  File::IOFile figure_file(path.toStdString(), "r+b");
  if (!figure_file)
  {
    QMessageBox::warning(
        this, tr("Failed to open the Infinity file!"),
        tr("Failed to open the Infinity file:\n%1\n\nThe file may already be in use on the base.")
            .arg(path),
        QMessageBox::Ok);
    return;
  }

  std::array<u8, IOS::HLE::USB::INFINITY_NUM_BLOCKS * IOS::HLE::USB::INFINITY_BLOCK_SIZE> image;
  if (!figure_file.ReadBytes(image.data(), image.size()))
  {
    QMessageBox::warning(
        this, tr("Failed to read the Infinity file!"),
        tr("Failed to read the Infinity file:\n%1\n\nThe file was too small.").arg(path),
        QMessageBox::Ok);
    return;
  }

  // Take off whatever was there, then put the new figure on; the base keeps the open file.
  IOS::HLE::USB::InfinityBase& base = Core::System::GetInstance().GetInfinityBase();
  base.RemoveFigure(slot);
  const std::string figure_name = base.LoadFigure(image, std::move(figure_file), slot);
  FigureField(slot)->setText(QString::fromStdString(figure_name));
}

void InfinityBaseWindow::EmulateBase(bool emulate)
{
  Config::SetBaseOrCurrent(Config::MAIN_EMULATE_INFINITY_BASE, emulate);
  m_positions_box->setVisible(emulate);
}

void InfinityBaseWindow::OnEmulationStateChanged(Core::State state)
{
  // The base can only be plugged in or out while nothing is running.
  m_plugged_in->setEnabled(state == Core::State::Uninitialized);
}

CreateFigureDialog::CreateFigureDialog(QWidget* parent, FigureUIPosition slot) : QDialog(parent)
{
  setWindowTitle(tr("Infinity Figure Creator"));
  setObjectName(QStringLiteral("infinity_creator"));
  setMinimumSize(QSize(500, 150));
  auto* column = new QVBoxLayout;

  // Characters that fit this position, then a catch-all entry.
  auto* character_list = new QComboBox();
  QStringList names;
  u32 first_number = 0;
  for (const auto& [name, number] : IOS::HLE::USB::InfinityBase::GetFigureList())
  {
    if (!FitsPosition(slot, number))
      continue;
    const QString shown = QString::fromUtf8(name);
    character_list->addItem(shown, QVariant(number));
    names << shown;
    if (first_number == 0)
      first_number = number;
  }
  character_list->addItem(tr("--Unknown--"), QVariant(0xFFFF));
  character_list->setEditable(true);
  character_list->setInsertPolicy(QComboBox::NoInsert);

  auto* name_search = new QCompleter(names, this);
  name_search->setCaseSensitivity(Qt::CaseInsensitive);
  name_search->setCompletionMode(QCompleter::PopupCompletion);
  name_search->setFilterMode(Qt::MatchContains);
  character_list->setCompleter(name_search);

  column->addWidget(character_list);
  column->addWidget(Separator());

  auto* number_row = new QHBoxLayout();
  auto* number_label = new QLabel(tr("Figure Number:"));
  auto* number_field = new QLineEdit(QString::number(first_number));
  auto* digits_only =
      new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d*")), this);
  number_field->setValidator(digits_only);
  number_row->addWidget(number_label);
  number_row->addWidget(number_field);
  column->addLayout(number_row);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
  column->addWidget(buttons);

  setLayout(column);

  // Picking a character fills in its number (the catch-all entry fills in 65535).
  connect(character_list, &QComboBox::currentIndexChanged, this,
          [character_list, number_field](int index) {
            const u32 number = character_list->itemData(index).toUInt();
            if (number != 0xFFFFFFFF)
              number_field->setText(QString::number(number));
          });

  connect(buttons, &QDialogButtonBox::accepted, this,
          [this, number_field] { OnCreate(number_field->text()); });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  connect(name_search, QOverload<const QString&>::of(&QCompleter::activated), this,
          [character_list](const QString& text) {
            character_list->setCurrentText(text);
            character_list->setCurrentIndex(character_list->findText(text));
          });
}

void CreateFigureDialog::OnCreate(const QString& number_text)
{
  bool parsed = false;
  const auto number = static_cast<u32>(number_text.toULong(&parsed));
  if (!parsed)
  {
    QMessageBox::warning(this, tr("Error converting value"), tr("Character entered is invalid!"),
                         QMessageBox::Ok);
    return;
  }

  // Suggest a file named after the character, in the last figure folder.
  IOS::HLE::USB::InfinityBase& base = Core::System::GetInstance().GetInfinityBase();
  QString suggestion = s_figure_folder;
  const std::string known_name = base.FindFigure(number);
  if (known_name.empty())
  {
    // i18n: This is used to create a file name. The string must end in ".bin".
    const QString unknown_pattern = tr("Unknown(%1).bin");
    suggestion += unknown_pattern.arg(number);
  }
  else
  {
    suggestion += QString::fromStdString(known_name + ".bin");
  }

  m_file_path = DolphinFileDialog::getSaveFileName(this, tr("Create Infinity File"), suggestion,
                                                   tr("Infinity Object (*.bin);;"));
  if (m_file_path.isEmpty())
    return;

  if (!base.CreateFigure(m_file_path.toStdString(), number))
  {
    QMessageBox::warning(
        this, tr("Failed to create Infinity file"),
        tr("Blank figure creation failed at:\n%1\n\nTry again with a different character.")
            .arg(m_file_path),
        QMessageBox::Ok);
    return;
  }

  RememberFolderOf(m_file_path);
  accept();
}

QString CreateFigureDialog::GetFilePath() const
{
  return m_file_path;
}
