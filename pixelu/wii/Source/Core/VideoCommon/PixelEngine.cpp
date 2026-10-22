// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/PixelEngine.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include "Common/BitField.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/System.h"

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/VideoBackendBase.h"

namespace PixelEngine
{
namespace
{
// Processor interface interrupt lines owned by the PE.
constexpr u32 PI_LINE_TOKEN = 0x200;
constexpr u32 PI_LINE_FINISH = 0x400;

// In single-core (and deterministic dual-core) mode the emulated GPU finishes far sooner than
// real hardware would; delivering the token/finish interrupt at least this many CPU cycles later
// gives games the time they expect to arm their handlers.
constexpr int MIN_SINGLE_CORE_DELAY = 500;

struct PerfCounterRegs
{
  u32 low_offset;
  PerfQueryType query;
};

constexpr PerfCounterRegs PERF_COUNTERS[] = {
    {PE_PERF_ZCOMP_INPUT_ZCOMPLOC_L, PQ_ZCOMP_INPUT_ZCOMPLOC},
    {PE_PERF_ZCOMP_OUTPUT_ZCOMPLOC_L, PQ_ZCOMP_OUTPUT_ZCOMPLOC},
    {PE_PERF_ZCOMP_INPUT_L, PQ_ZCOMP_INPUT},
    {PE_PERF_ZCOMP_OUTPUT_L, PQ_ZCOMP_OUTPUT},
    {PE_PERF_BLEND_INPUT_L, PQ_BLEND_INPUT},
    {PE_PERF_EFB_COPY_CLOCKS_L, PQ_EFB_COPY_CLOCKS},
};
}  // namespace

PixelEngineManager::PixelEngineManager(Core::System& system) : m_system{system}
{
}

void PixelEngineManager::DoState(PointerWrap& p)
{
  // Savestate order (fixed): the registers, then the token/interrupt delivery state.
  const auto save_in_order = [&p](auto&... fields) { (p.Do(fields), ...); };
  save_in_order(m_z_conf, m_alpha_conf, m_dst_alpha_conf, m_alpha_mode_conf, m_alpha_read,
                m_control);
  save_in_order(m_token, m_queued_token, m_queued_token_irq, m_queued_finish_irq,
                m_delivery_scheduled, m_token_irq_asserted, m_finish_irq_asserted);
}

void PixelEngineManager::Init()
{
  for (u16* reg : {&m_control.hex, &m_z_conf.hex, &m_alpha_conf.hex, &m_dst_alpha_conf.hex,
                   &m_alpha_mode_conf.hex, &m_alpha_read.hex})
  {
    *reg = 0;
  }

  m_token = 0;
  m_queued_token = 0;
  m_queued_token_irq = false;
  m_queued_finish_irq = false;
  m_delivery_scheduled = false;
  m_token_irq_asserted = false;
  m_finish_irq_asserted = false;

  m_delivery_event = m_system.GetCoreTiming().RegisterEvent("SetTokenFinish", OnTokenFinishEvent);
}

void PixelEngineManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // EFB access configuration: plain storage, readable and writable.
  const std::pair<u32, u16*> plain_regs[] = {
      {PE_ZCONF, &m_z_conf.hex},
      {PE_ALPHACONF, &m_alpha_conf.hex},
      {PE_DSTALPHACONF, &m_dst_alpha_conf.hex},
      {PE_ALPHAMODE, &m_alpha_mode_conf.hex},
      {PE_ALPHAREAD, &m_alpha_read.hex},
  };
  for (const auto& [offset, storage] : plain_regs)
    mmio->Register(base | offset, MMIO::DirectRead<u16>(storage), MMIO::DirectWrite<u16>(storage));

  // Performance counters come from the backend's queries; each 32-bit count is split over a
  // low and a high register.
  for (const PerfCounterRegs& counter : PERF_COUNTERS)
  {
    const PerfQueryType query = counter.query;
    mmio->Register(base | counter.low_offset,
                   MMIO::ComplexRead<u16>([query](Core::System&, u32) {
                     return static_cast<u16>(g_video_backend->Video_GetQueryResult(query) & 0xFFFF);
                   }),
                   MMIO::InvalidWrite<u16>());
    mmio->Register(base | (counter.low_offset + 2),
                   MMIO::ComplexRead<u16>([query](Core::System&, u32) {
                     return static_cast<u16>(g_video_backend->Video_GetQueryResult(query) >> 16);
                   }),
                   MMIO::InvalidWrite<u16>());
  }

  mmio->Register(base | PE_CTRL_REGISTER, MMIO::DirectRead<u16>(&m_control.hex),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   PixelEngineManager& pe = system.GetPixelEngine();
                   const UPECtrlReg written{.hex = val};

                   // Acknowledge bits drop the corresponding asserted interrupt.
                   if (written.pe_token)
                     pe.m_token_irq_asserted = false;
                   if (written.pe_finish)
                     pe.m_finish_irq_asserted = false;

                   // Only the enables are stored; the acknowledge bits are write-only.
                   UPECtrlReg stored{.hex = pe.m_control.hex};
                   stored.pe_token_enable = written.pe_token_enable.Value();
                   stored.pe_finish_enable = written.pe_finish_enable.Value();
                   stored.pe_token = false;
                   stored.pe_finish = false;
                   pe.m_control.hex = stored.hex;

                   DEBUG_LOG_FMT(PIXELENGINE, "(w16) CTRL_REGISTER: {:#06x}", val);
                   pe.UpdateInterrupts();
                 }));

  mmio->Register(base | PE_TOKEN_REG, MMIO::DirectRead<u16>(&m_token), MMIO::InvalidWrite<u16>());

  // Reading any bounding box register tells us the game uses the feature, so the pixel shaders
  // stop tracking the box from then on.
  for (int side = 0; side < 4; ++side)
  {
    const u32 offset = PE_BBOX_LEFT + 2 * static_cast<u32>(side);
    mmio->Register(base | offset, MMIO::ComplexRead<u16>([side](Core::System& system, u32) {
                     g_bounding_box->Disable(system.GetPixelShaderManager());
                     return g_video_backend->Video_GetBoundingBox(side);
                   }),
                   MMIO::InvalidWrite<u16>());
  }
}

void PixelEngineManager::UpdateInterrupts()
{
  ProcessorInterface::ProcessorInterfaceManager& pi = m_system.GetProcessorInterface();
  pi.SetInterrupt(PI_LINE_TOKEN, m_token_irq_asserted && m_control.pe_token_enable);
  pi.SetInterrupt(PI_LINE_FINISH, m_finish_irq_asserted && m_control.pe_finish_enable);
}

void PixelEngineManager::OnTokenFinishEvent(Core::System& system, u64, s64)
{
  system.GetPixelEngine().DeliverPending();
}

// CPU thread: publish whatever the GPU queued since the event was scheduled.
void PixelEngineManager::DeliverPending()
{
  std::unique_lock guard(m_handover_lock);
  m_delivery_scheduled = false;
  m_token = m_queued_token;

  if (m_queued_token_irq)
  {
    m_queued_token_irq = false;
    m_token_irq_asserted = true;
    UpdateInterrupts();
  }

  if (!m_queued_finish_irq)
    return;

  m_queued_finish_irq = false;
  m_finish_irq_asserted = true;
  UpdateInterrupts();
  // A finished frame drives frame-advance and movie bookkeeping; do that outside the lock.
  guard.unlock();
  Core::FrameUpdateOnCPUThread();
}

// GPU thread, with m_handover_lock held: make sure a delivery event is on its way.
void PixelEngineManager::ScheduleDelivery(int cycles_into_future)
{
  if (m_delivery_scheduled)
    return;
  m_delivery_scheduled = true;

  const bool timed = !m_system.IsDualCoreMode() || m_system.GetFifo().UseDeterministicGPUThread();
  // With a free-running GPU thread the delay is meaningless, so deliver as soon as possible.
  const s64 delay = timed ? std::max(MIN_SINGLE_CORE_DELAY, cycles_into_future) : 0;
  const CoreTiming::FromThread origin =
      timed ? CoreTiming::FromThread::CPU : CoreTiming::FromThread::NON_CPU;
  m_system.GetCoreTiming().ScheduleEvent(delay, m_delivery_event, 0, origin);
}

void PixelEngineManager::SetToken(const u16 token, const bool interrupt, int cycles_into_future)
{
  DEBUG_LOG_FMT(PIXELENGINE, "VIDEO Backend raises INT_CAUSE_PE_TOKEN (btw, token: {:04x})", token);

  std::lock_guard guard(m_handover_lock);
  m_queued_token = token;
  m_queued_token_irq = m_queued_token_irq || interrupt;
  ScheduleDelivery(cycles_into_future);
}

void PixelEngineManager::SetFinish(int cycles_into_future)
{
  DEBUG_LOG_FMT(PIXELENGINE, "VIDEO Set Finish");

  std::lock_guard guard(m_handover_lock);
  m_queued_finish_irq = true;
  ScheduleDelivery(cycles_into_future);
}

}  // namespace PixelEngine
