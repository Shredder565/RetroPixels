// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/MappingCommon.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <QObject>
#include <QTimer>

#include "DolphinQt/Config/Mapping/MappingButton.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/QtUtils/BlockUserInputFilter.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/InputConfig.h"

namespace MappingCommon
{
namespace
{
using Detector = ciface::Core::InputDetector;

// Give up if nothing is pressed within this time.
constexpr auto NOTHING_PRESSED_TIMEOUT = std::chrono::seconds(3);
// With "wait for alternate mappings", how long to keep listening after the first press.
constexpr auto ALTERNATES_WINDOW = std::chrono::milliseconds(750);
// Hard limit for one detection.
constexpr auto DETECTION_LIMIT = std::chrono::seconds(5);
// When a click queues another button mid-detection, presses this recent are that click.
constexpr auto QUEUEING_CLICK_TIME = std::chrono::milliseconds(50);

// The queue of mapping buttons waiting for input detection in one MappingWindow. The front
// button is the one being detected; detection starts a moment after it reaches the front.
//
// Note: no Q_OBJECT, so tr() uses the "QObject" context (as existing translations expect).
class DetectionQueue final : public QObject
{
public:
  explicit DetectionQueue(MappingWindow* window) : QObject{window}, m_window{window}
  {
    connect(window, &MappingWindow::Update, this, &DetectionQueue::Poll);
    connect(window, &MappingWindow::ConfigChanged, this, &DetectionQueue::Clear);
    connect(window, &MappingWindow::UnQueueInputDetection, this, &DetectionQueue::Remove);
    connect(window, &MappingWindow::QueueInputDetection, this, &DetectionQueue::Add);
    connect(window, &MappingWindow::CancelMapping, this, &DetectionQueue::Clear);

    m_start_delay = new QTimer(this);
    m_start_delay->setSingleShot(true);
    connect(m_start_delay, &QTimer::timeout, this, &DetectionQueue::BeginListening);
  }

private:
  ControllerEmu::EmulatedController* Controller() const { return m_window->GetController(); }

  void BeginListening()
  {
    const auto& own_device = Controller()->GetDefaultDevice();
    MappingButton* const button = m_queue.front();

    button->setFocus();
    button->setText(tr("[ Press Now ]"));
    QtUtils::InstallKeyboardBlocker(button, button, &MappingButton::ConfigChanged);

    std::vector<std::string> devices;
    if (m_window->IsCreateOtherDeviceMappingsEnabled())
      devices = g_controller_interface.GetAllDeviceStrings();
    else
      devices.push_back(own_device.ToString());

    m_detector = std::make_unique<Detector>();
    const auto state_lock = Controller()->GetStateLock();
    m_detector->Start(g_controller_interface, devices);
  }

  // Runs on every indicator tick of the window.
  void Poll()
  {
    if (m_detector == nullptr)
      return;

    const bool wait_for_alternates = m_window->IsWaitForAlternateMappingsEnabled();
    m_detector->Update(NOTHING_PRESSED_TIMEOUT,
                       wait_for_alternates ? ALTERNATES_WINDOW : ALTERNATES_WINDOW * 0,
                       DETECTION_LIMIT);
    if (!m_detector->IsComplete())
      return;

    MappingButton* const finished = m_queue.front();
    Detector::Results detected = m_detector->TakeResults();

    if (!Apply(&detected))
    {
      // Nothing usable was pressed: drop this and every other queued detection.
      Clear();
      return;
    }

    // Iterative mapping moves on to the next button, unless the user queued more already.
    if (!m_window->IsIterativeMappingEnabled() || !m_queue.empty())
      return;

    emit finished->QueueNextButtonMapping();
    if (m_queue.empty())
      return;

    // An analog press (a stick or trigger) makes no sense as a modifier: clear the modifier
    // button that came next and skip ahead.
    MappingButton* const next = m_queue.front();
    const bool any_analog = std::ranges::any_of(detected, &Detector::Detection::IsAnalogPress);
    if (next->GetControlType() == MappingButton::ControlType::ModifierInput && any_analog)
    {
      StoreExpression(next, "");
      Remove(next);
      emit next->QueueNextButtonMapping();
    }
  }

  // Turns the detected inputs into the front button's expression. False if they are not a
  // complete detection.
  bool Apply(Detector::Results* detected)
  {
    if (!ciface::MappingCommon::ContainsCompleteDetection(*detected))
      return false;

    ciface::MappingCommon::RemoveSpuriousTriggerCombinations(detected);

    const auto& own_device = Controller()->GetDefaultDevice();
    MappingButton* const button = m_queue.front();
    StoreExpression(button, ciface::MappingCommon::BuildExpression(
                                *detected, own_device, ciface::MappingCommon::Quote::On));
    Remove(button);
    return true;
  }

  void StoreExpression(MappingButton* button, const std::string& expression)
  {
    ControlReference* const reference = button->GetControlReference();
    reference->SetExpression(expression);
    emit m_window->Save();

    ControllerEmu::EmulatedController* const controller = Controller();
    controller->UpdateSingleControlReference(g_controller_interface, reference);
    controller->GetConfig()->GenerateControllerTextures();
  }

  // Any change to the queue drops the running detection; it restarts (after the delay) for
  // whichever button is now at the front.
  void Restart()
  {
    m_detector.reset();
    if (m_queue.empty())
      m_start_delay->stop();
    else
      m_start_delay->start(INPUT_DETECT_INITIAL_DELAY);
  }

  // Takes `button` out of the queue (it shows its mapping again either way). Returns whether
  // it was queued.
  bool Remove(MappingButton* button)
  {
    emit button->ConfigChanged();

    const auto position = std::ranges::find(m_queue, button);
    if (position == m_queue.end())
      return false;
    m_queue.erase(position);

    Restart();
    return true;
  }

  // Clicking a queued button again takes it out of the queue instead.
  void Add(MappingButton* button)
  {
    if (Remove(button))
      return;

    button->setText(QStringLiteral("[ ... ]"));
    m_queue.push_back(button);

    // If a detection was running, the click that queued this button ends it: keep what was
    // pressed before the click and map it.
    if (m_detector != nullptr)
    {
      Detector::Results detected = m_detector->TakeResults();
      ciface::MappingCommon::RemoveDetectionsAfterTimePoint(&detected,
                                                            Clock::now() - QUEUEING_CLICK_TIME);
      Apply(&detected);
    }

    Restart();
  }

  void Clear()
  {
    // The buttons go back to showing their expressions.
    for (MappingButton* const button : m_queue)
      emit button->ConfigChanged();
    m_queue.clear();

    Restart();
  }

  MappingWindow* const m_window;
  QTimer* m_start_delay = nullptr;
  std::deque<MappingButton*> m_queue;
  std::unique_ptr<Detector> m_detector;
};
}  // namespace

void CreateMappingProcessor(MappingWindow* window)
{
  new DetectionQueue{window};
}
}  // namespace MappingCommon
