// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/RegisterColumn.h"

#include <bit>
#include <utility>

#include "DolphinQt/QtUtils/ModalMessageBox.h"

RegisterColumn::RegisterColumn(RegisterType type, std::function<u64()> get,
                               std::function<void(u64)> set)
    : m_kind(type), m_read(std::move(get)), m_write(std::move(set))
{
  RefreshValue();
  Redraw();

  // Read-only registers lose the (default) editable flag; writable ones are editable and enabled
  // only.
  if (m_write)
    setFlags(Qt::ItemIsEditable | Qt::ItemIsEnabled);
  else
    setFlags(flags() ^ Qt::ItemIsEditable);

  setData(DATA_TYPE, static_cast<quint32>(type));
}

RegisterDisplay RegisterColumn::GetDisplay() const
{
  return m_format;
}

void RegisterColumn::SetDisplay(RegisterDisplay display)
{
  m_format = display;
  Redraw();
}

u64 RegisterColumn::GetValue() const
{
  return m_shown;
}

void RegisterColumn::RefreshValue()
{
  // A value that changed since the last refresh is drawn in red.
  QBrush colour = QPalette().brush(QPalette::Text);
  const u64 current = m_read();
  if (current != m_shown)
  {
    m_shown = current;
    colour.setColor(Qt::red);
  }
  setForeground(colour);

  Redraw();
}

void RegisterColumn::SetValue()
{
  const QString typed = text();
  bool ok = false;
  u64 raw = 0;

  switch (m_format)
  {
  case RegisterDisplay::Hex:
    raw = typed.toULongLong(&ok, 16);
    break;
  case RegisterDisplay::SInt32:
    // Negative numbers sign-extend into the upper half.
    raw = static_cast<u64>(static_cast<s64>(typed.toInt(&ok)));
    break;
  case RegisterDisplay::UInt32:
    raw = typed.toUInt(&ok);
    break;
  case RegisterDisplay::Float:
    raw = std::bit_cast<u32>(typed.toFloat(&ok));
    break;
  case RegisterDisplay::Double:
    raw = std::bit_cast<u64>(typed.toDouble(&ok));
    break;
  }

  if (ok)
    m_write(raw);
  else
    ModalMessageBox::critical(nullptr, QObject::tr("Error"), QObject::tr("Invalid input provided"));

  RefreshValue();
}

int RegisterColumn::HexDigits() const
{
  switch (m_kind)
  {
  case RegisterType::ibat:
  case RegisterType::dbat:
  case RegisterType::fpr:
  case RegisterType::tb:
    return 16;
  default:
    return 8;
  }
}

void RegisterColumn::Redraw()
{
  QString shown;
  switch (m_format)
  {
  case RegisterDisplay::Hex:
    shown = QStringLiteral("%1").arg(m_shown, HexDigits(), 16, QLatin1Char('0'));
    break;
  case RegisterDisplay::SInt32:
    shown = QString::number(static_cast<qint32>(m_shown));
    break;
  case RegisterDisplay::UInt32:
    shown = QString::number(static_cast<quint32>(m_shown));
    break;
  case RegisterDisplay::Float:
    shown = QString::number(std::bit_cast<float>(static_cast<u32>(m_shown)));
    break;
  case RegisterDisplay::Double:
    shown = QString::number(std::bit_cast<double>(m_shown));
    break;
  }
  setText(shown);
}
