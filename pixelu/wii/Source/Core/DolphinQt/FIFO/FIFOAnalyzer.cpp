// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/FIFO/FIFOAnalyzer.h"

#include <algorithm>
#include <bit>
#include <utility>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "Common/Assert.h"
#include "Common/Swap.h"
#include "Core/FifoPlayer/FifoPlayer.h"

#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/XFStructs.h"

namespace
{
// Data stored on the object rows of the tree (frame and part rows carry none):
// the frame number (0-based),
constexpr int FRAME_ROLE = Qt::UserRole;
// the first part of the object (0-based),
constexpr int PART_START_ROLE = Qt::UserRole + 1;
// and its last part, the draw or EFB copy that ends it.
constexpr int PART_END_ROLE = Qt::UserRole + 2;

const QString OBJECT_SPLITTER_KEY = QStringLiteral("fifoanalyzer/objectsplitter");
const QString SEARCH_SPLITTER_KEY = QStringLiteral("fifoanalyzer/searchsplitter");

QString Hex(u32 value, int digits)
{
  return QStringLiteral("%1").arg(value, digits, 16, QLatin1Char('0'));
}

QString HexBytes(const u8* bytes, u32 count)
{
  QString out;
  for (u32 i = 0; i < count; ++i)
    out += Hex(bytes[i], 2);
  return out;
}

// One line per command for the detail list. CP writes are not applied to the state: the copy
// handed in is already the state at the end of the object.
class CommandSummary final : public OpcodeDecoder::Callback
{
public:
  explicit CommandSummary(const CPState& cp_state) : m_cp_state(cp_state) {}

  OPCODE_CALLBACK(void OnCP(const u8 command, const u32 value))
  {
    const auto [name, desc] = GetCPRegInfo(command, value);
    ASSERT(!name.empty());
    text = QStringLiteral("CP  %1  %2  %3")
               .arg(Hex(command, 2), Hex(value, 8), QString::fromStdString(name));
  }

  OPCODE_CALLBACK(void OnXF(const u16 address, const u8 count, const u8* words))
  {
    const auto [name, desc] = GetXFTransferInfo(address, count, words);
    ASSERT(!name.empty());

    // The command word as it appears in the stream: (count - 1) << 16 | first register.
    text = QStringLiteral("XF  %1  ").arg(Hex(address | ((count - 1u) << 16), 8));
    for (u32 word = 0; word < count; ++word)
      text += Hex(Common::swap32(&words[word * 4]), 8) + QLatin1Char(' ');
    text += QStringLiteral("  ") + QString::fromStdString(name);
  }

  OPCODE_CALLBACK(void OnBP(const u8 command, const u32 value))
  {
    const auto [name, desc] = GetBPRegInfo(command, value);
    ASSERT(!name.empty());
    text = QStringLiteral("BP  %1  %2  %3")
               .arg(Hex(command, 2), Hex(value, 6), QString::fromStdString(name));
  }

  OPCODE_CALLBACK(void OnIndexedLoad(const CPArray array, const u32 index, const u16 address,
                                     const u8 size))
  {
    const auto [desc, written] = GetXFIndexedLoadInfo(array, index, address, size);
    text = QStringLiteral("LOAD INDX %1   %2")
               .arg(QString::fromStdString(fmt::to_string(array)), QString::fromStdString(desc));
  }

  OPCODE_CALLBACK(void OnPrimitiveCommand(const OpcodeDecoder::Primitive primitive, const u8 vat,
                                          const u32 vertex_size, const u16 num_vertices, const u8*))
  {
    // The vertex bytes go to the description instead; a hex dump here would be unreadable.
    // A count of zero is legal: the next command simply follows.
    const u32 opcode =
        0x80u | (u32{std::to_underlying(primitive)} << OpcodeDecoder::GX_PRIMITIVE_SHIFT) | vat;
    text = QStringLiteral("PRIMITIVE %1 (%2)  %3 vertices %4 bytes/vertex %5 total bytes")
               .arg(QString::fromStdString(fmt::to_string(primitive)), Hex(opcode, 2))
               .arg(num_vertices)
               .arg(vertex_size)
               .arg(num_vertices * vertex_size);
  }

  OPCODE_CALLBACK(void OnDisplayList(const u32 address, const u32 size))
  {
    text = QObject::tr("Call display list at %1 with size %2").arg(Hex(address, 8), Hex(size, 8));
  }

  OPCODE_CALLBACK(void OnNop(const u32 count))
  {
    text = count > 1 ? QStringLiteral("NOP (%1x)").arg(count) : QStringLiteral("NOP");
  }

  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8*))
  {
    switch (static_cast<OpcodeDecoder::Opcode>(opcode))
    {
    case OpcodeDecoder::Opcode::GX_CMD_UNKNOWN_METRICS:
      text = QStringLiteral("GX_CMD_UNKNOWN_METRICS");
      break;
    case OpcodeDecoder::Opcode::GX_CMD_INVL_VC:
      text = QStringLiteral("GX_CMD_INVL_VC");
      break;
    default:
      text = QStringLiteral("Unknown opcode %1").arg(opcode, 2, 16);
      break;
    }
  }

  OPCODE_CALLBACK(void OnCommand(const u8*, u32)) {}

  OPCODE_CALLBACK(CPState& GetCPState()) { return m_cp_state; }

  // Left as it was by commands that report nothing.
  QString text;

private:
  CPState m_cp_state;
};

// "<kind> register <name>", then its description or a note that there is none.
QString RegisterText(const QString& heading, const std::string& name, const std::string& desc)
{
  QString out = heading + QString::fromStdString(name) + QLatin1Char('\n');
  out += desc.empty() ? QObject::tr("No description available") : QString::fromStdString(desc);
  return out;
}

u32 DirectColorBytes(ColorFormat format)
{
  switch (format)
  {
  case ColorFormat::RGB565:
  case ColorFormat::RGBA4444:
    return 2;
  case ColorFormat::RGB888:
  case ColorFormat::RGBA6666:
    return 3;
  default:
    return 4;
  }
}

// Writes a vertex's bytes component by component: hex digits, with float values decoded.
class VertexDump
{
public:
  VertexDump(const u8* bytes, QString& out) : m_bytes(bytes), m_out(out) {}

  u32 Position() const { return m_pos; }

  // A plain field (matrix index, color) of the given size, followed by two spaces.
  void Field(u32 size)
  {
    m_out += HexBytes(m_bytes + m_pos, size);
    m_pos += size;
    m_out += QStringLiteral("  ");
  }

  // An attribute stored directly (`direct_count` values of `format`) or as an index into an
  // array (`indexed_count` 8- or 16-bit indices). Every value is followed by a space and the
  // attribute by one more.
  void Attribute(VertexComponentFormat how, ComponentFormat format, u32 direct_count,
                 u32 indexed_count = 1)
  {
    u32 count = direct_count;
    switch (how)
    {
    case VertexComponentFormat::NotPresent:
      return;
    case VertexComponentFormat::Index8:
      format = ComponentFormat::UByte;
      count = indexed_count;
      break;
    case VertexComponentFormat::Index16:
      format = ComponentFormat::UShort;
      count = indexed_count;
      break;
    default:
      break;
    }

    const u32 size = GetElementSize(format);
    for (u32 value = 0; value < count; ++value)
    {
      m_out += HexBytes(m_bytes + m_pos, size);
      if (format == ComponentFormat::Float)
      {
        const float decoded = std::bit_cast<float>(Common::swap32(m_bytes + m_pos));
        m_out += QStringLiteral(" (%1)").arg(decoded);
      }
      m_pos += size;
      m_out += QLatin1Char(' ');
    }
    m_out += QLatin1Char(' ');
  }

private:
  const u8* m_bytes;
  QString& m_out;
  u32 m_pos = 0;
};

// The longer text for the description pane.
class CommandExplanation final : public OpcodeDecoder::Callback
{
public:
  explicit CommandExplanation(const CPState& cp_state) : m_cp_state(cp_state) {}

  OPCODE_CALLBACK(void OnBP(const u8 command, const u32 value))
  {
    const auto [name, desc] = GetBPRegInfo(command, value);
    ASSERT(!name.empty());
    text = RegisterText(QObject::tr("BP register "), name, desc);
  }

  OPCODE_CALLBACK(void OnCP(const u8 command, const u32 value))
  {
    const auto [name, desc] = GetCPRegInfo(command, value);
    ASSERT(!name.empty());
    text = RegisterText(QObject::tr("CP register "), name, desc);
  }

  OPCODE_CALLBACK(void OnXF(const u16 address, const u8 count, const u8* words))
  {
    const auto [name, desc] = GetXFTransferInfo(address, count, words);
    ASSERT(!name.empty());
    text = RegisterText(QObject::tr("XF register "), name, desc);
  }

  OPCODE_CALLBACK(void OnIndexedLoad(const CPArray array, const u32 index, const u16 address,
                                     const u8 size))
  {
    const auto [desc, written] = GetXFIndexedLoadInfo(array, index, address, size);

    // What each indexed-load array is typically used for (nothing for other arrays).
    const auto typical_use = [array]() -> QString {
      switch (array)
      {
      case CPArray::XF_A:
        return QObject::tr("Usually used for position matrices");
      case CPArray::XF_B:
        // i18n: A normal matrix is a matrix used for transforming normal vectors. The word
        // "normal" does not have its usual meaning here, but rather the meaning of
        // "perpendicular to a surface".
        return QObject::tr("Usually used for normal matrices");
      case CPArray::XF_C:
        // i18n: Tex coord is short for texture coordinate
        return QObject::tr("Usually used for tex coord matrices");
      case CPArray::XF_D:
        return QObject::tr("Usually used for light objects");
      default:
        return {};
      }
    };
    text = QString::fromStdString(desc) + QLatin1Char('\n') + typical_use() + QLatin1Char('\n') +
           QString::fromStdString(written);
  }

  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          const u32 vertex_size, const u16 num_vertices,
                                          const u8* vertex_data))
  {
    // i18n: In this context, a primitive means a point, line, triangle or rectangle.
    // Do not translate the word primitive as if it was an adjective.
    text = QObject::tr("Primitive %1")
               .arg(QString::fromStdString(fmt::format("{} VAT {}", primitive, vat)));
    text += QLatin1Char('\n');

    const auto& [low, high] = m_cp_state.vtx_desc;
    const VAT& format = m_cp_state.vtx_attr[vat];

    VertexDump dump(vertex_data, text);
    for (u32 vertex = 0; vertex < num_vertices; ++vertex)
    {
      ASSERT(dump.Position() == vertex * vertex_size);
      text += QLatin1Char('\n');

      if (low.PosMatIdx)
        dump.Field(1);
      for (const bool has_index : low.TexMatIdx)
      {
        if (has_index)
          dump.Field(1);
      }

      dump.Attribute(low.Position, format.g0.PosFormat,
                     format.g0.PosElements == CoordComponentCount::XY ? 2 : 3);

      // With NBT there are three vectors (normal, binormal, tangent); indexed normals use one
      // index for all of them unless NormalIndex3 gives each its own.
      const u32 vectors = format.g0.NormalElements == NormalComponentCount::NTB ? 3 : 1;
      const u32 values_per_vector = low.Normal == VertexComponentFormat::Direct ? 3 : 1;
      dump.Attribute(low.Normal, format.g0.NormalFormat, values_per_vector * vectors,
                     format.g0.NormalIndex3 ? vectors : 1);

      for (u32 color = 0; color < low.Color.Size(); ++color)
      {
        switch (low.Color[color])
        {
        case VertexComponentFormat::Index8:
          dump.Field(1);
          break;
        case VertexComponentFormat::Index16:
          dump.Field(2);
          break;
        case VertexComponentFormat::Direct:
          dump.Field(DirectColorBytes(format.GetColorFormat(color)));
          break;
        case VertexComponentFormat::NotPresent:
          break;
        }
      }

      for (u32 coord = 0; coord < high.TexCoord.Size(); ++coord)
      {
        dump.Attribute(high.TexCoord[coord], format.GetTexFormat(coord),
                       format.GetTexElements(coord) == TexComponentCount::ST ? 2 : 1);
      }
    }
  }

  OPCODE_CALLBACK(void OnDisplayList(u32, u32)) { text = QObject::tr("No description available"); }

  OPCODE_CALLBACK(void OnNop(u32)) { text = QObject::tr("No description available"); }

  OPCODE_CALLBACK(void OnUnknown(u8, const u8*)) { text = QObject::tr("No description available"); }

  OPCODE_CALLBACK(void OnCommand(const u8*, u32)) {}

  OPCODE_CALLBACK(CPState& GetCPState()) { return m_cp_state; }

  QString text;

private:
  CPState m_cp_state;
};
}  // namespace

FIFOAnalyzer::FIFOAnalyzer(FifoPlayer& fifo_player) : m_fifo_player(fifo_player)
{
  BuildLayout();
  ConnectSignals();

  UpdateTree();

  const auto& settings = Settings::GetQSettings();
  m_object_splitter->restoreState(settings.value(OBJECT_SPLITTER_KEY).toByteArray());
  m_search_splitter->restoreState(settings.value(SEARCH_SPLITTER_KEY).toByteArray());

  OnDebugFontChanged(Settings::Instance().GetDebugFont());
  connect(&Settings::Instance(), &Settings::DebugFontChanged, this,
          &FIFOAnalyzer::OnDebugFontChanged);
}

FIFOAnalyzer::~FIFOAnalyzer()
{
  auto& settings = Settings::GetQSettings();
  settings.setValue(OBJECT_SPLITTER_KEY, m_object_splitter->saveState());
  settings.setValue(SEARCH_SPLITTER_KEY, m_search_splitter->saveState());
}

void FIFOAnalyzer::BuildLayout()
{
  m_tree_widget = new QTreeWidget;
  m_detail_list = new QListWidget;
  m_entry_detail_browser = new QTextBrowser;

  // Objects on the left, their commands on the right.
  m_object_splitter = new QSplitter(Qt::Horizontal);
  m_object_splitter->addWidget(m_tree_widget);
  m_object_splitter->addWidget(m_detail_list);
  m_tree_widget->header()->hide();

  m_search_box = new QGroupBox(tr("Search Current Object"));
  m_search_edit = new QLineEdit;
  m_search_new = new NonDefaultQPushButton(tr("Search"));
  m_search_next = new NonDefaultQPushButton(tr("Next Match"));
  m_search_previous = new NonDefaultQPushButton(tr("Previous Match"));
  m_search_label = new QLabel;

  // Nothing to step through until a search found something.
  m_search_next->setEnabled(false);
  m_search_previous->setEnabled(false);

  auto* search_row = new QHBoxLayout;
  for (QWidget* part :
       {static_cast<QWidget*>(m_search_edit), static_cast<QWidget*>(m_search_new),
        static_cast<QWidget*>(m_search_next), static_cast<QWidget*>(m_search_previous),
        static_cast<QWidget*>(m_search_label)})
  {
    search_row->addWidget(part);
  }
  m_search_box->setLayout(search_row);
  m_search_box->setMaximumHeight(m_search_box->minimumSizeHint().height());

  // Description and search below.
  m_search_splitter = new QSplitter(Qt::Vertical);
  m_search_splitter->addWidget(m_object_splitter);
  m_search_splitter->addWidget(m_entry_detail_browser);
  m_search_splitter->addWidget(m_search_box);

  auto* outer = new QHBoxLayout;
  outer->addWidget(m_search_splitter);
  setLayout(outer);
}

void FIFOAnalyzer::ConnectSignals()
{
  // Selecting in one pane refreshes the pane to its right.
  connect(m_tree_widget, &QTreeWidget::itemSelectionChanged, this, &FIFOAnalyzer::UpdateDetails);
  connect(m_detail_list, &QListWidget::currentRowChanged, this, &FIFOAnalyzer::UpdateDescription);

  // Enter in the search field acts like "Search".
  connect(m_search_edit, &QLineEdit::returnPressed, this, &FIFOAnalyzer::BeginSearch);
  connect(m_search_new, &QPushButton::clicked, this, &FIFOAnalyzer::BeginSearch);
  connect(m_search_next, &QPushButton::clicked, this, &FIFOAnalyzer::FindNext);
  connect(m_search_previous, &QPushButton::clicked, this, &FIFOAnalyzer::FindPrevious);
}

void FIFOAnalyzer::Update()
{
  // Rebuild from the left: frames/objects, then the selected object's commands, then the
  // description of the selected command.
  UpdateTree();
  UpdateDetails();
  UpdateDescription();
}

void FIFOAnalyzer::UpdateTree()
{
  m_tree_widget->clear();

  const bool have_recording = m_fifo_player.IsPlaying();
  if (!have_recording)
  {
    m_tree_widget->addTopLevelItem(new QTreeWidgetItem(QStringList{tr("No recording loaded.")}));
    return;
  }

  auto* recording = new QTreeWidgetItem({tr("Recording")});
  m_tree_widget->addTopLevelItem(recording);

  const u32 frame_count = m_fifo_player.GetFile()->GetFrameCount();
  for (u32 frame = 0; frame < frame_count; ++frame)
  {
    auto* frame_row = new QTreeWidgetItem({tr("Frame %1").arg(frame)});
    recording->addChild(frame_row);

    const auto& [parts, part_type_counts] = m_fifo_player.GetAnalyzedFrameInfo(frame);
    ASSERT(!parts.empty());

    // Commands are not listed on their own: they belong to the draw or EFB copy they lead up
    // to, which closes the object. Draws and copies are numbered separately.
    Common::EnumMap<u32, FramePartType::EFBCopy> seen;
    u32 object_start = 0;
    for (u32 part = 0; part < parts.size(); ++part)
    {
      const FramePartType type = parts[part].m_type;
      const u32 number = seen[type]++;

      QTreeWidgetItem* object_row = nullptr;
      if (type == FramePartType::PrimitiveData)
        object_row = new QTreeWidgetItem({tr("Object %1").arg(number)});
      else if (type == FramePartType::EFBCopy)
        object_row = new QTreeWidgetItem({tr("EFB copy %1").arg(number)});
      if (!object_row)
        continue;

      frame_row->addChild(object_row);
      object_row->setData(0, FRAME_ROLE, frame);
      object_row->setData(0, PART_START_ROLE, object_start);
      object_row->setData(0, PART_END_ROLE, part);
      object_start = part + 1;
    }

    // A frame ends with an EFB copy, so no commands are left over, and the tally agrees with
    // the one made when the frame was analyzed.
    ASSERT(object_start == parts.size());
    ASSERT(std::ranges::equal(part_type_counts, seen));
  }
}

std::optional<FIFOAnalyzer::ObjectSpan> FIFOAnalyzer::SelectedObject() const
{
  const QList<QTreeWidgetItem*> selected = m_tree_widget->selectedItems();
  if (selected.isEmpty())
    return std::nullopt;

  const QTreeWidgetItem* row = selected[0];
  if (row->data(0, FRAME_ROLE).isNull() || row->data(0, PART_START_ROLE).isNull())
    return std::nullopt;

  return ObjectSpan{row->data(0, FRAME_ROLE).toUInt(), row->data(0, PART_START_ROLE).toUInt(),
                    row->data(0, PART_END_ROLE).toUInt()};
}

FIFOAnalyzer::ObjectBytes FIFOAnalyzer::BytesOf(const ObjectSpan& span) const
{
  const auto& parts = m_fifo_player.GetAnalyzedFrameInfo(span.frame).parts;
  const FifoFrameInfo& frame = m_fifo_player.GetFile()->GetFrame(span.frame);

  const u32 start = parts[span.first_part].m_start;
  const u32 end = parts[span.last_part].m_end;
  return {frame.fifoData.data() + start, start, end - start};
}

void FIFOAnalyzer::UpdateDetails()
{
  // Clearing the list moves its current row, which runs UpdateDescription right away; with the
  // offsets emptied first it returns without touching the old object's data.
  m_object_data_offsets.clear();
  m_detail_list->clear();
  m_search_results.clear();
  m_search_next->setEnabled(false);
  m_search_previous->setEnabled(false);
  m_search_label->clear();

  if (!m_fifo_player.IsPlaying())
    return;

  const std::optional<ObjectSpan> span = SelectedObject();
  if (!span)
    return;

  const ObjectBytes object = BytesOf(*span);

  // The CP state is the one after the whole object; it is right by the time any draw is reached.
  CommandSummary summary(
      m_fifo_player.GetAnalyzedFrameInfo(span->frame).parts[span->last_part].m_cpmem);

  u32 offset = 0;
  while (offset < object.size)
  {
    const u32 command_start = offset;
    m_object_data_offsets.push_back(command_start);
    offset += OpcodeDecoder::RunCommand(object.begin + command_start, object.size - command_start,
                                        summary);

    m_detail_list->addItem(QStringLiteral("%1:  ").arg(Hex(object.fifo_offset + command_start, 8)) +
                           summary.text);
  }

  // Selecting the first command also refreshes the description for the new object.
  m_detail_list->setCurrentRow(0);
}

void FIFOAnalyzer::BeginSearch()
{
  const QString query = m_search_edit->text();

  if (!m_fifo_player.IsPlaying())
    return;

  const std::optional<ObjectSpan> span = SelectedObject();
  if (!span)
  {
    m_search_label->setText(tr("Invalid search parameters (no object selected)"));
    return;
  }

  QTreeWidgetItem* object_row = m_tree_widget->selectedItems()[0];
  const int row_in_frame = object_row->parent()->indexOfChild(object_row);

  // The query is a run of hex bytes, two digits each.
  if (query.length() % 2 != 0)
  {
    m_search_label->setText(tr("Invalid search string (only even string lengths supported)"));
    return;
  }

  std::vector<u8> needle;
  for (qsizetype at = 0; at < query.length(); at += 2)
  {
    bool parsed = false;
    const uint byte = query.mid(at, 2).toUInt(&parsed, 16);
    if (!parsed)
    {
      m_search_label->setText(tr("Invalid search string (couldn't convert to number)"));
      return;
    }
    needle.push_back(static_cast<u8>(byte));
  }

  m_search_results.clear();

  // Each command that contains the bytes counts once.
  const ObjectBytes object = BytesOf(*span);
  const u32 length = static_cast<u32>(needle.size());
  for (u32 command = 0; command < m_object_data_offsets.size(); ++command)
  {
    const u32 command_start = m_object_data_offsets[command];
    const u32 command_end = command + 1 == m_object_data_offsets.size() ?
                                object.size :
                                m_object_data_offsets[command + 1];

    for (u32 at = command_start; at + length <= command_end; ++at)
    {
      if (std::equal(needle.begin(), needle.end(), object.begin + at))
      {
        m_search_results.push_back({span->frame, static_cast<u32>(row_in_frame), command});
        break;
      }
    }
  }

  ShowSearchResult(0);

  m_search_label->setText(
      tr("Found %1 results for \"%2\"").arg(m_search_results.size()).arg(query));
}

void FIFOAnalyzer::FindNext()
{
  const int current = m_detail_list->currentRow();
  ASSERT(current >= 0);

  const auto next = std::ranges::find_if(m_search_results, [current](const SearchResult& hit) {
    return hit.command > static_cast<u32>(current);
  });
  if (next != m_search_results.end())
    ShowSearchResult(static_cast<size_t>(next - m_search_results.begin()));
}

void FIFOAnalyzer::FindPrevious()
{
  const int current = m_detail_list->currentRow();
  ASSERT(current >= 0);

  for (size_t i = m_search_results.size(); i-- > 0;)
  {
    if (m_search_results[i].command < static_cast<u32>(current))
    {
      ShowSearchResult(i);
      return;
    }
  }
}

void FIFOAnalyzer::ShowSearchResult(size_t index)
{
  if (m_search_results.empty())
    return;

  // Past the end means the last one.
  index = std::min(index, m_search_results.size() - 1);
  const SearchResult hit = m_search_results[index];

  m_tree_widget->setCurrentItem(m_tree_widget->topLevelItem(0)
                                    ->child(static_cast<int>(hit.frame))
                                    ->child(static_cast<int>(hit.object_row)));
  m_detail_list->setCurrentRow(static_cast<int>(hit.command));

  m_search_next->setEnabled(index + 1 < m_search_results.size());
  m_search_previous->setEnabled(index > 0);
}

void FIFOAnalyzer::UpdateDescription()
{
  m_entry_detail_browser->clear();

  if (!m_fifo_player.IsPlaying())
    return;

  if (m_tree_widget->selectedItems().isEmpty() || m_object_data_offsets.empty())
    return;

  const std::optional<ObjectSpan> span = SelectedObject();
  if (!span)
    return;

  const u32 command = static_cast<u32>(m_detail_list->currentRow());
  const ObjectBytes object = BytesOf(*span);
  const u32 command_start = m_object_data_offsets[command];

  CommandExplanation explanation(
      m_fifo_player.GetAnalyzedFrameInfo(span->frame).parts[span->last_part].m_cpmem);
  OpcodeDecoder::RunCommand(object.begin + command_start, object.size - command_start, explanation);
  m_entry_detail_browser->setText(explanation.text);
}

void FIFOAnalyzer::OnDebugFontChanged(const QFont& font)
{
  m_detail_list->setFont(font);
  m_entry_detail_browser->setFont(font);
}
