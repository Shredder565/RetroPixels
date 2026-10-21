// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/NewPatchDialog.h"

#include <algorithm>
#include <array>
#include <utility>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "Core/PatchEngine.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

// One entry box: the three hex fields and the entry they edit. Handlers
// hold a pointer to it, so it never moves.
struct NewPatchEntry
{
  NewPatchEntry() = default;
  NewPatchEntry(const NewPatchEntry&) = delete;
  NewPatchEntry(NewPatchEntry&&) = delete;
  NewPatchEntry& operator=(const NewPatchEntry&) = delete;
  NewPatchEntry& operator=(NewPatchEntry&&) = delete;

  QLineEdit* address = nullptr;
  QLineEdit* value = nullptr;
  QLineEdit* comparand = nullptr;
  PatchEngine::PatchEntry entry;
};

namespace
{
bool IsHex(const QLineEdit* field)
{
  bool ok = false;
  field->text().toUInt(&ok, 16);
  return ok;
}

// Parses a field as hex, drawing it bold red when it doesn't parse.
u32 ReadHexField(QLineEdit* field, const QString& text)
{
  bool ok = false;
  const u32 number = text.toUInt(&ok, 16);

  QFont look;
  look.setBold(!ok);
  QPalette colours;
  if (!ok)
    colours.setColor(QPalette::Text, Qt::red);
  field->setFont(look);
  field->setPalette(colours);

  return number;
}

QString AsHex(u32 number)
{
  return QStringLiteral("%1").arg(number, 8, 16, QLatin1Char('0'));
}
}  // namespace

NewPatchDialog::NewPatchDialog(QWidget* parent, PatchEngine::Patch& patch)
    : QDialog(parent), m_patch(patch)
{
  setWindowTitle(tr("Patch Editor"));

  m_name = new QLineEdit;
  m_name->setPlaceholderText(tr("Patch name"));
  m_name->setText(QString::fromStdString(m_patch.name));

  m_entries_widget = new QWidget;
  m_entries_column = new QVBoxLayout;
  auto* const scroller = new QScrollArea;
  m_entries_widget->setLayout(m_entries_column);
  scroller->setWidget(m_entries_widget);
  scroller->setWidgetResizable(true);

  m_add_entry = new QPushButton(tr("Add"));
  m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);

  auto* const grid = new QGridLayout;
  grid->addWidget(m_name, 0, 0);
  grid->addWidget(scroller, 1, 0);
  grid->addWidget(m_add_entry, 2, 0);
  grid->addWidget(m_buttons, 3, 0);
  setLayout(grid);

  // The name is written to the patch as it is typed.
  connect(m_name, &QLineEdit::textEdited,
          [this](const QString& text) { m_patch.name = text.toStdString(); });
  connect(m_add_entry, &QPushButton::clicked, this, &NewPatchDialog::AppendBlankEntry);
  connect(m_buttons, &QDialogButtonBox::accepted, this, &NewPatchDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  for (const PatchEngine::PatchEntry& entry : m_patch.entries)
    m_entries_column->addWidget(MakeEntryBox(entry));

  // A new patch starts with one blank entry and switched on.
  if (m_patch.entries.empty())
  {
    AppendBlankEntry();
    m_patch.enabled = true;
  }
}

NewPatchDialog::~NewPatchDialog() = default;

void NewPatchDialog::AppendBlankEntry()
{
  m_entries_column->addWidget(MakeEntryBox({}));
}

QGroupBox* NewPatchDialog::MakeEntryBox(const PatchEngine::PatchEntry& entry)
{
  auto* const box = new QGroupBox();

  auto* const size_box = new QGroupBox(tr("Type"));
  auto* const size_row = new QHBoxLayout;
  auto* const remove = new QPushButton(tr("Remove"));

  const std::array<std::pair<QRadioButton*, PatchEngine::PatchType>, 3> sizes{{
      {new QRadioButton(tr("8-bit")), PatchEngine::PatchType::Patch8Bit},
      {new QRadioButton(tr("16-bit")), PatchEngine::PatchType::Patch16Bit},
      {new QRadioButton(tr("32-bit")), PatchEngine::PatchType::Patch32Bit},
  }};
  for (const auto& [button, type] : sizes)
    size_row->addWidget(button);
  size_box->setLayout(size_row);

  auto* const address = new QLineEdit;
  auto* const value = new QLineEdit;
  auto* const comparand = new QLineEdit;

  NewPatchEntry* const state = m_entries.emplace_back(std::make_unique<NewPatchEntry>()).get();
  state->address = address;
  state->value = value;
  state->comparand = comparand;
  state->entry = entry;

  auto* const conditional = new QCheckBox(tr("Conditional"));
  auto* const comparand_caption = new QLabel(tr("Comparand:"));

  auto* const grid = new QGridLayout;
  grid->addWidget(size_box, 0, 0, 1, -1);
  grid->addWidget(new QLabel(tr("Address:")), 1, 0);
  grid->addWidget(address, 1, 1);
  grid->addWidget(new QLabel(tr("Value:")), 2, 0);
  grid->addWidget(value, 2, 1);
  grid->addWidget(conditional, 3, 0, 1, -1);
  grid->addWidget(comparand_caption, 4, 0);
  grid->addWidget(comparand, 4, 1);
  grid->addWidget(remove, 5, 0, 1, -1);
  box->setLayout(grid);

  connect(address, &QLineEdit::textEdited, [state](const QString& text) {
    state->entry.address = ReadHexField(state->address, text);
  });
  connect(value, &QLineEdit::textEdited, [state](const QString& text) {
    state->entry.value = ReadHexField(state->value, text);
  });
  connect(comparand, &QLineEdit::textEdited, [state](const QString& text) {
    state->entry.comparand = ReadHexField(state->comparand, text);
  });

  // The last remaining entry cannot be removed.
  connect(remove, &QPushButton::clicked, [this, box, state] {
    if (m_entries.size() <= 1)
      return;
    box->setVisible(false);
    m_entries_column->removeWidget(box);
    box->deleteLater();
    const auto found = std::ranges::find_if(
        m_entries, [state](const std::unique_ptr<NewPatchEntry>& e) { return e.get() == state; });
    m_entries.erase(found);
  });

  for (const auto& [button, type] : sizes)
  {
    connect(button, &QRadioButton::toggled, [state, kind = type](bool checked) {
      if (checked)
        state->entry.type = kind;
    });
  }
  for (const auto& [button, type] : sizes)
    button->setChecked(entry.type == type);

  connect(conditional, &QCheckBox::toggled, [state, comparand_caption, comparand](bool on) {
    state->entry.conditional = on;
    comparand_caption->setVisible(on);
    comparand->setVisible(on);
  });
  conditional->setChecked(entry.conditional);
  comparand_caption->setVisible(entry.conditional);
  comparand->setVisible(entry.conditional);

  address->setText(AsHex(entry.address));
  value->setText(AsHex(entry.value));
  comparand->setText(AsHex(entry.comparand));

  return box;
}

bool NewPatchDialog::AllFieldsValid() const
{
  // The comparand only counts for conditional entries.
  return std::ranges::all_of(m_entries, [](const std::unique_ptr<NewPatchEntry>& e) {
    return IsHex(e->address) && IsHex(e->value) && (!e->entry.conditional || IsHex(e->comparand));
  });
}

void NewPatchDialog::accept()
{
  if (m_name->text().isEmpty())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("You have to enter a name."));
    return;
  }

  if (!AllFieldsValid())
  {
    ModalMessageBox::critical(
        this, tr("Error"),
        tr("Some values you provided are invalid.\nPlease check the highlighted values."));
    return;
  }

  m_patch.entries.clear();
  for (const std::unique_ptr<NewPatchEntry>& e : m_entries)
    m_patch.entries.emplace_back(e->entry);

  QDialog::accept();
}
