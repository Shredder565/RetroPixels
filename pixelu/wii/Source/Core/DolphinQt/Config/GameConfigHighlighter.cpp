// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/GameConfigHighlighter.h"

#include <QBrush>
#include <QColor>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextLayout>

#include "DolphinQt/Settings.h"

struct HighlightingRule
{
  QRegularExpression pattern;
  QTextCharFormat format;
};

namespace
{
// Picks the colour for the current theme.
QBrush Themed(bool dark, QColor on_dark, Qt::GlobalColor on_light)
{
  return dark ? QBrush{on_dark} : QBrush{on_light};
}
}  // namespace

GameConfigHighlighter::GameConfigHighlighter(QTextDocument* parent) : QObject(parent)
{
  const bool dark = Settings::Instance().IsThemeDark();

  QTextCharFormat assignment;
  assignment.setForeground(Themed(dark, QColor(255, 96, 96), Qt::red));

  QTextCharFormat header;
  header.setFontWeight(QFont::Bold);

  QTextCharFormat comment;
  comment.setForeground(Themed(dark, QColor(0, 220, 0), Qt::darkGreen));
  comment.setFontItalic(true);

  QTextCharFormat boolean;
  boolean.setFontWeight(QFont::Bold);
  boolean.setForeground(Themed(dark, QColor(132, 132, 255), Qt::blue));

  QTextCharFormat number;
  number.setForeground(Themed(dark, QColor(66, 138, 255), Qt::darkBlue));

  // The ranges are handed to the layout in this rule order.
  const auto add = [this](const QString& regex, const QTextCharFormat& format) {
    m_rules.push_back(HighlightingRule{QRegularExpression(regex), format});
  };
  add(QStringLiteral("="), assignment);
  add(QStringLiteral("^\\[.*?\\]"), header);
  add(QStringLiteral("\\bTrue\\b"), boolean);
  add(QStringLiteral("\\bFalse\\b"), boolean);
  add(QStringLiteral("\\b[0-9a-fA-F]+\\b"), number);
  add(QStringLiteral("^#.*"), comment);
  add(QStringLiteral("^\\$.*"), comment);
  add(QStringLiteral("^\\*.*"), comment);

  // Re-format every block touched by an edit, from the block holding the
  // first changed character up to the end of the inserted text.
  connect(parent, &QTextDocument::contentsChange, this,
          [this, parent](const int position, int, const int inserted) {
            const int last = position + inserted;
            for (QTextBlock block = parent->findBlock(position);
                 block.isValid() && block.position() <= last; block = block.next())
            {
              FormatBlock(block);
            }
          });

  for (QTextBlock block = parent->begin(); block.isValid(); block = block.next())
    FormatBlock(block);
}

GameConfigHighlighter::~GameConfigHighlighter() = default;

void GameConfigHighlighter::FormatBlock(const QTextBlock& block)
{
  const QString text = block.text();
  QList<QTextLayout::FormatRange> ranges;
  for (const HighlightingRule& rule : m_rules)
  {
    for (auto matches = rule.pattern.globalMatch(text); matches.hasNext();)
    {
      const QRegularExpressionMatch match = matches.next();
      QTextLayout::FormatRange range;
      range.start = static_cast<int>(match.capturedStart());
      range.length = static_cast<int>(match.capturedLength());
      range.format = rule.format;
      ranges.push_back(range);
    }
  }

  QTextLayout* const layout = block.layout();
  layout->clearFormats();
  layout->setFormats(ranges);
}
