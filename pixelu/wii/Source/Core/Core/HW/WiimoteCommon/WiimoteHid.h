// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <type_traits>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"

namespace WiimoteEmu
{
struct DesiredWiimoteState;
}

namespace WiimoteCommon
{
// Bluetooth HID transaction header values (Bluetooth HID profile 1.0). The upper nibble of the
// header byte is the transaction type, the lower nibble its parameter.
constexpr u8 HID_TYPE_HANDSHAKE = 0;
constexpr u8 HID_TYPE_SET_REPORT = 5;
constexpr u8 HID_TYPE_DATA = 0xA;

constexpr u8 HID_HANDSHAKE_SUCCESS = 0;

constexpr u8 HID_PARAM_INPUT = 1;
constexpr u8 HID_PARAM_OUTPUT = 2;

// A Wii Remote as seen by the emulated Bluetooth stack: either the emulated remote or a real one.
class HIDWiimote
{
public:
  using InterruptCallbackType = std::function<void(u8 hid_type, const u8* data, u32 size)>;

  enum class SensorBarState : bool
  {
    Disabled,
    Enabled
  };

  virtual ~HIDWiimote() = default;

  // The HID channels to the Wii were opened / closed.
  virtual void EventLinked() = 0;
  virtual void EventUnlinked() = 0;

  // Bluetooth slot the remote is attached to.
  virtual u8 GetWiimoteDeviceIndex() const = 0;
  virtual void SetWiimoteDeviceIndex(u8 index) = 0;

  // Called at the remote's report rate (about 200 Hz) while the channels are open. PrepareInput
  // gathers the user's input; Update then turns it into reports.
  virtual void PrepareInput(WiimoteEmu::DesiredWiimoteState* target_state,
                            SensorBarState sensor_bar_state) = 0;
  virtual void Update(const WiimoteEmu::DesiredWiimoteState& target_state) = 0;

  // Payload of a DATA/output (0xa2) packet on the interrupt channel, without the header byte.
  virtual void InterruptDataOutput(const u8* data, u32 size) = 0;

  // Buttons currently held. Only the button bits are meaningful. Used to let a disconnected
  // remote ask for a reconnection.
  virtual ButtonData GetCurrentlyPressedButtons() = 0;

  void SetInterruptCallback(InterruptCallbackType callback) { m_callback = std::move(callback); }

protected:
  // Sends an input report (header byte DATA/input).
  void InterruptDataInputCallback(const u8* data, u32 size)
  {
    constexpr u8 data_input_header = (HID_TYPE_DATA << 4) | HID_PARAM_INPUT;
    InterruptCallback(data_input_header, data, size);
  }

  void InterruptCallback(u8 hid_type, const u8* data, u32 size) { m_callback(hid_type, data, size); }

private:
  InterruptCallbackType m_callback;
};

#pragma pack(push, 1)

// An input report: its ID byte followed by the report body.
template <typename T>
struct TypedInputData
{
  static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>);

  TypedInputData(InputReportID id) : report_id(id) {}

  u8* GetData() { return reinterpret_cast<u8*>(this); }
  const u8* GetData() const { return reinterpret_cast<const u8*>(this); }
  constexpr u32 GetSize() const { return sizeof(*this); }

  InputReportID report_id;
  T payload = {};
};

#pragma pack(pop)

}  // namespace WiimoteCommon
