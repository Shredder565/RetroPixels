// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "Core/HW/WiimoteEmu/I2CBus.h"

namespace Common
{
class Matrix44;
}

// The remote's IR camera: an i2c device (slave 0x58) that tracks up to four bright spots and
// reports them in one of three formats (WiiBrew: "Wiimote", IR Camera).

namespace WiimoteEmu
{
using IRObject = Common::TVec2<u16>;

// One tracked spot. All bits set means "nothing seen".
struct CameraPoint
{
  constexpr CameraPoint() : position({0xffff, 0xffff}), size(0xff) {}
  constexpr CameraPoint(IRObject position_, u8 size_) : position(position_), size(size_) {}

  constexpr bool operator==(const CameraPoint& other) const
  {
    return position == other.position && size == other.size;
  }

  IRObject position;
  u8 size;
};

// Basic format: two objects in five bytes (10 bit coordinates, no size).
struct IRBasic
{
  u8 x1;
  u8 y1;
  u8 x2hi : 2;
  u8 y2hi : 2;
  u8 x1hi : 2;
  u8 y1hi : 2;
  u8 x2;
  u8 y2;

  auto GetObject1() const { return IRObject(Join(x1hi, x1), Join(y1hi, y1)); }
  auto GetObject2() const { return IRObject(Join(x2hi, x2), Join(y2hi, y2)); }

  void SetObject1(const IRObject& obj)
  {
    x1 = Low(obj.x);
    x1hi = High(obj.x);
    y1 = Low(obj.y);
    y1hi = High(obj.y);
  }
  void SetObject2(const IRObject& obj)
  {
    x2 = Low(obj.x);
    x2hi = High(obj.x);
    y2 = Low(obj.y);
    y2hi = High(obj.y);
  }

  static u16 Join(u8 high, u8 low) { return static_cast<u16>((high << 8) | low); }
  static u8 Low(u16 value) { return static_cast<u8>(value); }
  static u8 High(u16 value) { return static_cast<u8>((value >> 8) & 0b11); }
};
static_assert(sizeof(IRBasic) == 5, "Wrong size");

// Extended format: one object in three bytes, with a 4 bit size.
struct IRExtended
{
  u8 x;
  u8 y;
  u8 size : 4;
  u8 xhi : 2;
  u8 yhi : 2;

  auto GetPosition() const { return IRObject(IRBasic::Join(xhi, x), IRBasic::Join(yhi, y)); }
  void SetPosition(const IRObject& obj)
  {
    x = IRBasic::Low(obj.x);
    xhi = IRBasic::High(obj.x);
    y = IRBasic::Low(obj.y);
    yhi = IRBasic::High(obj.y);
  }
};
static_assert(sizeof(IRExtended) == 3, "Wrong size");

// Full format: the extended bytes plus a bounding box and an intensity (nine bytes).
struct IRFull : IRExtended
{
  u8 xmin : 7;
  u8 : 1;
  u8 ymin : 7;
  u8 : 1;
  u8 xmax : 7;
  u8 : 1;
  u8 ymax : 7;
  u8 : 1;
  u8 zero;
  u8 intensity;
};
static_assert(sizeof(IRFull) == 9, "Wrong size");

class CameraLogic : public I2CSlave
{
public:
  static constexpr u8 I2C_ADDR = 0x58;

  // Camera resolution in the reported coordinates.
  static constexpr int CAMERA_RES_X = 1024;
  static constexpr int CAMERA_RES_Y = 768;

  // 42 degrees across, 4:3 aspect ratio.
  static constexpr float CAMERA_AR = 4.f / 3;
  static constexpr float CAMERA_FOV_X = 42 * float(MathUtil::TAU) / 360;
  static constexpr float CAMERA_FOV_Y = CAMERA_FOV_X / CAMERA_AR;

  // Distance between the two LED groups of a Nintendo sensor bar, in meters.
  static constexpr float SENSOR_BAR_LED_SEPARATION = 0.2f;

  // Values of the mode register (0x33).
  enum : u8
  {
    IR_MODE_BASIC = 1,
    IR_MODE_EXTENDED = 3,
    IR_MODE_FULL = 5,
  };

  // The camera can track four spots. A sensor bar normally shows up as two of them.
  static constexpr int NUM_POINTS = 4;

  // Spot sizes go from 0 to 15; nearby LEDs show up as about 2-4.
  static constexpr int MAX_POINT_SIZE = 15;

  static constexpr int CAMERA_DATA_BYTES = 36;

  void Reset();
  void DoState(PointerWrap& p);

  // Where the two sensor bar LED groups appear for a given view of the world.
  static std::array<CameraPoint, NUM_POINTS> GetCameraPoints(const Common::Matrix44& transform,
                                                             Common::Vec2 field_of_view);

  // Writes the points into the object data registers in the configured format.
  void Update(const std::array<CameraPoint, NUM_POINTS>& camera_points);

  // Output report 0x13. A disabled camera does not answer on the bus.
  void SetEnabled(bool is_enabled);

private:
#pragma pack(push, 1)
  struct Register
  {
    // 0x00: sensitivity block 1 (written by the Wii), then registers of unknown use.
    std::array<u8, 9> sensitivity_block1;
    std::array<u8, 17> unk_0x09;

    // 0x1a: sensitivity block 2.
    std::array<u8, 2> sensitivity_block2;
    std::array<u8, 20> unk_0x1c;

    // 0x30: must be 0x08 for the camera to track objects.
    u8 enable_object_tracking;
    std::array<u8, 2> unk_0x31;

    // 0x33: report format.
    u8 mode;
    std::array<u8, 3> unk_0x34;

    // 0x37: object data, read by the remote for its data reports.
    std::array<u8, CAMERA_DATA_BYTES> camera_data;
    std::array<u8, 165> unk_0x5b;
  };
#pragma pack(pop)

  static_assert(0x100 == sizeof(Register));

public:
  // Start of the object data on the bus.
  static const u8 REPORT_DATA_OFFSET = offsetof(Register, camera_data);

private:
  int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out) override;
  int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in) override;

  Register m_reg_data{};

  bool m_is_enabled = false;
};
}  // namespace WiimoteEmu
