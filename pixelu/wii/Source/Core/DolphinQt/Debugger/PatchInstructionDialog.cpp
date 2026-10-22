// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/PatchInstructionDialog.h"

#include <algorithm>
#include <array>
#include <optional>

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/Assembler/GekkoAssembler.h"
#include "Common/GekkoDisassembler.h"
#include "Common/Swap.h"

namespace
{
QString HexWord(u32 word)
{
  return QStringLiteral("%1").arg(word, 8, 16, QLatin1Char('0'));
}

// Assembles a single line and returns the first word of the first block, if any block came out.
std::optional<u32> AssembleOne(const QString& line, u32 address)
{
  namespace GA = Common::GekkoAssembler;
  const auto outcome = GA::Assemble(line.toStdString(), address);
  if (GA::IsFailure(outcome))
    return std::nullopt;

  const auto& blocks = GA::GetT(outcome);
  if (blocks.empty())
    return std::nullopt;

  // Short blocks are padded with zero bytes instead of reading past their end.
  std::array<u8, 4> raw{};
  const auto& bytes = blocks.front().instructions;
  std::copy_n(bytes.begin(), std::min<size_t>(bytes.size(), raw.size()), raw.begin());
  return Common::swap32(raw.data());
}
}  // namespace

PatchInstructionDialog::PatchInstructionDialog(QWidget* parent, u32 address, u32 value)
    : QDialog(parent), m_word(value), m_where(address)
{
  setWindowModality(Qt::WindowModal);
  setWindowTitle(tr("Instruction"));

  m_entry = new QLineEdit;
  m_preview = new QLabel;
  m_dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

  auto* column = new QVBoxLayout;
  column->addWidget(new QLabel(tr("New instruction:")));
  column->addWidget(m_entry);
  column->addWidget(m_preview);
  column->addWidget(m_dialog_buttons);
  setLayout(column);

  connect(m_dialog_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_dialog_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_entry, &QLineEdit::textChanged, this, &PatchInstructionDialog::Reinterpret);

  // Filling the field runs Reinterpret through the connection above.
  m_entry->setText(HexWord(value));
}

void PatchInstructionDialog::Reinterpret()
{
  const QString typed = m_entry->text();

  bool accepted = false;
  QString shown = QStringLiteral("(ill)\t---");

  bool parsed_hex = false;
  const u32 as_hex = typed.toUInt(&parsed_hex, 16);
  if (parsed_hex)
  {
    m_word = as_hex;
    accepted = true;
    shown = QString::fromStdString(Common::GekkoDisassembler::Disassemble(as_hex, m_where));
  }
  else if (const std::optional<u32> assembled = AssembleOne(typed, m_where))
  {
    m_word = *assembled;
    accepted = true;
    shown = HexWord(m_word);
  }

  m_dialog_buttons->button(QDialogButtonBox::Ok)->setEnabled(accepted);
  m_preview->setText(tr("Instruction: %1").arg(shown));
}

u32 PatchInstructionDialog::GetCode() const
{
  return m_word;
}
