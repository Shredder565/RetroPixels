// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "Common/Assembler/AssemblerShared.h"

// Roles a span of assembly text can be drawn in.
enum class HighlightFormat
{
  Directive,
  Mnemonic,
  Symbol,
  Immediate,
  GPR,
  FPR,
  SPR,
  CRField,
  CRFlag,
  Str,
  HaLa,
  Paren,
  Default,
  Comment,
  Error,
};

// Per-line results of the last full parse, kept on the text block so that cheap re-highlights
// (bracket matching) can reuse them.
struct BlockInfo : public QTextBlockUserData
{
  std::vector<std::tuple<int, int, HighlightFormat>> block_format;
  std::vector<std::pair<int, int>> parens;
  std::optional<Common::GekkoAssembler::AssemblerError> error;
  bool error_at_eol = false;
};

class GekkoSyntaxHighlight : public QSyntaxHighlighter
{
  Q_OBJECT;

public:
  explicit GekkoSyntaxHighlight(QTextDocument* document, QTextCharFormat base_format,
                                bool dark_scheme);

  void HighlightSubstr(int start, int len, HighlightFormat format);
  // 0: parse the line again; 1: only redraw, adding the bracket pair at the cursor.
  void SetMode(int mode) { m_pass = mode; }
  void SetCursorLoc(int loc) { m_cursor_column = loc; }

protected:
  void highlightBlock(const QString& line) override;

private:
  int m_pass = 0;
  int m_cursor_column = 0;
  QTextCharFormat m_plain;
  bool m_dark = false;
};
