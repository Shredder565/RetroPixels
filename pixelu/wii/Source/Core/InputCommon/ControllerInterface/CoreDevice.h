// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

// The value type of every input and output.
typedef double ControlState;

namespace ciface
{
// Period of periodic force-feedback effects (100 Hz), in milliseconds.
constexpr int RUMBLE_PERIOD_MS = 10;

// Length given to force-feedback effects, in milliseconds. Effects are stopped explicitly, so this
// only has to outlast the longest rumble a game might request.
constexpr int RUMBLE_LENGTH_MS = 1000 * 10;

// Battery inputs report a percentage, so they map straight onto "Battery" settings. Every other
// input (except raw motion data) tops out at 1.0.
constexpr ControlState BATTERY_INPUT_MAX_VALUE = 100.0;

namespace Core
{
// What a device's UpdateInput() wants done with the device afterwards.
enum class DeviceRemoval
{
  Remove,
  Keep,
};

// A host input device: a named list of inputs and outputs.
class Device
{
public:
  class Input;
  class Output;

  // Something on a device that can be mapped: an input or an output.
  class Control
  {
  public:
    virtual ~Control() = default;
    virtual std::string GetName() const = 0;
    virtual Input* ToInput() { return nullptr; }
    virtual Output* ToOutput() { return nullptr; }

    // Whether `name` refers to this control. Overridden to keep accepting names used by older
    // configurations.
    virtual bool IsMatchingName(std::string_view name) const;

    // Hidden controls still work but are not listed in the UI.
    virtual bool IsHidden() const;
  };

  class Input : public Control
  {
  public:
    // False for inputs that should not be picked up by input detection (absolute positions,
    // motion sensors and the like).
    virtual bool IsDetectable() const { return true; }

    // The current value, normally 0.0 at rest and 1.0 when fully pressed. One input exists per
    // direction of an axis; the input of the opposite direction may return a negative value,
    // which helps input detection tell the two apart. Negative values never reach mappings.
    virtual ControlState GetState() const = 0;

    Input* ToInput() override { return this; }

    // True for an input that combines `input` with others (e.g. "Shift" for "LSHIFT").
    virtual bool IsChild(const Input*) const { return false; }
  };

  // An input that reports a change since the last update instead of an absolute value.
  class RelativeInput : public Input
  {
  public:
    bool IsDetectable() const override { return false; }
  };

  class Output : public Control
  {
  public:
    ~Output() override = default;
    virtual void SetState(ControlState state) = 0;
    Output* ToOutput() override { return this; }
  };

  virtual ~Device();

  int GetId() const { return m_id; }
  void SetId(int id) { m_id = id; }
  virtual std::string GetName() const = 0;
  virtual std::string GetSource() const = 0;
  // "Source/Id/Name".
  std::string GetQualifiedName() const;
  virtual DeviceRemoval UpdateInput() { return DeviceRemoval::Keep; }

  // False once the device is gone (for backends that detect that themselves).
  virtual bool IsValid() const { return true; }

  // True for devices that exist regardless of hardware (the system keyboard and mouse), which a
  // refresh does not need to recreate.
  virtual bool IsVirtualDevice() const { return false; }

  // An id this device would like to get (e.g. the player number lit on an XInput pad).
  virtual std::optional<int> GetPreferredId() const;

  // Devices are listed from the highest priority down; the first device of the list becomes the
  // default device of new mappings, provided its priority is not negative.
  static constexpr int DEFAULT_DEVICE_SORT_PRIORITY = std::numeric_limits<int>::max();
  virtual int GetSortPriority() const { return 0; }

  const std::vector<Input*>& Inputs() const { return m_input_list; }
  const std::vector<Output*>& Outputs() const { return m_output_list; }

  // The outermost input that combines `input` (or `input` itself).
  Input* GetParentMostInput(Input* input) const;

  Input* FindInput(std::string_view name) const;
  Output* FindOutput(std::string_view name) const;

protected:
  // The device takes ownership of the control.
  void AddInput(Input* const i);
  void AddOutput(Output* const o);

  // Adds the two half-axis inputs `low` and `high` plus a "Full ..." input for each direction,
  // for axes whose rest position is unknown (centred stick or trigger at one end).
  void AddFullAnalogSurfaceInputs(Input* low, Input* high);

  // Adds an input named `name` that is pressed while either of two existing inputs is.
  void AddCombinedInput(std::string name, const std::pair<std::string, std::string>& inputs);

private:
  int m_id = 0;
  std::vector<Input*> m_input_list;
  std::vector<Output*> m_output_list;
};

// Identifies a device by source, id and name; written as "Source/Id/Name" in configurations.
class DeviceQualifier
{
public:
  DeviceQualifier() : cid(-1) {}
  DeviceQualifier(std::string source_, const int id_, std::string name_)
      : source(std::move(source_)), cid(id_), name(std::move(name_))
  {
  }
  void FromDevice(const Device* const dev);
  void FromString(const std::string& str);
  std::string ToString() const;

  bool operator==(const DeviceQualifier& devq) const;

  bool operator==(const Device* dev) const;

  std::string source;
  int cid;
  std::string name;
};

// The list of devices, shared by everything that maps or reads inputs.
class DeviceContainer
{
public:
  // One input picked up by an InputDetector.
  struct InputDetection
  {
    std::shared_ptr<Device> device;
    Device::Input* input = nullptr;
    Clock::time_point press_time;
    std::optional<Clock::time_point> release_time;
    ControlState smoothness = 0;

    // Digital presses jump straight to 1.0 (smoothness 1); analog ones ramp up.
    bool IsAnalogPress() const { return smoothness > 1.00001; }
  };

  Device::Input* FindInput(std::string_view name, const Device* def_dev) const;
  Device::Output* FindOutput(std::string_view name, const Device* def_dev) const;

  std::vector<std::shared_ptr<Device>> GetAllDevices() const;
  std::vector<std::string> GetAllDeviceStrings() const;
  bool HasDefaultDevice() const;
  std::string GetDefaultDeviceString() const;
  std::shared_ptr<Device> FindDevice(const DeviceQualifier& devq) const;

  bool HasConnectedDevice(const DeviceQualifier& qualifier) const;

  std::recursive_mutex& GetDevicesMutex() const { return m_devices_mutex; }

protected:
  // Guards the m_devices vector itself (not the devices in it).
  mutable std::recursive_mutex m_devices_mutex;
  std::vector<std::shared_ptr<Device>> m_devices;
};

// Watches the inputs of some devices and reports the ones the user presses. An input only counts
// if it starts at rest, so stuck buttons and half-axes of centred sticks are ignored. Presses keep
// being collected until the timeouts given to Update() run out.
class InputDetector
{
public:
  using Detection = DeviceContainer::InputDetection;
  using Results = std::vector<Detection>;

  InputDetector();
  ~InputDetector();

  void Start(const DeviceContainer& container, std::span<const std::string> device_strings);
  void Update(std::chrono::milliseconds initial_wait, std::chrono::milliseconds confirmation_wait,
              std::chrono::milliseconds maximum_wait);
  bool IsComplete() const;

  const Results& GetResults() const;

  // Moves the results out.
  Results TakeResults();

private:
  struct Watch;

  Clock::time_point m_started_at;
  Results m_found;
  std::unique_ptr<Watch> m_watch;
};

}  // namespace Core
}  // namespace ciface
