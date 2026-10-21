// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/AssemblyEditor.h"

#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>

#include <QFile>
#include <QFontDatabase>
#include <QHelpEvent>
#include <QPainter>
#include <QTextBlock>
#include <QToolTip>

#include "DolphinQt/Debugger/GekkoSyntaxHighlight.h"

namespace
{
QString LeafName(const QString& path)
{
  return QString::fromStdString(std::filesystem::path(path.toStdString()).filename().string());
}

// The first line of a saved file may be "#<base address>", optionally indented.
// Returns false if the line is not such a header (or has nothing after the '#', at end of file).
bool ParseBaseHeader(const std::string& line, QString* base)
{
  size_t pos = 0;
  while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
    ++pos;
  if (pos == line.size() || line[pos] != '#')
    return false;

  std::string value = line.substr(pos + 1);
  if (value.empty())
    return false;
  if (value.back() == '\n')
    value.pop_back();
  *base = value.empty() ? QStringLiteral("0") : QString::fromStdString(value);
  return true;
}
}  // namespace

// Draws the line numbers to the left of the text.
class AsmEditor::Gutter final : public QWidget
{
public:
  explicit Gutter(AsmEditor* owner) : QWidget(owner), m_owner(owner) {}
  QSize sizeHint() const override { return {m_owner->LineNumberAreaWidth(), 0}; }

protected:
  void paintEvent(QPaintEvent* event) override { m_owner->LineNumberAreaPaintEvent(event); }

private:
  AsmEditor* m_owner;
};

AsmEditor::AsmEditor(QString path, int editor_num, bool dark_scheme, QWidget* parent)
    : QPlainTextEdit(parent), m_file_path(std::move(path)), m_base(QStringLiteral("0")),
      m_number(editor_num), m_dark(dark_scheme)
{
  if (!m_file_path.isEmpty())
    m_file_name = LeafName(m_file_path);

  m_gutter = new Gutter(this);
  m_syntax = new GekkoSyntaxHighlight(document(), currentCharFormat(), dark_scheme);
  m_cursor_block = textCursor().block();

  QFont fixed(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  fixed.setPointSize(12);
  setFont(fixed);
  m_gutter->setFont(fixed);

  FitGutter();
  RefreshCursorLine();
  setMouseTracking(true);

  connect(this, &AsmEditor::blockCountChanged, this, [this](int) { FitGutter(); });
  connect(this, &AsmEditor::updateRequest, this, &AsmEditor::ScrollGutter);
  connect(this, &AsmEditor::cursorPositionChanged, this, &AsmEditor::RefreshCursorLine);
  connect(this, &AsmEditor::textChanged, this, [this] { MarkModified(true); });
}

void AsmEditor::MarkModified(bool modified)
{
  m_modified = modified;
  emit DirtyChanged();
}

int AsmEditor::SpaceWidth() const
{
  return fontMetrics().horizontalAdvance(QLatin1Char(' '));
}

int AsmEditor::LineNumberAreaWidth()
{
  // Room for at least two digits, plus a few pixels of padding.
  int digits = 1;
  for (int lines = std::max(1, blockCount()); lines >= 10; lines /= 10)
    ++digits;
  return 3 + SpaceWidth() * std::max(2, digits);
}

void AsmEditor::SetBaseAddress(const QString& ba)
{
  if (ba == m_base)
    return;
  m_base = ba;
  MarkModified(true);
}

bool AsmEditor::LoadFromPath()
{
  QFile source(m_file_path);
  if (!source.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;

  QString header_base;
  if (ParseBaseHeader(source.readLine().toStdString(), &header_base))
    m_base = header_base;
  else
    source.seek(0);

  // Loading is not an edit: keep the text-changed signal (and the modified flag) quiet.
  const bool was_blocked = blockSignals(true);
  setPlainText(QString::fromUtf8(source.readAll()));
  blockSignals(was_blocked);
  return true;
}

bool AsmEditor::PathsMatch(const QString& path) const
{
  if (path.isEmpty() || m_file_path.isEmpty())
    return false;
  return std::filesystem::path(path.toStdString()) ==
         std::filesystem::path(m_file_path.toStdString());
}

void AsmEditor::Zoom(int amount)
{
  if (amount > 0)
    zoomIn(amount);
  else
    zoomOut(-amount);
  m_gutter->setFont(font());
}

bool AsmEditor::SaveFile(const QString& save_path)
{
  QFile target(save_path);
  if (!target.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    return false;

  if (save_path != m_file_path)
  {
    m_file_path = save_path;
    m_file_name = LeafName(m_file_path);
    emit PathChanged();
  }

  const QByteArray header = QStringLiteral("#%1\n").arg(m_base).toUtf8();
  if (target.write(header) == -1 || target.write(toPlainText().toUtf8()) == -1)
    return false;

  MarkModified(false);
  return true;
}

void AsmEditor::FitGutter()
{
  setViewportMargins(LineNumberAreaWidth(), 0, 0, 0);
}

void AsmEditor::ScrollGutter(const QRect& rect, int dy)
{
  if (dy == 0)
    m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
  else
    m_gutter->scroll(0, dy);

  if (rect.contains(viewport()->rect()))
    FitGutter();
}

void AsmEditor::resizeEvent(QResizeEvent* e)
{
  QPlainTextEdit::resizeEvent(e);
  const QRect inner = contentsRect();
  m_gutter->setGeometry(QRect(inner.left(), inner.top(), LineNumberAreaWidth(), inner.height()));
}

void AsmEditor::paintEvent(QPaintEvent* event)
{
  QPlainTextEdit::paintEvent(event);

  // An error at the end of a line has no characters to underline; draw a short red bar instead.
  QPainter painter(viewport());
  QPen bar(Qt::red);
  bar.setStyle(Qt::SolidLine);
  bar.setWidth(1);
  painter.setPen(bar);

  const int bar_length = SpaceWidth();
  QTextCursor probe(document());
  for (QTextBlock line = firstVisibleBlock(); line.isValid() && line.isVisible();
       line = line.next())
  {
    const auto* info = static_cast<const BlockInfo*>(line.userData());
    if (!info || !info->error_at_eol)
      continue;

    probe.setPosition(line.position() + line.length() - 1);
    probe.clearSelection();
    const QRect at = cursorRect(probe);
    const int y = at.y() + at.height();
    painter.drawLine(at.x(), y, at.x() + bar_length, y);
  }
}

bool AsmEditor::ShowErrorTip(const QPoint& local, const QPoint& global)
{
  QTextCursor probe = cursorForPosition(local);
  const QTextBlock line = probe.block();
  const auto* info = static_cast<const BlockInfo*>(line.userData());
  if (!info || !info->error)
    return false;

  const int start = line.position() + static_cast<int>(info->error->col);
  const int end = line.position() + static_cast<int>(info->error->col + info->error->len);
  const int gutter = LineNumberAreaWidth();

  QRect hot;
  if (info->error_at_eol)
  {
    probe.setPosition(end);
    const QRect left = cursorRect(probe);
    // The width here is measured from the viewport's left edge, as it always was.
    hot = QRect(left.x() + gutter, left.y(), left.x() + SpaceWidth(), left.height());
  }
  else
  {
    probe.setPosition(start);
    const QRect left = cursorRect(probe);
    probe.setPosition(end);
    const QRect right = cursorRect(probe);
    hot = QRect(left.x() + gutter, left.y(), right.x() - left.x(), left.height());
  }

  if (!hot.contains(local))
    return false;
  QToolTip::showText(global, QString::fromStdString(info->error->message));
  return true;
}

bool AsmEditor::event(QEvent* e)
{
  if (e->type() != QEvent::ToolTip)
    return QPlainTextEdit::event(e);

  const auto* help = static_cast<QHelpEvent*>(e);
  if (!ShowErrorTip(help->pos(), help->globalPos()))
    QToolTip::hideText();
  return true;
}

void AsmEditor::keyPressEvent(QKeyEvent* event)
{
  // Shift+Enter would insert a soft line break inside the block; treat it as a plain Enter so every
  // line stays its own block.
  const bool is_enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
  if (is_enter && (event->modifiers() & Qt::ShiftModifier))
    event->setModifiers(event->modifiers() & ~Qt::ShiftModifier);
  QPlainTextEdit::keyPressEvent(event);
}

void AsmEditor::wheelEvent(QWheelEvent* event)
{
  QPlainTextEdit::wheelEvent(event);
  if (!(event->modifiers() & Qt::ControlModifier))
    return;

  const int steps = static_cast<int>(std::round(event->angleDelta().y() / 120.0));
  if (steps != 0)
    emit ZoomRequested(steps);
}

void AsmEditor::RefreshCursorLine()
{
  const bool was_blocked = blockSignals(true);
  const QTextCursor cursor = textCursor();

  // Redraw the line the cursor left without its bracket highlight.
  if (cursor.blockNumber() != m_cursor_block.blockNumber())
  {
    m_syntax->SetMode(2);
    m_syntax->rehighlightBlock(m_cursor_block);
    m_cursor_block = textCursor().block();
  }

  m_syntax->SetCursorLoc(cursor.positionInBlock());
  m_syntax->SetMode(1);
  m_syntax->rehighlightBlock(textCursor().block());
  m_syntax->SetMode(0);

  blockSignals(was_blocked);
}

void AsmEditor::LineNumberAreaPaintEvent(QPaintEvent* event)
{
  QPainter painter(m_gutter);
  const QRect dirty = event->rect();
  painter.fillRect(dirty, m_dark ? QColor::fromRgb(76, 76, 76) : QColor::fromRgb(180, 180, 180));

  const int gutter_width = m_gutter->width();
  const int text_height = fontMetrics().height();

  QTextBlock line = firstVisibleBlock();
  int number = line.blockNumber() + 1;
  int y_top = qRound(blockBoundingGeometry(line).translated(contentOffset()).top());
  int y_bottom = y_top + qRound(blockBoundingRect(line).height());

  for (; line.isValid() && y_top <= dirty.bottom(); ++number)
  {
    if (line.isVisible() && y_bottom >= dirty.top())
      painter.drawText(0, y_top, gutter_width, text_height, Qt::AlignRight,
                       QString::number(number));

    line = line.next();
    y_top = y_bottom;
    y_bottom = y_top + qRound(blockBoundingRect(line).height());
  }
}
