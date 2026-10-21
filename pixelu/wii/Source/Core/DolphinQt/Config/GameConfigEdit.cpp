// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/GameConfigEdit.h"

#include <utility>

#include <QAbstractItemView>
#include <QCompleter>
#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextCursor>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QWhatsThis>

#include "DolphinQt/Config/GameConfigHighlighter.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"

GameConfigEdit::GameConfigEdit(QWidget* parent, QString path, bool read_only)
    : QWidget{parent}, m_path(std::move(path)), m_read_only(read_only)
{
  m_text = new QTextEdit;
  m_text->setReadOnly(m_read_only);
  m_text->setAcceptRichText(false);

  m_reload_button = new QPushButton(tr("Refresh"));
  m_external_button = new QPushButton(tr("Open in External Editor"));
  // A read-only (default) INI can only be viewed here.
  if (m_read_only)
  {
    m_reload_button->hide();
    m_external_button->hide();
  }

  auto* const buttons = new QHBoxLayout;
  buttons->addWidget(m_reload_button);
  buttons->addWidget(m_external_button);
  auto* const column = new QVBoxLayout;
  column->addLayout(buttons);
  column->addWidget(m_text);
  setLayout(column);

  ReadFromDisk();

  new GameConfigHighlighter(m_text->document());

  Describe(QStringLiteral("Core"),
           tr("Section that contains most CPU and Hardware related settings."));
  Describe(QStringLiteral("CPUThread"),
           tr("Controls whether or not Dual Core should be enabled. Can improve performance "
              "but can also cause issues. Defaults to <b>True</b>"));
  Describe(QStringLiteral("FastDiscSpeed"),
           tr("Emulate the disc speed of real hardware. Disabling can cause instability. "
              "Defaults to <b>True</b>"));
  Describe(QStringLiteral("MMU"),
           tr("Controls whether or not the Memory Management Unit should be emulated "
              "fully. Few games require it."));
  Describe(QStringLiteral("DSPHLE"),
           tr("Controls whether to use high or low-level DSP emulation. "
              "Defaults to <b>True</b>"));
  Describe(QStringLiteral("JITFollowBranch"),
           tr("Tries to translate branches ahead of time, improving performance in most "
              "cases. Defaults to <b>True</b>"));
  Describe(QStringLiteral("Gecko"), tr("Section that contains all Gecko cheat codes."));
  Describe(QStringLiteral("ActionReplay"),
           tr("Section that contains all Action Replay cheat codes."));
  Describe(QStringLiteral("Video_Settings"),
           tr("Section that contains all graphics related settings."));

  // Completion offers the described keywords in the order above.
  m_completer = new QCompleter(m_text);
  auto* const keyword_model = new QStringListModel(m_completer);
  keyword_model->setStringList(m_keywords);
  m_completer->setModel(keyword_model);
  m_completer->setModelSorting(QCompleter::UnsortedModel);
  m_completer->setCompletionMode(QCompleter::PopupCompletion);
  m_completer->setWidget(m_text);

  connect(m_text, &QTextEdit::textChanged, this, &GameConfigEdit::WriteToDisk);
  connect(m_text, &QTextEdit::selectionChanged, this, &GameConfigEdit::ShowHelpForSelection);
  connect(m_completer, qOverload<const QString&>(&QCompleter::activated), this,
          &GameConfigEdit::InsertCompletion);
  connect(m_reload_button, &QPushButton::clicked, this, &GameConfigEdit::ReadFromDisk);
  connect(m_external_button, &QPushButton::clicked, this, &GameConfigEdit::EditExternally);
}

void GameConfigEdit::Describe(const QString& keyword, const QString& help)
{
  m_help[keyword] = help;
  m_keywords << keyword;
}

void GameConfigEdit::ReadFromDisk()
{
  QFile file(m_path);
  if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    m_text->setPlainText(QString::fromStdString(file.readAll().toStdString()));
}

void GameConfigEdit::WriteToDisk()
{
  // Only an editable file that is on screen is written.
  if (m_read_only || !isVisible())
    return;

  QFile file(m_path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
  {
    ModalMessageBox::warning(this, tr("Warning"), tr("Failed to open config file!"));
    return;
  }

  if (file.write(m_text->toPlainText().toUtf8()) == -1)
    ModalMessageBox::warning(this, tr("Warning"), tr("Failed to write config file!"));
}

void GameConfigEdit::ShowHelpForSelection()
{
  const QString selected = m_text->textCursor().selectedText();
  const auto found = m_help.constFind(selected);
  if (found != m_help.constEnd())
    QWhatsThis::showText(QCursor::pos(), found.value(), this);
}

QString GameConfigEdit::WordAtCursor() const
{
  QTextCursor cursor = m_text->textCursor();
  cursor.select(QTextCursor::WordUnderCursor);
  return cursor.selectedText();
}

void GameConfigEdit::InsertCompletion(const QString& completion)
{
  // Append the part of the completion that hasn't been typed yet at the end
  // of the current word.
  const qsizetype missing = completion.length() - m_completer->completionPrefix().length();
  QTextCursor cursor = m_text->textCursor();
  cursor.movePosition(QTextCursor::Left);
  cursor.movePosition(QTextCursor::EndOfWord);
  cursor.insertText(completion.right(missing));
  m_text->setTextCursor(cursor);
}

void GameConfigEdit::EditExternally()
{
  QFile file(m_path);
  if (!file.exists())
  {
    if (m_read_only)
      return;
    // Create an empty file for the editor to open.
    if (!file.open(QIODevice::WriteOnly))
    {
      ModalMessageBox::warning(this, tr("Error"),
                               tr("Failed to create the configuration file:\n%1").arg(m_path));
      return;
    }
    file.close();
  }

  if (QDesktopServices::openUrl(QUrl::fromLocalFile(m_path)))
    return;

  ModalMessageBox::warning(this, tr("Error"),
                           tr("Failed to open file in external editor.\nMake sure there's an "
                              "application assigned to open INI files."));
}

void GameConfigEdit::keyPressEvent(QKeyEvent* e)
{
  QAbstractItemView* const popup = m_completer->popup();

  // While the popup is open these keys belong to the completer.
  if (popup->isVisible())
  {
    const int key = e->key();
    if (key == Qt::Key_Enter || key == Qt::Key_Return || key == Qt::Key_Escape ||
        key == Qt::Key_Tab || key == Qt::Key_Backtab)
    {
      e->ignore();
      return;
    }
  }

  QWidget::keyPressEvent(e);

  static const QString word_breaks = QStringLiteral("~!@#$%^&*()_+{}|:\"<>?,./;'\\-=");

  // Offer completions once at least two characters of a word are typed.
  const QString prefix = WordAtCursor();
  const QString typed = e->text();
  if (typed.isEmpty() || prefix.length() < 2 || word_breaks.contains(typed.right(1)))
  {
    popup->hide();
    return;
  }

  if (prefix != m_completer->completionPrefix())
  {
    m_completer->setCompletionPrefix(prefix);
    popup->setCurrentIndex(m_completer->completionModel()->index(0, 0));
  }

  QRect area = m_text->cursorRect();
  area.setWidth(popup->sizeHintForColumn(0) + popup->verticalScrollBar()->sizeHint().width());
  m_completer->complete(area);
}

void GameConfigEdit::focusInEvent(QFocusEvent* e)
{
  m_completer->setWidget(m_text);
  QWidget::focusInEvent(e);
}
