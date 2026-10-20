// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"

#include <cstddef>
#include <optional>
#include <type_traits>
#include <variant>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/VariantUtil.h"

#include "Core/HW/WiimoteEmu/MotionPlus.h"

// Layout of a serialized state:
//
//   header   bit 0 buttons, bit 1 acceleration, bit 2 camera, bit 3 MotionPlus,
//            bits 4-7 the extension's variant index (0 = none)
//   buttons  two bytes: A B + - 1 2 Home in bits 0-6, then Up Down Left Right in bits 0-3
//   accel    the low bits nibble (x bits 0-1, y bit 1, z bit 1) in the high half of a byte (the
//            d-pad byte when buttons are present, else a byte of its own), then the top eight
//            bits of x, y and z
//   camera   four points in the IR "extended" format
//   M+       per axis (pitch, roll, yaw): low eight bits, then bits 8-13 with the "slow" flag in
//            bit 7
//   ext      the raw bytes of the extension's desired state
//
// Absent parts take no space, so an idle remote without extension is one byte.

namespace WiimoteEmu
{
namespace
{
using ExtensionData = DesiredExtensionState::ExtensionData;

// The extension index has four bits.
static_assert(std::variant_size_v<ExtensionData> <= 16);

enum HeaderBit : u8
{
  HAS_BUTTONS = 1 << 0,
  HAS_ACCEL = 1 << 1,
  HAS_CAMERA = 1 << 2,
  HAS_MOTION_PLUS = 1 << 3,
};
constexpr int EXTENSION_SHIFT = 4;

constexpr std::size_t BUTTON_BYTES = 2;
constexpr std::size_t ACCEL_BYTES = 4;
constexpr std::size_t CAMERA_BYTES = 4 * sizeof(IRExtended);
constexpr std::size_t MOTION_PLUS_BYTES = 6;

u8 Bit(bool value, int position)
{
  return static_cast<u8>(u8(value) << position);
}

bool BitAt(u8 value, int position)
{
  return ((value >> position) & 1) != 0;
}

std::size_t ExtensionSize(std::size_t index)
{
  std::size_t size = 0;
  WithVariantAlternative<ExtensionData>(index, [&]<typename T>() { size = sizeof(T); });
  return size;
}

class Writer
{
public:
  explicit Writer(SerializedWiimoteState* out) : m_out(*out) { m_out.length = 0; }

  void Put(u8 value) { m_out.data[m_out.length++] = value; }
  void OrIntoLast(u8 value) { m_out.data[m_out.length - 1u] |= value; }

  template <typename T>
  void PutRaw(const T& value)
  {
    Common::BitCastPtr<T>(&m_out.data[m_out.length]) = value;
    m_out.length = static_cast<u8>(m_out.length + sizeof(T));
  }

private:
  SerializedWiimoteState& m_out;
};
}  // namespace

SerializedWiimoteState SerializeDesiredState(const DesiredWiimoteState& state)
{
  const bool buttons = (state.buttons.hex & WiimoteCommon::ButtonData::BUTTON_MASK) != 0;
  const bool accel = state.acceleration != DesiredWiimoteState::DEFAULT_ACCELERATION;
  const bool camera = state.camera_points != DesiredWiimoteState::DEFAULT_CAMERA;
  const bool motion_plus = state.motion_plus.has_value();
  const auto extension = static_cast<u8>(state.extension.data.index());

  SerializedWiimoteState result{};
  Writer out(&result);

  u8 header = static_cast<u8>(extension << EXTENSION_SHIFT);
  if (buttons)
    header |= HAS_BUTTONS;
  if (accel)
    header |= HAS_ACCEL;
  if (camera)
    header |= HAS_CAMERA;
  if (motion_plus)
    header |= HAS_MOTION_PLUS;
  out.Put(header);

  if (buttons)
  {
    const auto& b = state.buttons;
    out.Put(static_cast<u8>(Bit(b.a, 0) | Bit(b.b, 1) | Bit(b.plus, 2) | Bit(b.minus, 3) |
                            Bit(b.one, 4) | Bit(b.two, 5) | Bit(b.home, 6)));
    out.Put(static_cast<u8>(Bit(b.up, 0) | Bit(b.down, 1) | Bit(b.left, 2) | Bit(b.right, 3)));
  }

  if (accel)
  {
    // Y and Z lose their lowest bit.
    const auto& a = state.acceleration.value;
    const u8 low_bits =
        static_cast<u8>((a.x & 0b11) | Bit(BitAt(u8(a.y), 1), 2) | Bit(BitAt(u8(a.z), 1), 3));
    const auto low_nibble = static_cast<u8>(low_bits << 4);

    // The d-pad byte has a free upper half.
    if (buttons)
      out.OrIntoLast(low_nibble);
    else
      out.Put(low_nibble);

    out.Put(static_cast<u8>(a.x >> 2));
    out.Put(static_cast<u8>(a.y >> 2));
    out.Put(static_cast<u8>(a.z >> 2));
  }

  if (camera)
  {
    for (const CameraPoint& point : state.camera_points)
    {
      IRExtended packed{};
      packed.SetPosition(point.position);
      packed.size = point.size & 0xf;
      out.PutRaw(packed);
    }
  }

  if (motion_plus)
  {
    const auto& gyro = state.motion_plus->gyro.value;
    const auto& slow = state.motion_plus->is_slow;
    const auto put_axis = [&out](u16 value, bool is_slow) {
      out.Put(static_cast<u8>(value));
      out.Put(static_cast<u8>(((value >> 8) & 0x3f) | Bit(is_slow, 7)));
    };
    put_axis(gyro.x, slow.x);
    put_axis(gyro.y, slow.y);
    put_axis(gyro.z, slow.z);
  }

  if (extension != 0)
  {
    std::visit(
        [&out](const auto& ext) {
          static_assert(sizeof(ext) <= 6);
          out.PutRaw(ext);
        },
        state.extension.data);
  }

  return result;
}

bool DeserializeDesiredState(DesiredWiimoteState* state, const SerializedWiimoteState& serialized)
{
  // Start from the idle state; it is also what a rejected input leaves behind.
  state->buttons.hex = 0;
  state->acceleration = DesiredWiimoteState::DEFAULT_ACCELERATION;
  state->camera_points = DesiredWiimoteState::DEFAULT_CAMERA;
  state->motion_plus = std::nullopt;
  state->extension.data = std::monostate();

  if (serialized.length == 0)
    return false;

  const auto& d = serialized.data;
  const u8 header = d[0];
  const bool buttons = (header & HAS_BUTTONS) != 0;
  const bool accel = (header & HAS_ACCEL) != 0;
  const bool camera = (header & HAS_CAMERA) != 0;
  const bool motion_plus = (header & HAS_MOTION_PLUS) != 0;
  const auto extension = static_cast<u8>(header >> EXTENSION_SHIFT);

  if (extension >= ExtensionNumber::MAX)
    return false;

  // Buttons and acceleration share one byte when both are present.
  std::size_t expected = 1;
  if (buttons)
    expected += BUTTON_BYTES;
  if (accel)
    expected += buttons ? ACCEL_BYTES - 1 : ACCEL_BYTES;
  if (camera)
    expected += CAMERA_BYTES;
  if (motion_plus)
    expected += MOTION_PLUS_BYTES;
  if (extension != 0)
    expected += ExtensionSize(extension);

  if (serialized.length != expected)
    return false;

  std::size_t pos = 1;

  if (buttons)
  {
    const u8 main = d[pos];
    const u8 dpad = d[pos + 1];
    auto& b = state->buttons;
    b.a = BitAt(main, 0);
    b.b = BitAt(main, 1);
    b.plus = BitAt(main, 2);
    b.minus = BitAt(main, 3);
    b.one = BitAt(main, 4);
    b.two = BitAt(main, 5);
    b.home = BitAt(main, 6);
    b.up = BitAt(dpad, 0);
    b.down = BitAt(dpad, 1);
    b.left = BitAt(dpad, 2);
    b.right = BitAt(dpad, 3);
    pos += BUTTON_BYTES;
  }

  if (accel)
  {
    // The low bits live in the d-pad byte when buttons were sent.
    const std::size_t low_pos = buttons ? pos - 1 : pos;
    const std::size_t high_pos = buttons ? pos : pos + 1;
    const auto low_bits = static_cast<u8>(d[low_pos] >> 4);

    auto& a = state->acceleration.value;
    a.x = static_cast<u16>((d[high_pos] << 2) | (low_bits & 0b11));
    // Y and Z get their dropped bit back by repeating the bit above it.
    a.y =
        Common::ExpandValue<u16>(static_cast<u16>((d[high_pos + 1] << 1) | BitAt(low_bits, 2)), 1);
    a.z =
        Common::ExpandValue<u16>(static_cast<u16>((d[high_pos + 2] << 1) | BitAt(low_bits, 3)), 1);
    pos = high_pos + 3;
  }

  if (camera)
  {
    for (CameraPoint& point : state->camera_points)
    {
      const IRExtended packed = Common::BitCastPtr<IRExtended>(&d[pos]);
      point = CameraPoint(packed.GetPosition(), packed.size);
      pos += sizeof(IRExtended);
    }
  }

  if (motion_plus)
  {
    const auto axis_value = [&d](std::size_t at) {
      return static_cast<u16>(d[at] | ((d[at + 1] & 0x3f) << 8));
    };
    const auto axis_slow = [&d](std::size_t at) { return (d[at + 1] & 0x80) != 0; };

    using Format = MotionPlus::DataFormat;
    state->motion_plus =
        Format::Data{Format::GyroRawValue{Format::GyroType(axis_value(pos), axis_value(pos + 2),
                                                           axis_value(pos + 4))},
                     Format::SlowType(axis_slow(pos), axis_slow(pos + 2), axis_slow(pos + 4))};
    pos += MOTION_PLUS_BYTES;
  }

  if (extension != 0)
  {
    WithVariantAlternative<ExtensionData>(extension, [&]<typename T>() {
      state->extension.data.emplace<T>(Common::BitCastPtr<T>(&d[pos]));
    });
  }

  return true;
}
}  // namespace WiimoteEmu
