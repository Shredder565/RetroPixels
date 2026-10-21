// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef HAS_LIBMGBA

#include "DolphinQt/GBAHost.h"

#include <QApplication>

#include "Core/HW/GBACore.h"
#include "DolphinQt/GBAWidget.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"

GBAHost::GBAHost(std::weak_ptr<HW::GBA::Core> core) : m_core(std::move(core))
{
  // Made here but handed to the GUI thread, which creates the window.
  m_controller = new GBAWidgetController();
  m_controller->moveToThread(qApp->thread());

  const HW::GBA::CoreInfo info = m_core.lock()->GetCoreInfo();
  QueueOnObject(m_controller, [controller = m_controller, core = m_core, info] {
    controller->Create(core, info);
  });
}

GBAHost::~GBAHost()
{
  m_controller->deleteLater();
}

void GBAHost::GameChanged()
{
  const auto core = m_core.lock();
  if (!core || !core->IsStarted())
    return;

  const HW::GBA::CoreInfo info = core->GetCoreInfo();
  QueueOnObject(m_controller,
                [controller = m_controller, info] { controller->GameChanged(info); });
}

void GBAHost::FrameEnded(std::span<const u32> video_buffer)
{
  QueueOnObject(m_controller, [controller = m_controller, video_buffer] {
    controller->FrameEnded(video_buffer);
  });
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return std::make_unique<GBAHost>(std::move(core));
}
#endif  // HAS_LIBMGBA
