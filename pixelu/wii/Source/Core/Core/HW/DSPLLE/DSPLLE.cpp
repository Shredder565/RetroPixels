// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPLLE/DSPLLE.h"

#include <mutex>
#include <string>
#include <thread>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/DSP/DSPCaptureLogger.h"
#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPHost.h"
#include "Core/DSP/DSPTables.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"
#include "Core/DSP/Jit/DSPEmitterBase.h"

namespace DSP::LLE
{
namespace
{
// The DSP runs at a sixth of the CPU clock.
constexpr int CPU_CYCLES_PER_DSP_CYCLE = 6;

constexpr u32 MAIL_PENDING = 0x80000000;

// Reads a big-endian ROM image of exactly size_in_bytes bytes.
bool LoadDSPRom(u16* rom, const std::string& filename, u32 size_in_bytes)
{
  std::string bytes;
  if (!File::ReadFileToString(filename, bytes))
    return false;

  if (bytes.size() != size_in_bytes)
  {
    ERROR_LOG_FMT(DSPLLE, "{} has a wrong size ({}, expected {})", filename, bytes.size(),
                  size_in_bytes);
    return false;
  }

  const auto* data = reinterpret_cast<const u8*>(bytes.data());
  for (u32 word = 0; word < size_in_bytes / 2; ++word)
    rom[word] = Common::swap16(data + word * 2);
  return true;
}

// The user's GC folder takes precedence over the copies shipped in Sys.
std::string FindRomFile(const char* name)
{
  const std::string user_copy = File::GetUserPath(D_GCUSER_IDX) + name;
  if (File::Exists(user_copy))
    return user_copy;
  return File::GetSysDirectory() + GC_SYS_DIR DIR_SEP + name;
}

bool FillDSPInitOptions(DSPInitOptions* opts)
{
  if (!LoadDSPRom(opts->irom_contents.data(), FindRomFile(DSP_IROM), DSP_IROM_BYTE_SIZE))
    return false;
  if (!LoadDSPRom(opts->coef_contents.data(), FindRomFile(DSP_COEF), DSP_COEF_BYTE_SIZE))
    return false;

  opts->core_type = DSPInitOptions::CoreType::Interpreter;
#ifdef _M_X86_64
  if (Config::Get(Config::MAIN_DSP_JIT))
    opts->core_type = DSPInitOptions::CoreType::JIT64;
#endif

  if (Config::Get(Config::MAIN_DSP_CAPTURE_LOG))
  {
    // Replaces (and frees) the default do-nothing logger.
    delete opts->capture_logger;
    opts->capture_logger =
        new PCAPDSPCaptureLogger(File::GetUserPath(D_DUMPDSP_IDX) + "dsp.pcap");
  }
  return true;
}
}  // namespace

DSPLLE::DSPLLE() = default;

DSPLLE::~DSPLLE()
{
  m_dsp_core.Shutdown();
  DSP_StopSoundStream();
}

void DSPLLE::DoState(PointerWrap& p)
{
  // The state starts with a flag telling the two DSP engines apart.
  bool is_hle = false;
  p.Do(is_hle);
  if (is_hle && p.IsReadMode())
  {
    Core::DisplayMessage("State is incompatible with current DSP engine. Aborting load state.",
                         3000);
    p.SetVerifyMode();
    return;
  }
  m_dsp_core.DoState(p);
  p.Do(m_pending_cycles);
}

void DSPLLE::ThreadLoop()
{
  Common::SetCurrentThreadName("DSP thread");

  while (m_thread_running.IsSet())
  {
    const int cycles = static_cast<int>(m_pending_cycles.load());
    if (cycles > 0)
    {
      // While the emulator is paused (PauseAndLock) the core belongs to someone else.
      std::unique_lock lock(m_core_lock, std::try_to_lock);
      if (lock.owns_lock())
      {
        if (m_dsp_core.IsJITCreated())
          m_dsp_core.RunCycles(cycles);
        else
          m_dsp_core.GetInterpreter().RunCyclesThread(cycles);
        m_pending_cycles.store(0);
        continue;
      }
    }

    m_dsp_idle.Set();
    m_wake_dsp.Wait();
  }
}

bool DSPLLE::Initialize(bool wii, bool dsp_thread)
{
  m_stop_threading_requested = false;

  DSPInitOptions opts;
  if (!FillDSPInitOptions(&opts))
    return false;
  if (!m_dsp_core.Initialize(opts))
    return false;

  // Only the JIT is worth a thread; determinism rules threads out altogether.
  const bool use_thread = dsp_thread && !Core::WantsDeterminism() && m_dsp_core.IsJITCreated();

  m_wii = wii;
  m_threaded = use_thread;

  m_dsp_core.Reset();
  InitInstructionTable();

  if (use_thread)
  {
    m_thread_running.Set(true);
    m_thread = std::thread(&DSPLLE::ThreadLoop, this);
  }
  return true;
}

void DSPLLE::StopThread()
{
  m_thread_running.Clear();
  m_dsp_idle.Set();
  m_wake_dsp.Set();
  if (m_thread.joinable())
    m_thread.join();
}

void DSPLLE::DSP_StopSoundStream()
{
  if (m_threaded)
    StopThread();
}

void DSPLLE::Shutdown()
{
  m_dsp_core.Shutdown();
}

u16 DSPLLE::DSP_WriteControlRegister(u16 value)
{
  m_dsp_core.GetInterpreter().WriteControlRegister(value);

  if ((value & CR_EXTERNAL_INT) != 0)
  {
    if (m_threaded)
    {
      // Only the Zelda microcode raises this. Its frequent mail exchanges gain
      // nothing from a thread, so leave threaded mode at the next update and let
      // the thread take the interrupt meanwhile.
      m_stop_threading_requested = true;
      m_dsp_core.SetExternalInterrupt(true);
    }
    else
    {
      m_dsp_core.CheckExternalInterrupt();
      m_dsp_core.CheckExceptions();
    }
  }

  return DSP_ReadControlRegister();
}

u16 DSPLLE::DSP_ReadControlRegister()
{
  return m_dsp_core.GetInterpreter().ReadControlRegister();
}

u16 DSPLLE::DSP_ReadMailBoxHigh(bool cpu_mailbox)
{
  return m_dsp_core.ReadMailboxHigh(cpu_mailbox ? Mailbox::CPU : Mailbox::DSP);
}

u16 DSPLLE::DSP_ReadMailBoxLow(bool cpu_mailbox)
{
  return m_dsp_core.ReadMailboxLow(cpu_mailbox ? Mailbox::CPU : Mailbox::DSP);
}

void DSPLLE::DSP_WriteMailBoxHigh(bool cpu_mailbox, u16 value)
{
  if (!cpu_mailbox)
  {
    ERROR_LOG_FMT(DSPLLE, "CPU can't write to DSP mailbox");
    return;
  }

  if ((m_dsp_core.PeekMailbox(Mailbox::CPU) & MAIL_PENDING) != 0)
    WARN_LOG_FMT(DSPLLE, "CPU mailbox overwritten before the DSP read it");
  m_dsp_core.WriteMailboxHigh(Mailbox::CPU, value);
}

void DSPLLE::DSP_WriteMailBoxLow(bool cpu_mailbox, u16 value)
{
  if (!cpu_mailbox)
  {
    ERROR_LOG_FMT(DSPLLE, "CPU can't write to DSP mailbox");
    return;
  }
  m_dsp_core.WriteMailboxLow(Mailbox::CPU, value);
}

void DSPLLE::DSP_Update(int cycles)
{
  const int dsp_cycles = cycles / CPU_CYCLES_PER_DSP_CYCLE;
  if (dsp_cycles <= 0)
    return;

  if (m_threaded && (m_stop_threading_requested || Core::WantsDeterminism()))
  {
    DSP_StopSoundStream();
    m_threaded = false;
    m_stop_threading_requested = false;
    Config::SetBaseOrCurrent(Config::MAIN_DSP_THREAD, false);
  }

  if (!m_threaded)
  {
    m_dsp_core.RunCycles(dsp_cycles);
    return;
  }

  // Hand the cycles to the thread once it has finished the previous batch.
  m_dsp_idle.Wait();
  m_pending_cycles.fetch_add(static_cast<u32>(dsp_cycles));
  m_wake_dsp.Set();
}

u32 DSPLLE::DSP_UpdateRate()
{
  return 12600;
}

void DSPLLE::PauseAndLock()
{
  m_core_lock.lock();
}

void DSPLLE::UnpauseAndUnlock()
{
  m_core_lock.unlock();

  if (m_threaded)
  {
    // Let the thread catch up on anything that piled up during the pause.
    m_dsp_idle.Wait();
    m_wake_dsp.Set();
  }
}
}  // namespace DSP::LLE
