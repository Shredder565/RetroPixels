// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"

namespace WiimoteCommon
{
// Reads and writes the pieces of a data report (IDs 0x30-0x3f) in place, at data_ptr (the byte
// after the report ID). Getters and setters for a piece the mode does not carry do nothing.
class DataReportManipulator
{
public:
  using CoreData = ButtonData;

  virtual ~DataReportManipulator() = default;

  virtual bool HasCore() const = 0;
  virtual bool HasAccel() const = 0;
  bool HasIR() const;
  bool HasExt() const;

  virtual void GetCoreData(CoreData*) const = 0;
  virtual void SetCoreData(const CoreData&) = 0;

  // The interleaved modes only hold part of the Z axis; GetAccelData then updates *accel
  // in place and keeps the bits the other half of the pair provides.
  virtual void GetAccelData(AccelData*) const = 0;
  virtual void SetAccelData(const AccelData&) = 0;

  virtual u8* GetIRDataPtr() = 0;
  virtual const u8* GetIRDataPtr() const = 0;
  virtual u32 GetIRDataSize() const = 0;
  // Where this report's IR bytes start within the camera's 36 byte object data.
  virtual u32 GetIRDataFormatOffset() const = 0;
  virtual IRReportFormat GetIRReportFormat() const = 0;

  virtual u8* GetExtDataPtr() = 0;
  virtual const u8* GetExtDataPtr() const = 0;
  virtual u32 GetExtDataSize() const = 0;

  u8* GetDataPtr();
  const u8* GetDataPtr() const;

  // Size of the report body (without the report ID).
  virtual u32 GetDataSize() const = 0;

  u8* data_ptr = nullptr;
};

std::unique_ptr<DataReportManipulator> MakeDataReportManipulator(InputReportID rpt_id,
                                                                 u8* data_ptr);

// Owns the buffer of one data report (report ID included) and edits it.
class DataReportBuilder
{
public:
  using CoreData = ButtonData;

  // The largest body is that of mode 0x3d (21 extension bytes).
  static constexpr int MAX_DATA_SIZE = 21;

  explicit DataReportBuilder(InputReportID rpt_id);

  static bool IsValidMode(InputReportID rpt_id);

  void SetMode(InputReportID rpt_id);
  InputReportID GetMode() const;

  bool HasCore() const;
  bool HasAccel() const;
  bool HasIR() const;
  bool HasExt() const;

  void GetCoreData(CoreData*) const;
  void SetCoreData(const CoreData&);
  void GetAccelData(AccelData*) const;
  void SetAccelData(const AccelData&);

  u32 GetIRDataSize() const;
  u32 GetIRDataFormatOffset() const;
  IRReportFormat GetIRReportFormat() const;
  u8* GetIRDataPtr();
  const u8* GetIRDataPtr() const;

  u32 GetExtDataSize() const;
  u8* GetExtDataPtr();
  const u8* GetExtDataPtr() const;

  // The whole report, report ID first.
  u8* GetDataPtr();
  const u8* GetDataPtr() const;
  u32 GetDataSize() const;

private:
  TypedInputData<std::array<u8, MAX_DATA_SIZE>> m_data;

  std::unique_ptr<DataReportManipulator> m_manip;
};

}  // namespace WiimoteCommon
