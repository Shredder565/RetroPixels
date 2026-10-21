// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/GekkoSyntaxHighlight.h"

#include <algorithm>
#include <utility>

#include <QColor>

#include "Common/Assembler/GekkoParser.h"

namespace
{
namespace GA = Common::GekkoAssembler;
namespace GAD = Common::GekkoAssembler::detail;

using Span = std::tuple<int, int, HighlightFormat>;

// Listens to the parser and records which columns of the line play which role.
class SpanCollector final : public GAD::ParsePlugin
{
public:
  std::vector<Span> TakeSpans() { return std::move(m_spans); }
  std::vector<std::pair<int, int>> TakeBracketPairs() { return std::move(m_pairs); }

  void OnDirectivePre(GA::GekkoDirective) override { MarkNext(HighlightFormat::Directive); }
  void OnInstructionPre(const GAD::ParseInfo&, bool) override
  {
    MarkNext(HighlightFormat::Mnemonic);
  }

  void OnTerminal(GAD::Terminal kind, const GAD::AssemblerToken&) override
  {
    if (const std::optional<HighlightFormat> role = RoleOf(kind))
      MarkNext(*role);
  }

  void OnHiaddr(std::string_view) override
  {
    MarkNext(HighlightFormat::Symbol);
    // The "@ha"/"@l" suffix sits two tokens further on.
    const auto& [where, suffix] = m_owner->lexer.LookaheadTagRef(2);
    m_spans.emplace_back(static_cast<int>(where.col), static_cast<int>(suffix.token_val.length()),
                         HighlightFormat::HaLa);
  }
  void OnLoaddr(std::string_view name) override { OnHiaddr(name); }

  void OnOpenParen(GAD::ParenType) override { m_open.push_back(Column()); }
  void OnCloseParen(GAD::ParenType) override
  {
    if (m_open.empty())
      return;
    m_pairs.emplace_back(m_open.back(), Column());
    m_open.pop_back();
  }

  void OnError() override
  {
    const GA::AssemblerError& error = *m_owner->error;
    m_spans.emplace_back(static_cast<int>(error.col), static_cast<int>(error.len),
                         HighlightFormat::Error);
  }

  // Declarations record (length, column) rather than (column, length); kept as it always was.
  void OnLabelDecl(std::string_view) override { MarkNextSwapped(); }
  void OnNumericLabelDecl(std::string_view, u32) override { MarkNextSwapped(); }
  void OnVarDecl(std::string_view) override { MarkNextSwapped(); }

private:
  static std::optional<HighlightFormat> RoleOf(GAD::Terminal kind)
  {
    using T = GAD::Terminal;
    switch (kind)
    {
    case T::Id:
    case T::NumLabFwd:
    case T::NumLabBwd:
      return HighlightFormat::Symbol;
    case T::Hex:
    case T::Dec:
    case T::Oct:
    case T::Bin:
    case T::Flt:
      return HighlightFormat::Immediate;
    case T::GPR:
      return HighlightFormat::GPR;
    case T::FPR:
      return HighlightFormat::FPR;
    case T::SPR:
      return HighlightFormat::SPR;
    case T::CRField:
      return HighlightFormat::CRField;
    case T::Lt:
    case T::Gt:
    case T::Eq:
    case T::So:
      return HighlightFormat::CRFlag;
    case T::Str:
      return HighlightFormat::Str;
    default:
      return std::nullopt;
    }
  }

  int Column() const { return static_cast<int>(m_owner->lexer.ColNumber()); }
  int NextLength() const
  {
    return static_cast<int>(m_owner->lexer.LookaheadRef().token_val.length());
  }

  void MarkNext(HighlightFormat role) { m_spans.emplace_back(Column(), NextLength(), role); }
  void MarkNextSwapped()
  {
    m_spans.emplace_back(NextLength(), Column(), HighlightFormat::Symbol);
  }

  std::vector<int> m_open;
  std::vector<std::pair<int, int>> m_pairs;
  std::vector<Span> m_spans;
};

// Gruvbox colours; index 0 for light themes, 1 for dark ones.
struct ThemeColour
{
  QRgb light;
  QRgb dark;
};

constexpr ThemeColour DIRECTIVE_TINT{0x9d0006, 0xfb4934};
constexpr ThemeColour MNEMONIC_TINT{0x79740e, 0xb8bb26};
constexpr ThemeColour NUMBER_TINT{0xb57614, 0xfabd2f};
constexpr ThemeColour REGISTER_TINT{0x076678, 0x83a598};
constexpr ThemeColour HALA_TINT{0xaf3a03, 0xfe8019};
constexpr ThemeColour BRACKET_FILL{0xd5c4a1, 0x504945};
constexpr ThemeColour STRING_TINT{0x98971a, 0x98971a};
constexpr ThemeColour COMMENT_TINT{0x689d6a, 0x689d6a};
}  // namespace

GekkoSyntaxHighlight::GekkoSyntaxHighlight(QTextDocument* document, QTextCharFormat base_format,
                                           bool dark_scheme)
    : QSyntaxHighlighter(document), m_plain(std::move(base_format)), m_dark(dark_scheme)
{
}

void GekkoSyntaxHighlight::highlightBlock(const QString& text)
{
  auto* info = static_cast<BlockInfo*>(currentBlockUserData());
  if (!info)
  {
    info = new BlockInfo;
    setCurrentBlockUserData(info);
  }

  const qsizetype hash = text.indexOf(QLatin1Char('#'));
  if (hash >= 0)
    HighlightSubstr(static_cast<int>(hash), static_cast<int>(text.length() - hash),
                    HighlightFormat::Comment);

  switch (m_pass)
  {
  case 0:
  {
    SpanCollector collector;
    GAD::ParseWithPlugin(&collector, text.toStdString());
    info->block_format = collector.TakeSpans();
    info->parens = collector.TakeBracketPairs();
    info->error = std::move(collector.Error());
    info->error_at_eol = info->error.has_value() && info->error->len == 0;
    break;
  }
  case 1:
  {
    const auto pair = std::ranges::find_if(info->parens, [this](const std::pair<int, int>& p) {
      return p.first == m_cursor_column || p.second == m_cursor_column;
    });
    if (pair != info->parens.end())
    {
      HighlightSubstr(pair->first, 1, HighlightFormat::Paren);
      HighlightSubstr(pair->second, 1, HighlightFormat::Paren);
    }
    break;
  }
  default:
    break;
  }

  for (const auto& [column, length, role] : info->block_format)
    HighlightSubstr(column, length, role);
}

void GekkoSyntaxHighlight::HighlightSubstr(int start, int len, HighlightFormat format)
{
  const auto pick = [this](const ThemeColour& c) { return QColor(m_dark ? c.dark : c.light); };

  QTextCharFormat look = m_plain;
  switch (format)
  {
  case HighlightFormat::Directive:
    look.setForeground(pick(DIRECTIVE_TINT));
    break;
  case HighlightFormat::Mnemonic:
    look.setForeground(pick(MNEMONIC_TINT));
    break;
  case HighlightFormat::Immediate:
    look.setForeground(pick(NUMBER_TINT));
    break;
  case HighlightFormat::GPR:
  case HighlightFormat::FPR:
  case HighlightFormat::SPR:
  case HighlightFormat::CRField:
  case HighlightFormat::CRFlag:
    look.setForeground(pick(REGISTER_TINT));
    break;
  case HighlightFormat::Str:
    look.setForeground(pick(STRING_TINT));
    break;
  case HighlightFormat::HaLa:
    look.setForeground(pick(HALA_TINT));
    break;
  case HighlightFormat::Paren:
    look.setBackground(pick(BRACKET_FILL));
    break;
  case HighlightFormat::Comment:
    look.setForeground(pick(COMMENT_TINT));
    break;
  case HighlightFormat::Error:
    look.setUnderlineColor(Qt::red);
    look.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    break;
  case HighlightFormat::Default:
    look.clearForeground();
    look.clearBackground();
    break;
  case HighlightFormat::Symbol:
    // Symbols keep the plain look.
    break;
  }

  setFormat(start, len, look);
}
