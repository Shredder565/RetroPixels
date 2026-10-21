// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/CoreDevice.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/MathUtil.h"

namespace ciface::Core
{
namespace
{
// A press is detected once an input rises this far above its starting value. Release is below
// (1 - threshold); the detection logic relies on the threshold being above one half.
constexpr ControlState DETECTION_LEVEL = 0.55;
constexpr ControlState RELEASE_LEVEL = 1 - DETECTION_LEVEL;

// Pressed while either of two inputs is; e.g. "Shift" for "LSHIFT" and "RSHIFT".
class EitherInput final : public Device::Input
{
public:
  EitherInput(std::string label, Device::Input* a, Device::Input* b)
      : m_label(std::move(label)), m_a(a), m_b(b)
  {
  }

  std::string GetName() const override { return m_label; }

  ControlState GetState() const override
  {
    ControlState level = 0;
    if (m_a != nullptr)
      level = m_a->GetState();
    if (m_b != nullptr)
      level = std::max(level, m_b->GetState());
    return level;
  }

  bool IsDetectable() const override { return false; }

  bool IsChild(const Input* part) const override { return part == m_a || part == m_b; }

private:
  const std::string m_label;
  Device::Input* const m_a;
  Device::Input* const m_b;
};

// Treats a whole axis as one input running from 0.0 (fully `from`) to 1.0 (fully `to`), with
// 0.5 in the middle. Used where an axis may rest at one end (triggers) rather than the centre.
class WholeAxisInput final : public Device::Input
{
public:
  WholeAxisInput(Device::Input* from, Device::Input* to) : m_from(*from), m_to(*to) {}

  ControlState GetState() const override
  {
    return (1 + std::max(0.0, m_to.GetState()) - std::max(0.0, m_from.GetState())) / 2;
  }

  // "Full Axis X+" spans from "Axis X-" to "Axis X+".
  std::string GetName() const override { return "Full " + m_to.GetName(); }

  bool IsDetectable() const override { return m_from.IsDetectable() && m_to.IsDetectable(); }

  bool IsHidden() const override { return m_from.IsHidden() && m_to.IsHidden(); }

  bool IsMatchingName(std::string_view name) const override
  {
    if (Control::IsMatchingName(name))
      return true;

    // Configurations from before the "Full" prefix call this input "Axis X-+": the name of the
    // starting half followed by the sign of the end half.
    const std::string to_name = m_to.GetName();
    std::string legacy = m_from.GetName();
    legacy.push_back(to_name.back());
    return name == legacy;
  }

private:
  Device::Input& m_from;
  Device::Input& m_to;
};

// Reads the device id field of a qualifier string the way a formatted stream extraction does,
// keeping its platform's handling of malformed numbers.
int ParseQualifierId(const std::string& text)
{
  int id = -1;
  std::istringstream(text) >> id;
  return id;
}
}  // namespace

Device::~Device()
{
  for (Input* in : m_input_list)
    delete in;
  for (Output* out : m_output_list)
    delete out;
}

std::optional<int> Device::GetPreferredId() const
{
  return std::nullopt;
}

void Device::AddInput(Input* const i)
{
  m_input_list.push_back(i);
}

void Device::AddOutput(Output* const o)
{
  m_output_list.push_back(o);
}

std::string Device::GetQualifiedName() const
{
  return fmt::format("{}/{}/{}", GetSource(), GetId(), GetName());
}

Device::Input* Device::GetParentMostInput(Input* input) const
{
  // Climb while some input of this device combines the current one.
  Input* current = input;
  for (;;)
  {
    const auto parent = std::ranges::find_if(
        m_input_list, [current](const Input* candidate) { return candidate->IsChild(current); });
    if (parent == m_input_list.end())
      return current;
    current = *parent;
  }
}

Device::Input* Device::FindInput(std::string_view name) const
{
  const auto it = std::ranges::find_if(
      m_input_list, [name](const Input* candidate) { return candidate->IsMatchingName(name); });
  return it == m_input_list.end() ? nullptr : *it;
}

Device::Output* Device::FindOutput(std::string_view name) const
{
  const auto it = std::ranges::find_if(
      m_output_list, [name](const Output* candidate) { return candidate->IsMatchingName(name); });
  return it == m_output_list.end() ? nullptr : *it;
}

bool Device::Control::IsMatchingName(std::string_view name) const
{
  return name == GetName();
}

bool Device::Control::IsHidden() const
{
  return false;
}

void Device::AddFullAnalogSurfaceInputs(Input* low, Input* high)
{
  AddInput(low);
  AddInput(high);
  AddInput(new WholeAxisInput(low, high));
  AddInput(new WholeAxisInput(high, low));
}

void Device::AddCombinedInput(std::string name, const std::pair<std::string, std::string>& inputs)
{
  Input* const first = FindInput(inputs.first);
  Input* const second = FindInput(inputs.second);
  AddInput(new EitherInput(std::move(name), first, second));
}

std::string DeviceQualifier::ToString() const
{
  const bool has_id = cid >= 0;
  if (!has_id && source.empty() && name.empty())
    return {};

  if (!has_id)
    return fmt::format("{}//{}", source, name);
  return fmt::format("{}/{}/{}", source, cid, name);
}

// "Source/Id/Name". The pieces are split the way line-oriented stream reads would split them:
// the id is read from the second piece, the name runs to the end of the line, and with only two
// pieces the second one also serves as the name.
void DeviceQualifier::FromString(const std::string& str)
{
  *this = {};

  const size_t first_slash = str.find('/');
  if (first_slash == std::string::npos)
  {
    source = str;
    cid = ParseQualifierId({});
    return;
  }

  source = str.substr(0, first_slash);
  const std::string rest = str.substr(first_slash + 1);

  const size_t second_slash = rest.find('/');
  if (second_slash == std::string::npos)
  {
    name = rest;
    cid = ParseQualifierId(rest);
    return;
  }

  cid = ParseQualifierId(rest.substr(0, second_slash));
  const std::string tail = rest.substr(second_slash + 1);
  name = tail.substr(0, tail.find('\n'));
}

void DeviceQualifier::FromDevice(const Device* const dev)
{
  source = dev->GetSource();
  cid = dev->GetId();
  name = dev->GetName();
}

bool DeviceQualifier::operator==(const Device* const dev) const
{
  return cid == dev->GetId() && name == dev->GetName() && source == dev->GetSource();
}

bool DeviceQualifier::operator==(const DeviceQualifier& devq) const
{
  return cid == devq.cid && name == devq.name && source == devq.source;
}

std::shared_ptr<Device> DeviceContainer::FindDevice(const DeviceQualifier& devq) const
{
  std::lock_guard guard(m_devices_mutex);
  const auto it = std::ranges::find_if(
      m_devices, [&devq](const std::shared_ptr<Device>& dev) { return devq == dev.get(); });
  return it == m_devices.end() ? nullptr : *it;
}

std::vector<std::shared_ptr<Device>> DeviceContainer::GetAllDevices() const
{
  std::lock_guard guard(m_devices_mutex);
  return m_devices;
}

std::vector<std::string> DeviceContainer::GetAllDeviceStrings() const
{
  std::lock_guard guard(m_devices_mutex);

  std::vector<std::string> strings;
  strings.reserve(m_devices.size());
  for (const std::shared_ptr<Device>& dev : m_devices)
  {
    DeviceQualifier qualifier;
    qualifier.FromDevice(dev.get());
    strings.push_back(qualifier.ToString());
  }
  return strings;
}

bool DeviceContainer::HasDefaultDevice() const
{
  std::lock_guard guard(m_devices_mutex);
  // The list is kept sorted, so only the head can be a default device.
  return !m_devices.empty() && m_devices.front()->GetSortPriority() >= 0;
}

std::string DeviceContainer::GetDefaultDeviceString() const
{
  std::lock_guard guard(m_devices_mutex);
  if (m_devices.empty() || m_devices.front()->GetSortPriority() < 0)
    return "";

  DeviceQualifier qualifier;
  qualifier.FromDevice(m_devices.front().get());
  return qualifier.ToString();
}

Device::Input* DeviceContainer::FindInput(std::string_view name, const Device* def_dev) const
{
  // The given device wins; otherwise the first device that has such an input.
  if (def_dev != nullptr)
  {
    if (Device::Input* const found = def_dev->FindInput(name))
      return found;
  }

  std::lock_guard guard(m_devices_mutex);
  for (const std::shared_ptr<Device>& dev : m_devices)
  {
    if (Device::Input* const found = dev->FindInput(name))
      return found;
  }
  return nullptr;
}

Device::Output* DeviceContainer::FindOutput(std::string_view name, const Device* def_dev) const
{
  // Outputs are only ever looked up on the given device.
  return def_dev->FindOutput(name);
}

bool DeviceContainer::HasConnectedDevice(const DeviceQualifier& qualifier) const
{
  const std::shared_ptr<Device> dev = FindDevice(qualifier);
  return dev && dev->IsValid();
}

// The inputs being watched, grouped by device.
struct InputDetector::Watch
{
  // One watched input.
  struct Tracker
  {
    explicit Tracker(Device::Input* watched)
        : input(watched), start_level(watched->GetState()), previous_level(start_level)
    {
      // A zero step to begin with keeps the statistics meaningful for an instant press.
      steps.Push(0.0);
    }

    // Reads the input and records how far it moved since the previous read.
    void Sample()
    {
      const ControlState level = input->GetState();

      // Once a detected input is let go it may be detected again, from a fresh history.
      if (!armed && level < RELEASE_LEVEL)
      {
        previous_level = level;
        armed = true;
        steps.Clear();
      }

      steps.Push(level - previous_level);
      previous_level = level;
    }

    // Pressed means: armed, and risen well above where it started.
    bool IsPressedNow() const
    {
      return armed && previous_level - std::abs(start_level) > DETECTION_LEVEL;
    }

    // How evenly the input rose; 1 for a single jump, more for a gradual (analog) press.
    ControlState Smoothness() const { return 1 / std::sqrt(steps.Variance() / steps.Mean()); }

    Device::Input* input;
    ControlState start_level;
    ControlState previous_level;
    MathUtil::RunningVariance<ControlState> steps;
    bool armed = true;
  };

  struct WatchedDevice
  {
    std::shared_ptr<Device> device;
    std::vector<Tracker> trackers;
  };

  std::vector<WatchedDevice> devices;
};

InputDetector::InputDetector() : m_started_at{}, m_watch{}
{
}

InputDetector::~InputDetector() = default;

void InputDetector::Start(const DeviceContainer& container,
                          std::span<const std::string> device_strings)
{
  m_started_at = Clock::now();
  m_found = {};
  m_watch = std::make_unique<Watch>();

  for (const std::string& device_string : device_strings)
  {
    DeviceQualifier qualifier;
    qualifier.FromString(device_string);
    std::shared_ptr<Device> dev = container.FindDevice(qualifier);
    if (!dev)
      continue;

    // Absolute positions and motion sensors would trigger detection all by themselves.
    std::vector<Watch::Tracker> trackers;
    for (Device::Input* in : dev->Inputs())
    {
      if (in->IsDetectable())
        trackers.push_back(Watch::Tracker{in});
    }

    if (!trackers.empty())
      m_watch->devices.push_back(Watch::WatchedDevice{std::move(dev), std::move(trackers)});
  }

  // Nothing to watch: done straight away.
  if (m_watch->devices.empty())
    m_watch.reset();
}

void InputDetector::Update(std::chrono::milliseconds initial_wait,
                           std::chrono::milliseconds confirmation_wait,
                           std::chrono::milliseconds maximum_wait)
{
  if (!m_watch)
    return;

  const auto now = Clock::now();
  const auto running_for = now - m_started_at;

  // Stop on the overall limit, when nothing was pressed within the initial wait, or once the
  // last detection has been released for the confirmation wait.
  const bool out_of_time = running_for >= maximum_wait;
  const bool nothing_pressed = m_found.empty() && running_for >= initial_wait;
  const bool confirmed = !m_found.empty() && m_found.back().release_time.has_value() &&
                         now >= *m_found.back().release_time + confirmation_wait;
  if (out_of_time || nothing_pressed || confirmed)
  {
    m_watch.reset();
    return;
  }

  for (Watch::WatchedDevice& watched : m_watch->devices)
  {
    for (Watch::Tracker& tracker : watched.trackers)
    {
      tracker.Sample();
      if (!tracker.IsPressedNow())
        continue;

      tracker.armed = false;

      Detection found;
      found.device = watched.device;
      found.input = tracker.input;
      found.press_time = now;
      found.smoothness = tracker.Smoothness();
      m_found.push_back(std::move(found));
    }
  }

  // Note when detected inputs are let go.
  for (Detection& detection : m_found)
  {
    if (detection.release_time.has_value())
      continue;
    if (detection.input->GetState() < RELEASE_LEVEL)
      detection.release_time = Clock::now();
  }
}

bool InputDetector::IsComplete() const
{
  return m_watch == nullptr;
}

auto InputDetector::GetResults() const -> const Results&
{
  return m_found;
}

auto InputDetector::TakeResults() -> Results
{
  return std::move(m_found);
}

}  // namespace ciface::Core
