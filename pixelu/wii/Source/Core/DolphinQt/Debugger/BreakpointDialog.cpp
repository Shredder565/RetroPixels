// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/BreakpointDialog.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Expression.h"
#include "DolphinQt/Debugger/BreakpointWidget.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

namespace
{
QString Hex(u32 value)
{
  return QString::number(value, 16);
}

bool ReadHex(const QLineEdit* field, u32* value)
{
  bool ok = false;
  *value = field->text().toUInt(&ok, 16);
  return ok;
}
}  // namespace

BreakpointDialog::BreakpointDialog(BreakpointWidget* parent)
    : QDialog(parent), m_owner(parent), m_mode(OpenMode::New)
{
  setWindowTitle(tr("New Breakpoint"));
  BuildLayout();
  WireUp();

  SyncKindBoxes();
  SyncRangeFields();
}

BreakpointDialog::BreakpointDialog(BreakpointWidget* parent, const TBreakPoint* breakpoint)
    : QDialog(parent), m_owner(parent), m_mode(OpenMode::EditBreakPoint)
{
  setWindowTitle(tr("Edit Breakpoint"));
  BuildLayout();
  WireUp();

  m_code_address->setText(Hex(breakpoint->address));
  if (breakpoint->condition)
    m_condition->setText(QString::fromStdString(breakpoint->condition->GetText()));
  SetActionChoice(breakpoint->break_on_hit, breakpoint->log_on_hit);

  SyncKindBoxes();
  SyncRangeFields();
}

BreakpointDialog::BreakpointDialog(BreakpointWidget* parent, const TMemCheck* memcheck)
    : QDialog(parent), m_owner(parent), m_mode(OpenMode::EditMemCheck)
{
  setWindowTitle(tr("Edit Breakpoint"));
  BuildLayout();
  WireUp();

  m_first_address->setText(Hex(memcheck->start_address));
  if (memcheck->is_ranged)
  {
    m_single_address->setChecked(false);
    m_address_range->setChecked(true);
    m_last_address->setText(Hex(memcheck->end_address));
  }
  else
  {
    m_last_address->setText(Hex(memcheck->start_address + 1));
  }
  if (memcheck->condition)
    m_condition->setText(QString::fromStdString(memcheck->condition->GetText()));

  const bool reads = memcheck->is_break_on_read;
  const bool writes = memcheck->is_break_on_write;
  // Same order as the action choice below; an exclusive button that is already checked cannot be
  // unchecked, so the order decides what stays checked when nothing matches.
  m_on_read->setChecked(reads && !writes);
  m_on_write->setChecked(writes && !reads);
  m_on_access->setChecked(reads && writes);
  SetActionChoice(memcheck->break_on_hit, memcheck->log_on_hit);

  SyncKindBoxes();
  SyncRangeFields();
}

void BreakpointDialog::SetActionChoice(bool break_on_hit, bool log_on_hit)
{
  m_break_only->setChecked(break_on_hit && !log_on_hit);
  m_log_only->setChecked(log_on_hit && !break_on_hit);
  m_log_and_break->setChecked(break_on_hit && log_on_hit);
}

void BreakpointDialog::BuildLayout()
{
  m_dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                          QDialogButtonBox::Help);
  auto* kind_group = new QButtonGroup(this);

  // --- Instruction section
  m_code_section = new QWidget;
  auto* code_grid = new QGridLayout;
  m_code_kind = new QRadioButton(tr("Instruction Breakpoint"));
  kind_group->addButton(m_code_kind);
  m_code_box = new QGroupBox;
  m_code_address = new QLineEdit;

  auto* code_row = new QHBoxLayout;
  m_code_box->setLayout(code_row);
  code_row->addWidget(new QLabel(tr("Address:")));
  code_row->addWidget(m_code_address);

  code_grid->addWidget(m_code_kind, 0, 0, 1, 1);
  code_grid->addWidget(m_code_box, 1, 0, 1, 2);
  m_code_section->setLayout(code_grid);

  // --- Memory section
  m_memory_section = new QWidget;
  auto* memory_grid = new QGridLayout;
  m_memory_kind = new QRadioButton(tr("Memory Breakpoint"));
  kind_group->addButton(m_memory_kind);
  m_memory_box = new QGroupBox;

  auto* extent_group = new QButtonGroup(this);
  m_single_address = new QRadioButton(tr("Address"));
  m_single_address->setChecked(true);
  extent_group->addButton(m_single_address);
  // i18n: A range of memory addresses
  m_address_range = new QRadioButton(tr("Range"));
  extent_group->addButton(m_address_range);

  m_first_address = new QLineEdit;
  m_last_address = new QLineEdit;
  m_first_caption = new QLabel;  // text depends on the extent, see SyncRangeFields
  m_last_caption = new QLabel(tr("To:"));

  // i18n: This is a selectable condition when adding a breakpoint
  m_on_read = new QRadioButton(tr("Read"));
  // i18n: This is a selectable condition when adding a breakpoint
  m_on_write = new QRadioButton(tr("Write"));
  // i18n: This is a selectable condition when adding a breakpoint
  m_on_access = new QRadioButton(tr("Read or Write"));
  m_on_write->setChecked(true);

  // i18n: This is a selectable action when adding a breakpoint
  m_log_only = new QRadioButton(tr("Write to Log"));
  // i18n: This is a selectable action when adding a breakpoint
  m_break_only = new QRadioButton(tr("Break"));
  // i18n: This is a selectable action when adding a breakpoint
  m_log_and_break = new QRadioButton(tr("Write to Log and Break"));
  m_log_and_break->setChecked(true);

  auto* extent_grid = new QGridLayout;
  m_memory_box->setLayout(extent_grid);
  extent_grid->addWidget(m_single_address, 0, 0);
  extent_grid->addWidget(m_address_range, 0, 3);
  extent_grid->addWidget(m_first_caption, 1, 0);
  extent_grid->addWidget(m_first_address, 1, 1);
  extent_grid->addWidget(m_last_caption, 1, 2);
  extent_grid->addWidget(m_last_address, 1, 3);

  // i18n: If a condition is set for a breakpoint, the condition becoming true is a prerequisite for
  // triggering the breakpoint.
  auto* access_box = new QGroupBox(tr("Condition"));
  auto* access_row = new QHBoxLayout;
  access_box->setLayout(access_row);
  extent_grid->addWidget(access_box, 2, 0, 1, -1);
  access_row->addWidget(m_on_read);
  access_row->addWidget(m_on_write);
  access_row->addWidget(m_on_access);

  memory_grid->addWidget(m_memory_kind, 0, 0);
  memory_grid->addWidget(m_memory_box, 1, 0);
  m_memory_section->setLayout(memory_grid);

  // --- Action section
  auto* action_box = new QGroupBox(tr("Action"));

  auto* condition_row = new QHBoxLayout;
  m_condition = new QLineEdit();
  // i18n: If a condition is set for a breakpoint, the condition becoming true is a prerequisite for
  // triggering the breakpoint.
  condition_row->addWidget(new QLabel(tr("Condition:")));
  condition_row->addWidget(m_condition);

  auto* choice_row = new QHBoxLayout;
  choice_row->addWidget(m_log_only);
  choice_row->addWidget(m_break_only);
  choice_row->addWidget(m_log_and_break);

  auto* action_column = new QVBoxLayout;
  action_column->addLayout(condition_row);
  action_column->addLayout(choice_row);
  action_box->setLayout(action_column);

  auto* column = new QVBoxLayout;
  column->addWidget(m_code_section);
  column->addWidget(m_memory_section);
  column->addWidget(action_box);
  column->addWidget(m_dialog_buttons);

  // Editing keeps the kind fixed: the other section is hidden and the key address is read-only.
  if (m_mode == OpenMode::EditMemCheck)
  {
    m_code_section->setVisible(false);
    m_memory_kind->setChecked(true);
    m_first_address->setEnabled(false);
    m_last_address->setFocus();
  }
  else
  {
    if (m_mode == OpenMode::EditBreakPoint)
      m_memory_section->setVisible(false);
    m_code_kind->setChecked(true);
    if (m_mode == OpenMode::EditBreakPoint)
      m_code_address->setEnabled(false);
    m_code_address->setFocus();
  }

  setLayout(column);
}

void BreakpointDialog::WireUp()
{
  connect(m_dialog_buttons, &QDialogButtonBox::accepted, this, &BreakpointDialog::accept);
  connect(m_dialog_buttons, &QDialogButtonBox::rejected, this, &BreakpointDialog::reject);
  connect(m_dialog_buttons, &QDialogButtonBox::helpRequested, this,
          &BreakpointDialog::ExplainConditions);

  for (QRadioButton* kind : {m_code_kind, m_memory_kind})
    connect(kind, &QRadioButton::toggled, this, &BreakpointDialog::SyncKindBoxes);
  for (QRadioButton* extent : {m_single_address, m_address_range})
    connect(extent, &QRadioButton::toggled, this, &BreakpointDialog::SyncRangeFields);
}

void BreakpointDialog::SyncKindBoxes()
{
  m_code_box->setEnabled(m_code_kind->isChecked());
  m_memory_box->setEnabled(m_memory_kind->isChecked());
}

void BreakpointDialog::SyncRangeFields()
{
  const bool ranged = m_address_range->isChecked();
  m_last_address->setHidden(!ranged);
  m_last_caption->setHidden(!ranged);
  m_first_caption->setText(ranged ? tr("From:") : tr("Address:"));
}

void BreakpointDialog::ComplainAbout(const QString& field)
{
  ModalMessageBox::critical(this, tr("Error"), tr("Invalid input for the field \"%1\"").arg(field));
}

void BreakpointDialog::accept()
{
  const bool ranged = m_address_range->isChecked();
  const bool any_access = m_on_access->isChecked();
  const bool on_read = m_on_read->isChecked() || any_access;
  const bool on_write = m_on_write->isChecked() || any_access;
  const bool both_actions = m_log_and_break->isChecked();
  const bool do_log = m_log_only->isChecked() || both_actions;
  const bool do_break = m_break_only->isChecked() || both_actions;

  const QString condition = m_condition->text().trimmed();
  if (!condition.isEmpty() && !Expression::TryParse(condition.toUtf8().constData()))
  {
    // i18n: If a condition is set for a breakpoint, the condition becoming true is a prerequisite
    // for triggering the breakpoint.
    ComplainAbout(tr("Condition"));
    return;
  }

  if (m_code_kind->isChecked())
  {
    u32 address;
    if (!ReadHex(m_code_address, &address))
    {
      ComplainAbout(tr("Address"));
      return;
    }
    m_owner->AddBP(address, do_break, do_log, condition);
    QDialog::accept();
    return;
  }

  u32 first;
  if (!ReadHex(m_first_address, &first))
  {
    ComplainAbout(ranged ? tr("From") : tr("Address"));
    return;
  }

  if (!ranged)
  {
    m_owner->AddAddressMBP(first, on_read, on_write, do_log, do_break, condition);
    QDialog::accept();
    return;
  }

  u32 last;
  if (!ReadHex(m_last_address, &last))
  {
    ComplainAbout(tr("To"));
    return;
  }
  m_owner->AddRangedMBP(first, last, on_read, on_write, do_log, do_break, condition);
  QDialog::accept();
}

void BreakpointDialog::ExplainConditions()
{
  const QString help = tr(
      "Conditions:\nSets an expression that is evaluated when a breakpoint is hit. "
      "If the expression is false or 0, the breakpoint is ignored until hit again. "
      "Statements should be separated by a comma. Only the last statement will be used to "
      "determine what to do.\n\nRegisters that can be referenced:\nGPRs : r0..r31\nFPRs : "
      "f0..f31\nSPRs : xer, lr, ctr, dsisr, dar, dec, sdr1, srr0, srr1, tbl, tbu, pvr, "
      "sprg0..sprg3, ear, ibat0u..ibat7u, ibat0l..ibat7l, dbat0u..dbat7u, dbat0l..dbat07, "
      "gqr0..gqr7, hid0, hid1, hid2, hid4, iabr, dabr, wpar, dmau, dmal, ecid_u, ecid_m, "
      "ecid_l, upmc1..upmc4, usia, sia, l2cr, ictc, mmcr0, mmcr1, pmc1..pmc4, thrm1..thrm3\n"
      "Other : pc, msr\n\nFunctions:\nSet a register: r1 = 8\nCasts: s8(0xff). Available: "
      "s8, u8, s16, u16, s32, u32\nCallstack: callstack(0x80123456), callstack(\"anim\")\n"
      "Compare Strings: streq(r3, \"abc\"). Both parameters can be addresses or string "
      "constants.\nRead Memory: read_u32(0x80000000). Available: u8, s8, u16, s16, u32, s32, "
      "f32, f64\nWrite Memory: write_u32(r3, 0x80000000). Available: u8, u16, u32, f32, f64\n"
      "*currently writing will always be triggered\n\nOperations:\nUnary: -u, !u, ~u\nMath: "
      "*  / + -, power: **, remainder: %, shift: <<, >>\nCompare: <, <=, >, >=, ==, !=, &&, "
      "||\nBitwise: &, |, ^\n\nExamples:\nr4 == 1\nf0 == 1.0 && f2 < 10.0\nr26 <= r0 && "
      "((r5 + 3) & -4) * ((r6 + 3) & -4)* 4 > r0\np = r3 + 0x8, p == 0x8003510 && read_u32(p) "
      "!= 0\nWrite and break: r4 = 8, 1\nWrite and continue: f3 = f1 + f2, 0\nThe condition "
      "must always be last\n\nStrings should only be used in callstack() or streq() and "
      "\"quoted\". Do not assign strings to a variable.\nAll variables will be printed in the "
      "Memory Interface log, if there's a hit or a NaN result. To check for issues, assign a "
      "variable to your equation, so it can be printed.\n\nNote: All values are internally "
      "converted to Doubles for calculations. It's possible for them to go out of range or to "
      "become NaN. A warning will be given if NaN is returned, and the var that became NaN "
      "will be logged.");
  // i18n: The title for a dialog that shows help for how to use conditions. If a condition is set
  // for a breakpoint, the condition becoming true is a prerequisite for triggering the breakpoint.
  ModalMessageBox::information(this, tr("Conditional help"), help);
}
