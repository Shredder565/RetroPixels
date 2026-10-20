// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Camera.h"

#include <algorithm>
#include <cmath>

#include "Common/BitUtils.h"
#include "Common/ChunkFile.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"

namespace WiimoteEmu
{
namespace
{
// Register 0x30 must hold this for the camera to produce object data.
constexpr u8 TRACKING_ON = 0x08;

// Projects one sensor bar LED group. Returns an invisible point when it is behind the camera or
// outside the picture.
CameraPoint ProjectLed(const Common::Matrix44& camera_view, const Common::Vec3& led)
{
  const Common::Vec4 clip = camera_view * Common::Vec4(led, 1.0);
  if (clip.z < 0)
    return CameraPoint();

  // Truncation (instead of rounding) keeps the cursor symmetric around the center.
  const s32 x = s32((1 - clip.x / clip.w) * CameraLogic::CAMERA_RES_X / 2);
  const s32 y = s32((1 - clip.y / clip.w) * CameraLogic::CAMERA_RES_Y / 2);

  const bool inside =
      x >= 0 && y >= 0 && x < CameraLogic::CAMERA_RES_X && y < CameraLogic::CAMERA_RES_Y;
  if (!inside)
    return CameraPoint();

  // Spot size against distance, fitted to a real remote at the Wii's middle sensitivity setting.
  const float apparent_size = 2.37f * std::pow(clip.z, -0.778f);
  const s32 size = std::clamp<s32>(s32(std::lround(apparent_size)), 1, CameraLogic::MAX_POINT_SIZE);

  return CameraPoint({u16(x), u16(y)}, u8(size));
}

void WriteBasic(u8* out, const std::array<CameraPoint, CameraLogic::NUM_POINTS>& points)
{
  // Two objects per five byte group.
  for (size_t pair = 0; pair * 2 < points.size(); ++pair)
  {
    IRBasic packed = {};
    packed.SetObject1(points[pair * 2].position);
    packed.SetObject2(points[pair * 2 + 1].position);
    Common::BitCastPtr<IRBasic>(out + pair * sizeof(IRBasic)) = packed;
  }
}

void WriteExtended(u8* out, const std::array<CameraPoint, CameraLogic::NUM_POINTS>& points)
{
  for (size_t i = 0; i != points.size(); ++i)
  {
    const CameraPoint& point = points[i];
    if (point.position.x >= CameraLogic::CAMERA_RES_X)
      continue;

    IRExtended packed = {};
    packed.SetPosition(point.position);
    packed.size = point.size & 0xf;
    Common::BitCastPtr<IRExtended>(out + i * sizeof(IRExtended)) = packed;
  }
}

void WriteFull(u8* out, const std::array<CameraPoint, CameraLogic::NUM_POINTS>& points)
{
  for (size_t i = 0; i != points.size(); ++i)
  {
    const CameraPoint& point = points[i];
    if (point.position.x >= CameraLogic::CAMERA_RES_X)
      continue;

    const int x = point.position.x;
    const int y = point.position.y;
    const int radius = point.size;

    IRFull packed = {};
    packed.SetPosition(point.position);
    packed.size = point.size & 0xf;

    // The bounding box fields only keep their low 7 bits, and the intensity below is computed
    // from those stored values.
    packed.xmin = std::max(x - radius, 0) & 0x7f;
    packed.ymin = std::max(y - radius, 0) & 0x7f;
    packed.xmax = std::min(x + radius, CameraLogic::CAMERA_RES_X) & 0x7f;
    packed.ymax = std::min(y + radius, CameraLogic::CAMERA_RES_Y) & 0x7f;

    // Intensity: the pixel count of a disc filling the box, on the sensor's 128x96 grid
    // (8 subpixels per pixel in each direction).
    const int box_area = (packed.xmax - packed.xmin) * (packed.ymax - packed.ymin);
    const long intensity = std::lround(box_area / 8 / 8 * MathUtil::TAU / 8);
    packed.intensity = u8(std::min(0xffl, intensity));

    Common::BitCastPtr<IRFull>(out + i * sizeof(IRFull)) = packed;
  }
}
}  // namespace

void CameraLogic::Reset()
{
  m_reg_data = {};
  m_is_enabled = false;
}

void CameraLogic::DoState(PointerWrap& p)
{
  // m_is_enabled is restored by the remote from its status (IR bit).
  p.Do(m_reg_data);
}

void CameraLogic::SetEnabled(bool is_enabled)
{
  m_is_enabled = is_enabled;
}

int CameraLogic::BusRead(u8 slave_addr, u8 addr, int count, u8* data_out)
{
  if (slave_addr != I2C_ADDR || !m_is_enabled)
    return 0;
  return RawRead(&m_reg_data, addr, count, data_out);
}

int CameraLogic::BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in)
{
  if (slave_addr != I2C_ADDR || !m_is_enabled)
    return 0;
  return RawWrite(&m_reg_data, addr, count, data_in);
}

std::array<CameraPoint, CameraLogic::NUM_POINTS>
CameraLogic::GetCameraPoints(const Common::Matrix44& transform, Common::Vec2 field_of_view)
{
  // Perspective projection, a quarter turn so the camera looks along +Y, then the world.
  const Common::Matrix44 projection = Common::Matrix44::Perspective(
      field_of_view.y, field_of_view.x / field_of_view.y, 0.001f, 1000);
  const Common::Matrix44 look_forward =
      Common::Matrix44::FromMatrix33(Common::Matrix33::RotateX(float(MathUtil::TAU / 4)));
  const Common::Matrix44 camera_view = projection * look_forward * transform;

  constexpr float half_separation = SENSOR_BAR_LED_SEPARATION / 2;

  // Only the two LED groups are visible; the other two slots stay empty.
  std::array<CameraPoint, NUM_POINTS> points;
  points[0] = ProjectLed(camera_view, Common::Vec3{-half_separation, 0, 0});
  points[1] = ProjectLed(camera_view, Common::Vec3{half_separation, 0, 0});
  return points;
}

void CameraLogic::Update(const std::array<CameraPoint, NUM_POINTS>& camera_points)
{
  // Empty slots read as 0xff.
  m_reg_data.camera_data.fill(0xff);

  // The Wii writes 0x01 here while it changes the mode or sensitivity.
  if (m_reg_data.enable_object_tracking != TRACKING_ON)
    return;

  u8* const out = m_reg_data.camera_data.data();

  switch (m_reg_data.mode)
  {
  case IR_MODE_BASIC:
    WriteBasic(out, camera_points);
    break;
  case IR_MODE_EXTENDED:
    WriteExtended(out, camera_points);
    break;
  case IR_MODE_FULL:
    WriteFull(out, camera_points);
    break;
  default:
    // Games often enable IR reports before they set a mode; they get 0xff data meanwhile.
    break;
  }
}

}  // namespace WiimoteEmu
