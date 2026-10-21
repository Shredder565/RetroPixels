// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/CheatCodeEditor.h"

#include <regex>

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QTextEdit>

#include "Common/StringUtil.h"
#include "Core/ARDecrypt.h"
#include "Core/ActionReplay.h"
#include "Core/GeckoCodeConfig.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

namespace
{
// A "$Name" line is only recognised as the very first line of the text.
bool IsTitleLine(qsizetype index, const QString& line)
{
  return index == 0 && line[0] == u'$';
}

QStringList SplitLines(const QTextEdit* edit)
{
  return edit->toPlainText().split(QLatin1Char{'\n'});
}
}  // namespace

CheatCodeEditor::CheatCodeEditor(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Cheat Code Editor"));

  m_name = new QLineEdit;
  m_creator = new QLineEdit;
  m_notes = new QTextEdit;
  m_lines = new QTextEdit;
  m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save);
  m_creator_caption = new QLabel(tr("Creator:"));
  m_notes_caption = new QLabel(tr("Notes:"));

  auto* const grid = new QGridLayout;
  grid->addWidget(new QLabel(tr("Name:")), 0, 0);
  grid->addWidget(m_name, 0, 1);
  grid->addWidget(m_creator_caption, 1, 0);
  grid->addWidget(m_creator, 1, 1);
  grid->addWidget(m_notes_caption, 2, 0);
  grid->addWidget(m_notes, 2, 1);
  grid->addWidget(new QLabel(tr("Code:")), 3, 0);
  grid->addWidget(m_lines, 3, 1);
  grid->addWidget(m_buttons, 4, 1);

  m_lines->setFont(QFont(QFontDatabase::systemFont(QFontDatabase::FixedFont).family()));
  m_lines->setAcceptRichText(false);
  m_notes->setAcceptRichText(false);

  setLayout(grid);

  connect(m_buttons, &QDialogButtonBox::accepted, this, &CheatCodeEditor::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void CheatCodeEditor::SetGeckoFieldsVisible(bool visible)
{
  m_creator_caption->setHidden(!visible);
  m_creator->setHidden(!visible);
  m_notes_caption->setHidden(!visible);
  m_notes->setHidden(!visible);
}

void CheatCodeEditor::SetARCode(ActionReplay::ARCode* code)
{
  m_name->setText(QString::fromStdString(code->name));
  m_lines->clear();
  for (ActionReplay::AREntry& entry : code->ops)
    m_lines->append(QString::fromStdString(ActionReplay::SerializeLine(entry)));

  SetGeckoFieldsVisible(false);

  m_ar_target = code;
  m_gecko_target = nullptr;
}

void CheatCodeEditor::SetGeckoCode(Gecko::GeckoCode* code)
{
  m_name->setText(QString::fromStdString(code->name));
  m_creator->setText(QString::fromStdString(code->creator));
  m_lines->clear();
  for (const Gecko::GeckoCode::Code& entry : code->codes)
    m_lines->append(QString::fromStdString(entry.original_line));

  // Every note line keeps a trailing newline.
  QString notes;
  for (const std::string& note : code->notes)
    notes += QStringLiteral("%1\n").arg(QString::fromStdString(note));
  m_notes->setText(notes);

  SetGeckoFieldsVisible(true);

  m_gecko_target = code;
  m_ar_target = nullptr;
}

bool CheatCodeEditor::StoreARCode()
{
  QString name = m_name->text();
  std::vector<ActionReplay::AREntry> plain;
  std::vector<std::string> encrypted;

  const QStringList lines = SplitLines(m_lines);
  for (qsizetype index = 0; index < lines.size(); ++index)
  {
    const QString& line = lines[index];
    if (line.isEmpty())
      continue;
    if (IsTitleLine(index, line))
    {
      // The title only fills an empty name.
      if (name.isEmpty())
        name = line.right(line.size() - 1);
      continue;
    }

    const auto parsed = ActionReplay::DeserializeLine(line.toStdString());
    if (const auto* const entry = std::get_if<ActionReplay::AREntry>(&parsed))
    {
      plain.push_back(*entry);
      continue;
    }
    if (const auto* const line_text = std::get_if<ActionReplay::EncryptedLine>(&parsed))
    {
      encrypted.emplace_back(*line_text);
      continue;
    }

    const auto answer = ModalMessageBox::warning(
        this, tr("Parsing Error"),
        tr("Unable to parse line %1 of the entered AR code as a valid encrypted or "
           "decrypted code. Make sure you typed it correctly.\n\nWould you like to "
           "ignore this line and continue parsing?")
            .arg(index + 1),
        QMessageBox::Ok | QMessageBox::Abort);
    if (answer == QMessageBox::Abort)
      return false;
  }

  if (!encrypted.empty())
  {
    if (!plain.empty())
    {
      // Yes keeps only the encrypted lines, No only the plain ones, and
      // Cancel goes back to editing.
      const auto answer = ModalMessageBox::warning(
          this, tr("Invalid Mixed Code"),
          tr("This Action Replay code contains both encrypted and unencrypted lines; you "
             "should check that you have entered it correctly.\n\nDo you want to discard all "
             "unencrypted lines?"),
          QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
      if (answer == QMessageBox::Cancel)
        return false;
      if (answer == QMessageBox::Yes)
        plain.clear();
      else if (answer == QMessageBox::No)
        encrypted.clear();
    }
    ActionReplay::DecryptARCode(encrypted, &plain);
  }

  if (plain.empty())
  {
    ModalMessageBox::critical(this, tr("Error"),
                              tr("The resulting decrypted AR code doesn't contain any lines."));
    return false;
  }

  m_ar_target->name = name.toStdString();
  m_ar_target->ops = std::move(plain);
  m_ar_target->user_defined = true;
  return true;
}

bool CheatCodeEditor::StoreGeckoCode()
{
  QString name = m_name->text();
  std::vector<Gecko::GeckoCode::Code> parsed_lines;

  const QStringList lines = SplitLines(m_lines);
  for (qsizetype index = 0; index < lines.size(); ++index)
  {
    const QString& line = lines[index];
    if (line.isEmpty())
      continue;
    if (IsTitleLine(index, line))
    {
      if (name.isEmpty())
        name = line.right(line.size() - 1);
      continue;
    }

    if (const std::optional<Gecko::GeckoCode::Code> entry =
            Gecko::DeserializeLine(line.toStdString()))
    {
      parsed_lines.push_back(*entry);
      continue;
    }

    const auto answer = ModalMessageBox::warning(
        this, tr("Parsing Error"),
        tr("Unable to parse line %1 of the entered Gecko code as a valid code. Make sure "
           "you typed it correctly.\n\nWould you like to ignore this line and continue "
           "parsing?")
            .arg(index + 1),
        QMessageBox::Ok | QMessageBox::Abort);
    if (answer == QMessageBox::Abort)
      return false;
  }

  if (parsed_lines.empty())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("This Gecko code doesn't contain any lines."));
    return false;
  }

  Gecko::GeckoCode& target = *m_gecko_target;
  target.name = name.trimmed().toStdString();
  target.creator = m_creator->text().trimmed().toStdString();
  target.codes = std::move(parsed_lines);
  target.notes = SplitString(m_notes->toPlainText().toStdString(), '\n');
  target.user_defined = true;

  // A trailing "[Creator]" in the name moves to the creator field, unless
  // a creator was typed in.
  static const std::regex name_with_creator{R"(^(.*)\[(.*)\]$)"};
  std::smatch parts;
  if (std::regex_match(target.name, parts, name_with_creator))
  {
    std::string creator = parts[2].str();
    target.name = StripWhitespace(parts[1].str());
    if (target.creator.empty())
      target.creator = StripWhitespace(creator);
  }

  return true;
}

void CheatCodeEditor::accept()
{
  const bool stored = m_gecko_target != nullptr ? StoreGeckoCode() : StoreARCode();
  if (stored)
    QDialog::accept();
}
