// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/CheatSearchFactoryWidget.h"

#include <string>
#include <utility>
#include <vector>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "Common/StringUtil.h"
#include "Core/CheatSearch.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"

Q_DECLARE_METATYPE(Cheats::DataType);

namespace
{
// Where MEM1 and MEM2 appear in the virtual address space.
constexpr u32 MEM1_VIRTUAL_BASE = 0x80000000;
constexpr u32 MEM2_VIRTUAL_BASE = 0x90000000;

// A custom range may end at most at the top of the 32-bit address space.
constexpr u64 ADDRESS_SPACE_END = 0x1'0000'0000;

// Index of "32-bit Signed Integer", the type selected at first.
constexpr int DEFAULT_DATA_TYPE_INDEX = 6;
}  // namespace

CheatSearchFactoryWidget::CheatSearchFactoryWidget()
{
  auto* const layout = new QVBoxLayout{this};
  layout->addWidget(BuildAddressSpaceGroup());
  layout->addWidget(BuildDataTypeGroup());

  m_new_search = new NonDefaultQPushButton(tr("New Search"));
  layout->addWidget(m_new_search);
  layout->addStretch();

  connect(m_new_search, &QPushButton::clicked, this, &CheatSearchFactoryWidget::CreateSession);
  connect(m_standard_space, &QPushButton::toggled, this,
          &CheatSearchFactoryWidget::UpdateCustomRangeControls);
  connect(m_custom_space, &QRadioButton::toggled, this,
          &CheatSearchFactoryWidget::UpdateCustomRangeControls);

  UpdateCustomRangeControls();
}

CheatSearchFactoryWidget::~CheatSearchFactoryWidget() = default;

QWidget* CheatSearchFactoryWidget::BuildAddressSpaceGroup()
{
  auto* const group = new QGroupBox(tr("Address Space"));
  auto* const group_layout = new QVBoxLayout();
  group->setLayout(group_layout);

  // Standard and custom are exclusive with each other through their common parent; the three
  // custom modes have a button group of their own.
  m_standard_space = new QRadioButton(tr("Typical GameCube/Wii Address Space"));
  m_standard_space->setChecked(true);
  m_custom_space = new QRadioButton(tr("Custom Address Space"));

  auto* const standard_hint =
      new QLabel(tr("Sets up the search using standard MEM1 and (on Wii) MEM2 mappings in virtual "
                    "address space. This will work for the vast majority of games."));
  standard_hint->setWordWrap(true);

  auto* const custom_layout = new QVBoxLayout();
  custom_layout->setContentsMargins(6, 6, 6, 6);

  auto* const modes = new QButtonGroup(this);
  m_virtual_addresses = new QRadioButton(tr("Use virtual addresses when possible"));
  m_virtual_addresses->setChecked(true);
  m_physical_addresses = new QRadioButton(tr("Use physical addresses"));
  m_effective_addresses = new QRadioButton(tr("Use memory mapper configuration at time of scan"));
  for (QRadioButton* const mode : {m_virtual_addresses, m_physical_addresses, m_effective_addresses})
    modes->addButton(mode);
  for (QRadioButton* const mode : {m_virtual_addresses, m_physical_addresses, m_effective_addresses})
    custom_layout->addWidget(mode);

  auto* const start_label = new QLabel(tr("Range Start: "));
  m_range_start = new QLineEdit(QStringLiteral("0x80000000"));
  auto* const end_label = new QLabel(tr("Range End: "));
  m_range_end = new QLineEdit(QStringLiteral("0x81800000"));
  custom_layout->addWidget(start_label);
  custom_layout->addWidget(m_range_start);
  custom_layout->addWidget(end_label);
  custom_layout->addWidget(m_range_end);

  group_layout->addWidget(m_standard_space);
  group_layout->addWidget(standard_hint);
  group_layout->addWidget(m_custom_space);
  group_layout->addLayout(custom_layout);
  return group;
}

QWidget* CheatSearchFactoryWidget::BuildDataTypeGroup()
{
  auto* const group = new QGroupBox(tr("Data Type"));
  auto* const group_layout = new QVBoxLayout();
  group->setLayout(group_layout);

  m_data_type = new QComboBox();
  const std::pair<QString, Cheats::DataType> types[] = {
      {tr("8-bit Unsigned Integer"), Cheats::DataType::U8},
      {tr("16-bit Unsigned Integer"), Cheats::DataType::U16},
      {tr("32-bit Unsigned Integer"), Cheats::DataType::U32},
      {tr("64-bit Unsigned Integer"), Cheats::DataType::U64},
      {tr("8-bit Signed Integer"), Cheats::DataType::S8},
      {tr("16-bit Signed Integer"), Cheats::DataType::S16},
      {tr("32-bit Signed Integer"), Cheats::DataType::S32},
      {tr("64-bit Signed Integer"), Cheats::DataType::S64},
      {tr("32-bit Float"), Cheats::DataType::F32},
      {tr("64-bit Float"), Cheats::DataType::F64},
  };
  for (const auto& [name, type] : types)
    m_data_type->addItem(name, QVariant::fromValue(type));
  m_data_type->setCurrentIndex(DEFAULT_DATA_TYPE_INDEX);
  group_layout->addWidget(m_data_type);

  m_aligned = new QCheckBox(tr("Aligned to data type length"));
  m_aligned->setChecked(true);
  group_layout->addWidget(m_aligned);
  return group;
}

void CheatSearchFactoryWidget::UpdateCustomRangeControls()
{
  const bool custom = m_custom_space->isChecked();
  for (QWidget* const control : std::initializer_list<QWidget*>{
           m_virtual_addresses, m_physical_addresses, m_effective_addresses, m_range_start,
           m_range_end})
  {
    control->setEnabled(custom);
  }
}

void CheatSearchFactoryWidget::CreateSession()
{
  std::vector<Cheats::MemoryRange> ranges;
  PowerPC::RequestedAddressSpace space = PowerPC::RequestedAddressSpace::Virtual;

  if (m_standard_space->isChecked())
  {
    // The standard ranges depend on the console and memory size of the running game.
    auto& system = Core::System::GetInstance();
    if (!Core::IsRunning(system))
    {
      ModalMessageBox::warning(
          this, tr("No game running."),
          tr("Please start a game before starting a search with standard memory regions."));
      return;
    }

    auto& memory = system.GetMemory();
    ranges.emplace_back(MEM1_VIRTUAL_BASE, memory.GetRamSizeReal());
    if (system.IsWii())
      ranges.emplace_back(MEM2_VIRTUAL_BASE, memory.GetExRamSizeReal());
  }
  else
  {
    // Invalid or empty ranges are silently ignored.
    u64 start = 0;
    u64 end = 0;
    if (!TryParse(m_range_start->text().toStdString(), &start) ||
        !TryParse(m_range_end->text().toStdString(), &end))
    {
      return;
    }
    if (end <= start || end > ADDRESS_SPACE_END)
      return;

    ranges.emplace_back(static_cast<u32>(start), end - start);

    if (m_virtual_addresses->isChecked())
      space = PowerPC::RequestedAddressSpace::Virtual;
    else if (m_physical_addresses->isChecked())
      space = PowerPC::RequestedAddressSpace::Physical;
    else
      space = PowerPC::RequestedAddressSpace::Effective;
  }

  const bool aligned = m_aligned->isChecked();
  const auto type = m_data_type->currentData().value<Cheats::DataType>();
  const auto session = Cheats::MakeSession(std::move(ranges), space, aligned, type);
  if (session)
    emit NewSessionCreated(*session);
}
