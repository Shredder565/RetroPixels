// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/SDLHints/SDLHintsWindow.h"

#include <array>
#include <memory>
#include <string>

#include <QDialogButtonBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipCheckBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"

namespace
{
constexpr const char* HINTS_SECTION = "SDL_Hints";

// One switch on the main tab and the hint strings it stands for.
struct HintSwitch
{
  ToolTipCheckBox* box;
  const Config::Info<std::string>* hint;
  const char* checked_value;
  const char* unchecked_value;
};
}  // namespace

SDLHintsWindow::SDLHintsWindow(QWidget* parent) : QDialog(parent)
{
  setMinimumWidth(300);
  setMinimumHeight(270);

  m_close_box = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(m_close_box, &QDialogButtonBox::rejected, this, &SDLHintsWindow::Close);

  QWidget* const advanced_tab = BuildAdvancedTab();
  QWidget* const main_tab = BuildMainTab();

  m_tabs = new QTabWidget();
  m_tabs->addTab(main_tab, tr("Main"));
  m_tabs->addTab(advanced_tab, tr("Advanced"));
  m_shown_tab = TAB_MAIN;
  m_tabs->setCurrentIndex(m_shown_tab);
  connect(m_tabs, &QTabWidget::currentChanged, this, &SDLHintsWindow::SwitchTab);

  auto* const restart_note =
      new QLabel(tr("Dolphin must be restarted for these changes to take effect."));
  restart_note->setWordWrap(true);

  auto* const column = new QVBoxLayout();
  column->addWidget(QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxWarning, restart_note),
                    0);
  column->addWidget(m_tabs, 1);
  column->addWidget(m_close_box, 0, Qt::AlignBottom | Qt::AlignRight);
  setLayout(column);

  setWindowTitle(tr("SDL Controller Settings"));
}

QSize SDLHintsWindow::sizeHint() const
{
  return QSize(450, 0);
}

QWidget* SDLHintsWindow::BuildAdvancedTab()
{
  m_table = new QTableWidget(0, 2);
  QHeaderView* const header = m_table->horizontalHeader();
  m_table->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
  m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_table->setSelectionMode(QTableWidget::SingleSelection);
  // The name column can be dragged; the value column takes the rest.
  header->setSectionResizeMode(0, QHeaderView::Interactive);
  header->setSectionResizeMode(1, QHeaderView::Fixed);
  header->setMinimumSectionSize(60);
  m_table->verticalHeader()->setVisible(false);
  header->installEventFilter(this);
  connect(header, &QHeaderView::sectionResized, this, &SDLHintsWindow::OnNameColumnResized);

  LoadTable();

  auto* const add_button = new NonDefaultQPushButton(tr("Add"));
  connect(add_button, &QPushButton::pressed, this, &SDLHintsWindow::AppendRow);

  m_remove_button = new NonDefaultQPushButton(tr("Remove"));
  m_remove_button->setEnabled(false);
  connect(m_remove_button, &QPushButton::pressed, this, &SDLHintsWindow::DeleteCurrentRow);
  connect(m_table, &QTableWidget::itemSelectionChanged, this,
          &SDLHintsWindow::UpdateRemoveButton);

  auto* const row_buttons = new QDialogButtonBox;
  row_buttons->setContentsMargins(0, 0, 5, 5);
  row_buttons->addButton(add_button, QDialogButtonBox::ActionRole);
  row_buttons->addButton(m_remove_button, QDialogButtonBox::ActionRole);

  auto* const column = new QVBoxLayout();
  column->addWidget(m_table);
  column->addWidget(row_buttons);

  auto* const page = new QFrame();
  page->setLayout(column);
  return page;
}

QWidget* SDLHintsWindow::BuildMainTab()
{
  m_directinput = new ToolTipCheckBox(tr("Enable DirectInput Detection"));
  m_directinput->SetDescription(
      tr("Controls whether SDL should use DirectInput for detecting controllers. "
         "Enabling this fixes hotplug detection issues with DualSense controllers but causes "
         "Dolphin to hang up on shutdown when using certain 8BitDo controllers.<br><br>"
         "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_joycon_pairs = new ToolTipCheckBox(tr("Use Joy-Con Pairs as a Single Controller"));
  m_joycon_pairs->SetDescription(
      tr("Controls whether SDL should treat a pair of Joy-Con as a single controller "
         "or as two separate controllers.<br><br>"
         "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_joycon_sideways = new ToolTipCheckBox(tr("Sideways Joy-Con"));
  m_joycon_sideways->SetDescription(
      tr("Defines the default orientation for individual Joy-Con. This setting has no effect "
         "when Use Joy-Con Pairs as a Single Controller is enabled.<br><br>"
         "<dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));

  m_ps5_leds = new ToolTipCheckBox(tr("Enable DualSense Player LEDs"));
  m_ps5_leds->SetDescription(
      tr("Controls whether the player LEDs should be lit to indicate which player is "
         "associated with a DualSense controller.<br><br>"
         "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>"));

  // Each switch writes its hint to the base layer as soon as it is toggled.
  // The sideways switch is stored inverted, as the "vertical" hint.
  const std::array<HintSwitch, 4> switches{{
      {m_directinput, &Config::MAIN_SDL_HINT_JOYSTICK_DIRECTINPUT, "1", "0"},
      {m_joycon_pairs, &Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1", "0"},
      {m_joycon_sideways, &Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "0", "1"},
      {m_ps5_leds, &Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1", "0"},
  }};
  for (const HintSwitch& entry : switches)
  {
    connect(entry.box, &ToolTipCheckBox::toggled, [entry](bool checked) {
      Config::SetBase(*entry.hint, checked ? entry.checked_value : entry.unchecked_value);
    });
  }

  auto* const column = new QVBoxLayout();
  column->setContentsMargins(10, 10, 10, 10);
  for (const HintSwitch& entry : switches)
    column->addWidget(entry.box);
  column->addStretch(1);

  auto* const page = new QFrame();
  page->setLayout(column);

  LoadSwitches();
  return page;
}

void SDLHintsWindow::LoadTable()
{
  m_table->setRowCount(0);

  const std::shared_ptr<Config::Layer> base = Config::GetLayer(Config::LayerType::Base);
  const Config::Section& hints = base->GetSection(Config::System::Main, HINTS_SECTION);
  for (const auto& [location, value] : hints)
  {
    // Deleted entries keep their slot with no value.
    if (!value)
      continue;
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(location.key)));
    m_table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(*value)));
  }
}

void SDLHintsWindow::StoreTable()
{
  // Drop every stored hint, then write back the rows that have both a name
  // and a value.
  const std::shared_ptr<Config::Layer> base = Config::GetLayer(Config::LayerType::Base);
  Config::Section hints = base->GetSection(Config::System::Main, HINTS_SECTION);
  for (auto& entry : hints)
    entry.second.reset();

  for (int row = 0; row < m_table->rowCount(); ++row)
  {
    const QTableWidgetItem* const name_cell = m_table->item(row, 0);
    const QTableWidgetItem* const value_cell = m_table->item(row, 1);
    if (name_cell == nullptr || value_cell == nullptr)
      continue;

    const QString name = name_cell->text().trimmed();
    const QString value = value_cell->text().trimmed();
    if (name.isEmpty() || value.isEmpty())
      continue;

    const Config::Info<std::string> hint{{Config::System::Main, HINTS_SECTION, name.toStdString()},
                                         ""};
    Config::SetBase(hint, value.toStdString());
  }
}

void SDLHintsWindow::LoadSwitches()
{
  // A missing or unexpected value shows the switch in its SDL default state.
  SignalBlocking(m_directinput)
      ->setChecked(Config::GetBase(Config::MAIN_SDL_HINT_JOYSTICK_DIRECTINPUT) != "0");
  SignalBlocking(m_joycon_pairs)
      ->setChecked(Config::GetBase(Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS) != "0");
  SignalBlocking(m_joycon_sideways)
      ->setChecked(Config::GetBase(Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS) != "1");
  SignalBlocking(m_ps5_leds)
      ->setChecked(Config::GetBase(Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED) != "0");
}

void SDLHintsWindow::AppendRow()
{
  m_table->insertRow(m_table->rowCount());
  m_table->scrollToBottom();
}

void SDLHintsWindow::DeleteCurrentRow()
{
  m_table->removeRow(m_table->selectionModel()->currentIndex().row());
}

void SDLHintsWindow::UpdateRemoveButton()
{
  m_remove_button->setEnabled(m_table->selectionModel()->hasSelection());
}

void SDLHintsWindow::Close()
{
  SwitchTab(TAB_LEAVING);
  reject();
}

// The table is saved when the advanced tab is left, and each tab reloads
// from the config when it is shown.
void SDLHintsWindow::SwitchTab(int next)
{
  if (m_shown_tab == TAB_ADVANCED)
    StoreTable();

  if (next == TAB_MAIN)
    LoadSwitches();
  else if (next == TAB_ADVANCED)
    LoadTable();

  m_shown_tab = next;
}

void SDLHintsWindow::OnNameColumnResized(int column, int before, int after)
{
  if (column != 0 || before == after)
    return;

  QHeaderView* const header = m_table->horizontalHeader();
  const int total = header->size().width();
  header->setMaximumSectionSize(total - header->minimumSectionSize());
  header->resizeSection(1, total - after);
}

bool SDLHintsWindow::eventFilter(QObject* watched, QEvent* event)
{
  // Keep both columns filling the header when it changes width.
  if (qobject_cast<QHeaderView*>(watched) != nullptr && event->type() == QEvent::Resize)
  {
    const int total = static_cast<QResizeEvent*>(event)->size().width();
    QHeaderView* const header = m_table->horizontalHeader();
    header->setMaximumSectionSize(total - header->minimumSectionSize());
    header->resizeSection(0, total - header->sectionSize(1));
  }
  return QDialog::eventFilter(watched, event);
}
