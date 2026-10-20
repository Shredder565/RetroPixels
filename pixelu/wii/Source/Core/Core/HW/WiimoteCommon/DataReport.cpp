// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteCommon/DataReport.h"

#include <cstring>

#include "Common/Assert.h"
#include "Common/BitUtils.h"

namespace WiimoteCommon
{
namespace
{
// How a data report mode carries accelerometer data.
enum class AccelLayout : u8
{
  Absent,
  // Three bytes after the core bytes hold bits 2-9 of X/Y/Z. The core bytes add bits 0-1 of X
  // and bit 1 of Y and Z.
  Standard,
  // Byte 2 holds X bits 2-9; the core bytes hold Z bits 6-9.
  InterleaveFirst,
  // Byte 2 holds Y bits 2-9; the core bytes hold Z bits 2-5.
  InterleaveSecond,
};

// Where each piece of a data report lives (offsets are into the report body).
struct ReportLayout
{
  InputReportID mode;
  bool has_core;
  AccelLayout accel;
  u8 ir_offset;
  u8 ir_size;
  IRReportFormat ir_format;
  u8 ir_camera_offset;
  u8 ext_offset;
  u8 ext_size;
  u8 body_size;
};

// The data reporting modes, from WiiBrew's "Wiimote" page (Data Reporting). Fields: mode, core,
// accel, IR offset/size/format/camera offset, extension offset/size, body size.
constexpr ReportLayout LAYOUTS[] = {
    {InputReportID::ReportCore, true, AccelLayout::Absent, 0, 0, IRReportFormat::None, 0, 0, 0, 2},
    {InputReportID::ReportCoreAccel, true, AccelLayout::Standard, 0, 0, IRReportFormat::None, 0, 0,
     0, 5},
    {InputReportID::ReportCoreExt8, true, AccelLayout::Absent, 0, 0, IRReportFormat::None, 0, 2, 8,
     10},
    {InputReportID::ReportCoreAccelIR12, true, AccelLayout::Standard, 5, 12,
     IRReportFormat::Extended, 0, 0, 0, 17},
    {InputReportID::ReportCoreExt19, true, AccelLayout::Absent, 0, 0, IRReportFormat::None, 0, 2,
     19, 21},
    {InputReportID::ReportCoreAccelExt16, true, AccelLayout::Standard, 0, 0, IRReportFormat::None,
     0, 5, 16, 21},
    {InputReportID::ReportCoreIR10Ext9, true, AccelLayout::Absent, 2, 10, IRReportFormat::Basic, 0,
     12, 9, 21},
    {InputReportID::ReportCoreAccelIR10Ext6, true, AccelLayout::Standard, 5, 10,
     IRReportFormat::Basic, 0, 15, 6, 21},
    {InputReportID::ReportExt21, false, AccelLayout::Absent, 0, 0, IRReportFormat::None, 0, 0, 21,
     21},
    {InputReportID::ReportInterleave1, true, AccelLayout::InterleaveFirst, 3, 18,
     IRReportFormat::Full1, 0, 0, 0, 21},
    {InputReportID::ReportInterleave2, true, AccelLayout::InterleaveSecond, 3, 18,
     IRReportFormat::Full2, 18, 0, 0, 21},
};

const ReportLayout* FindLayout(InputReportID mode)
{
  for (const ReportLayout& layout : LAYOUTS)
  {
    if (layout.mode == mode)
      return &layout;
  }
  return nullptr;
}

// Positions of the accelerometer bits inside the two core bytes.
constexpr u8 CORE_ACCEL_SHIFT = 5;
constexpr u8 CORE_ACCEL_MASK = 0b11 << CORE_ACCEL_SHIFT;

u8 CoreAccelBits(const u8* core, int byte)
{
  return static_cast<u8>((core[byte] & CORE_ACCEL_MASK) >> CORE_ACCEL_SHIFT);
}

void SetCoreAccelBits(u8* core, int byte, u32 value)
{
  const u8 kept = static_cast<u8>(core[byte] & ~CORE_ACCEL_MASK);
  core[byte] = static_cast<u8>(kept | ((value & 0b11) << CORE_ACCEL_SHIFT));
}

u16 Expand(u32 value, size_t bits)
{
  return Common::ExpandValue<u16>(static_cast<u16>(value), bits);
}

// The one manipulator type: everything it does is driven by the mode's layout entry.
class LayoutManipulator final : public DataReportManipulator
{
public:
  explicit LayoutManipulator(const ReportLayout& layout) : m_layout(layout) {}

  bool HasCore() const override { return m_layout.has_core; }
  bool HasAccel() const override { return m_layout.accel != AccelLayout::Absent; }

  void GetCoreData(CoreData* result) const override
  {
    if (!m_layout.has_core)
      return;
    u16 raw;
    std::memcpy(&raw, data_ptr, sizeof(raw));
    result->hex = raw & CoreData::BUTTON_MASK;
  }

  void SetCoreData(const CoreData& buttons) override
  {
    if (!m_layout.has_core)
      return;
    u16 raw;
    std::memcpy(&raw, data_ptr, sizeof(raw));
    // The non-button bits belong to the accelerometer and stay as they are.
    raw = static_cast<u16>((raw & ~CoreData::BUTTON_MASK) | (buttons.hex & CoreData::BUTTON_MASK));
    std::memcpy(data_ptr, &raw, sizeof(raw));
  }

  void GetAccelData(AccelData* accel) const override
  {
    auto& value = accel->value;
    switch (m_layout.accel)
    {
    case AccelLayout::Standard:
    {
      const u8 low_x = CoreAccelBits(data_ptr, 0);
      const u8 low_yz = CoreAccelBits(data_ptr, 1);
      value.x = static_cast<u16>((data_ptr[2] << 2) | low_x);
      // Y and Z only have 9 bits here; the missing low bit is filled in.
      value.y = Expand(static_cast<u32>(data_ptr[3] << 1) | (low_yz & 1u), 1);
      value.z = Expand(static_cast<u32>(data_ptr[4] << 1) | ((low_yz >> 1) & 1u), 1);
      break;
    }
    case AccelLayout::InterleaveFirst:
    {
      // X has 8 bits in this report. Z bits 6-9 are here; bits 0-5 come from the second report.
      value.x = Expand(data_ptr[2], 2);
      const u32 z_high =
          (u32{CoreAccelBits(data_ptr, 0)} << 6) | (u32{CoreAccelBits(data_ptr, 1)} << 8);
      value.z = static_cast<u16>((value.z & 0x3fu) | z_high);
      break;
    }
    case AccelLayout::InterleaveSecond:
    {
      // Y has 8 bits in this report. Z bits 2-5 are here; the upper bits come from the first.
      value.y = Expand(data_ptr[2], 2);
      const u32 z_mid = CoreAccelBits(data_ptr, 0) | (u32{CoreAccelBits(data_ptr, 1)} << 2);
      value.z = static_cast<u16>((value.z & 0xffc0u) | Expand(z_mid, 2));
      break;
    }
    case AccelLayout::Absent:
      break;
    }
  }

  void SetAccelData(const AccelData& accel) override
  {
    const auto& value = accel.value;
    switch (m_layout.accel)
    {
    case AccelLayout::Standard:
      data_ptr[2] = static_cast<u8>(value.x >> 2);
      data_ptr[3] = static_cast<u8>(value.y >> 2);
      data_ptr[4] = static_cast<u8>(value.z >> 2);
      SetCoreAccelBits(data_ptr, 0, value.x);
      // Only Y's bit 1 is sent; the slot for Z's bit 1 is always left clear.
      SetCoreAccelBits(data_ptr, 1, (value.y >> 1) & 1u);
      break;
    case AccelLayout::InterleaveFirst:
      data_ptr[2] = static_cast<u8>(value.x >> 2);
      SetCoreAccelBits(data_ptr, 0, value.z >> 6);
      SetCoreAccelBits(data_ptr, 1, value.z >> 8);
      break;
    case AccelLayout::InterleaveSecond:
      data_ptr[2] = static_cast<u8>(value.y >> 2);
      SetCoreAccelBits(data_ptr, 0, value.z >> 2);
      SetCoreAccelBits(data_ptr, 1, value.z >> 4);
      break;
    case AccelLayout::Absent:
      break;
    }
  }

  u8* GetIRDataPtr() override { return m_layout.ir_size ? data_ptr + m_layout.ir_offset : nullptr; }
  const u8* GetIRDataPtr() const override
  {
    return m_layout.ir_size ? data_ptr + m_layout.ir_offset : nullptr;
  }
  u32 GetIRDataSize() const override { return m_layout.ir_size; }
  u32 GetIRDataFormatOffset() const override { return m_layout.ir_camera_offset; }
  IRReportFormat GetIRReportFormat() const override { return m_layout.ir_format; }

  u8* GetExtDataPtr() override
  {
    return m_layout.ext_size ? data_ptr + m_layout.ext_offset : nullptr;
  }
  const u8* GetExtDataPtr() const override
  {
    return m_layout.ext_size ? data_ptr + m_layout.ext_offset : nullptr;
  }
  u32 GetExtDataSize() const override { return m_layout.ext_size; }

  u32 GetDataSize() const override { return m_layout.body_size; }

private:
  const ReportLayout& m_layout;
};
}  // namespace

bool DataReportManipulator::HasIR() const
{
  return GetIRDataSize() != 0;
}

bool DataReportManipulator::HasExt() const
{
  return GetExtDataSize() != 0;
}

u8* DataReportManipulator::GetDataPtr()
{
  return data_ptr;
}

const u8* DataReportManipulator::GetDataPtr() const
{
  return data_ptr;
}

std::unique_ptr<DataReportManipulator> MakeDataReportManipulator(InputReportID rpt_id, u8* data_ptr)
{
  const ReportLayout* layout = FindLayout(rpt_id);
  ASSERT(layout != nullptr);
  if (layout == nullptr)
    layout = &LAYOUTS[0];

  auto manipulator = std::make_unique<LayoutManipulator>(*layout);
  manipulator->data_ptr = data_ptr;
  return manipulator;
}

DataReportBuilder::DataReportBuilder(InputReportID rpt_id) : m_data(rpt_id)
{
  SetMode(rpt_id);
}

bool DataReportBuilder::IsValidMode(InputReportID mode)
{
  return FindLayout(mode) != nullptr;
}

void DataReportBuilder::SetMode(InputReportID rpt_id)
{
  m_data.report_id = rpt_id;
  m_manip = MakeDataReportManipulator(rpt_id, m_data.payload.data());
}

InputReportID DataReportBuilder::GetMode() const
{
  return m_data.report_id;
}

bool DataReportBuilder::HasCore() const
{
  return m_manip->HasCore();
}

bool DataReportBuilder::HasAccel() const
{
  return m_manip->HasAccel();
}

bool DataReportBuilder::HasIR() const
{
  return m_manip->HasIR();
}

bool DataReportBuilder::HasExt() const
{
  return m_manip->HasExt();
}

void DataReportBuilder::GetCoreData(CoreData* core) const
{
  m_manip->GetCoreData(core);
}

void DataReportBuilder::SetCoreData(const CoreData& core)
{
  m_manip->SetCoreData(core);
}

void DataReportBuilder::GetAccelData(AccelData* accel) const
{
  m_manip->GetAccelData(accel);
}

void DataReportBuilder::SetAccelData(const AccelData& accel)
{
  m_manip->SetAccelData(accel);
}

u32 DataReportBuilder::GetIRDataSize() const
{
  return m_manip->GetIRDataSize();
}

u32 DataReportBuilder::GetIRDataFormatOffset() const
{
  return m_manip->GetIRDataFormatOffset();
}

IRReportFormat DataReportBuilder::GetIRReportFormat() const
{
  return m_manip->GetIRReportFormat();
}

u8* DataReportBuilder::GetIRDataPtr()
{
  return m_manip->GetIRDataPtr();
}

const u8* DataReportBuilder::GetIRDataPtr() const
{
  return m_manip->GetIRDataPtr();
}

u32 DataReportBuilder::GetExtDataSize() const
{
  return m_manip->GetExtDataSize();
}

u8* DataReportBuilder::GetExtDataPtr()
{
  return m_manip->GetExtDataPtr();
}

const u8* DataReportBuilder::GetExtDataPtr() const
{
  return m_manip->GetExtDataPtr();
}

u8* DataReportBuilder::GetDataPtr()
{
  return m_data.GetData();
}

const u8* DataReportBuilder::GetDataPtr() const
{
  return m_data.GetData();
}

u32 DataReportBuilder::GetDataSize() const
{
  return m_manip->GetDataSize() + static_cast<u32>(sizeof(m_data.report_id));
}

}  // namespace WiimoteCommon
