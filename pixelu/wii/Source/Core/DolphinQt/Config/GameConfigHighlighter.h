// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <vector>

#include <QObject>

struct HighlightingRule;

class QTextBlock;
class QTextDocument;

// Colours INI text in a document. Blocks are formatted directly as they
// change, rather than through QSyntaxHighlighter, which stalls on very large
// files.
class GameConfigHighlighter : public QObject
{
  Q_OBJECT

public:
  explicit GameConfigHighlighter(QTextDocument* parent);
  ~GameConfigHighlighter() override;

private:
  void FormatBlock(const QTextBlock& block);

  std::vector<HighlightingRule> m_rules;
};
