// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

// Forward declarations of every subsystem the System owns.

// Hardware.
namespace AudioInterface { class AudioInterfaceManager; }
namespace CPU { class CPUManager; }
namespace CoreTiming { class CoreTimingManager; }
namespace DSP { class DSPManager; }
namespace DVD { class DVDInterface; class DVDThread; }
namespace ExpansionInterface { class ExpansionInterfaceManager; }
namespace GPFifo { class GPFifoManager; }
namespace HSP { class HSPManager; }
namespace IOS { class WiiIPC; }
namespace Memory { class MemoryManager; }
namespace MemoryInterface { class MemoryInterfaceManager; }
namespace ProcessorInterface { class ProcessorInterfaceManager; }
namespace SerialInterface { class SerialInterfaceManager; }
namespace SystemTimers { class SystemTimersManager; }
namespace VideoInterface { class VideoInterfaceManager; }
struct Sram;

// CPU emulation.
namespace PowerPC { class MMU; class PowerPCManager; struct PowerPCState; }
class Interpreter;
class JitInterface;
class PPCSymbolDB;

// IOS and emulated USB devices.
namespace IOS::HLE { class EmulationKernel; class USBScanner; }
namespace IOS::HLE::USB { class SkylanderPortal; class InfinityBase; }

// Graphics.
namespace CommandProcessor { class CommandProcessorManager; }
namespace Fifo { class FifoManager; }
namespace PixelEngine { class PixelEngineManager; }
namespace VideoCommon { class CustomResourceManager; }
class GeometryShaderManager;
class PixelShaderManager;
class VertexShaderManager;
class XFStateManager;
struct VideoEvents;
class PerformanceMetrics;

// Everything else.
namespace Movie { class MovieManager; }
class FifoPlayer;
class FifoRecorder;
class SoundStream;

namespace Core
{
// The one emulated console: owns every subsystem object and a few flags
// that are fixed for the length of an emulation session.
class System
{
public:
  ~System();

  System(const System&) = delete;
  System& operator=(const System&) = delete;
  System(System&&) = delete;
  System& operator=(System&&) = delete;

  // The process-wide instance, created on first use.
  static System& GetInstance()
  {
    static System s_system;
    return s_system;
  }

  // Latches the session settings (dual core, MMU, pause on panic, memory size).
  void Initialize();

  bool IsDualCoreMode() const { return m_separate_cpu_and_gpu_threads; }
  bool IsMMUMode() const { return m_mmu_enabled; }
  bool IsPauseOnPanicMode() const { return m_pause_on_panic_enabled; }
  bool IsMIOS() const { return m_is_mios; }
  bool IsTriforce() const { return m_is_triforce; }
  bool IsWii() const { return m_is_wii; }
  bool IsBranchWatchIgnoreApploader() { return m_branch_watch_ignore_apploader; }
  u32 GetSimulatedMemorySize() const { return m_simulated_memory_size; }

  void SetIsMIOS(bool is_mios) { m_is_mios = is_mios; }
  void SetIsTriforce(bool is_triforce) { m_is_triforce = is_triforce; }
  void SetIsWii(bool is_wii) { m_is_wii = is_wii; }
  void SetIsBranchWatchIgnoreApploader(bool enable) { m_branch_watch_ignore_apploader = enable; }

  SoundStream* GetSoundStream() const;
  void SetSoundStream(std::unique_ptr<SoundStream> sound_stream);
  bool IsSoundStreamRunning() const;
  void SetSoundStreamRunning(bool running);
  bool IsAudioDumpStarted() const;
  void SetAudioDumpStarted(bool started);

  IOS::HLE::EmulationKernel* GetIOS() const;
  void SetIOS(std::unique_ptr<IOS::HLE::EmulationKernel> ios);

  AudioInterface::AudioInterfaceManager& GetAudioInterface() const;
  CPU::CPUManager& GetCPU() const;
  CoreTiming::CoreTimingManager& GetCoreTiming() const;
  CommandProcessor::CommandProcessorManager& GetCommandProcessor() const;
  DSP::DSPManager& GetDSP() const;
  DVD::DVDInterface& GetDVDInterface() const;
  DVD::DVDThread& GetDVDThread() const;
  ExpansionInterface::ExpansionInterfaceManager& GetExpansionInterface() const;
  Fifo::FifoManager& GetFifo() const;
  FifoPlayer& GetFifoPlayer() const;
  FifoRecorder& GetFifoRecorder() const;
  GeometryShaderManager& GetGeometryShaderManager() const;
  GPFifo::GPFifoManager& GetGPFifo() const;
  HSP::HSPManager& GetHSP() const;
  Interpreter& GetInterpreter() const;
  JitInterface& GetJitInterface() const;
  IOS::HLE::USB::SkylanderPortal& GetSkylanderPortal() const;
  IOS::HLE::USB::InfinityBase& GetInfinityBase() const;
  IOS::WiiIPC& GetWiiIPC() const;
  Memory::MemoryManager& GetMemory() const;
  MemoryInterface::MemoryInterfaceManager& GetMemoryInterface() const;
  PowerPC::MMU& GetMMU() const;
  Movie::MovieManager& GetMovie() const;
  PerformanceMetrics& GetPerfMetrics() const;
  PixelEngine::PixelEngineManager& GetPixelEngine() const;
  PixelShaderManager& GetPixelShaderManager() const;
  PowerPC::PowerPCManager& GetPowerPC() const;
  PowerPC::PowerPCState& GetPPCState() const;
  PPCSymbolDB& GetPPCSymbolDB() const;
  ProcessorInterface::ProcessorInterfaceManager& GetProcessorInterface() const;
  SerialInterface::SerialInterfaceManager& GetSerialInterface() const;
  Sram& GetSRAM() const;
  SystemTimers::SystemTimersManager& GetSystemTimers() const;
  IOS::HLE::USBScanner& GetUSBScanner() const;
  VertexShaderManager& GetVertexShaderManager() const;
  XFStateManager& GetXFStateManager() const;
  VideoInterface::VideoInterfaceManager& GetVideoInterface() const;
  VideoCommon::CustomResourceManager& GetCustomResourceManager() const;
  VideoEvents& GetVideoEvents() const;

private:
  System();

  // All subsystem objects live here, defined in System.cpp.
  struct Parts;
  std::unique_ptr<Parts> m_parts;

  bool m_separate_cpu_and_gpu_threads = false;
  bool m_mmu_enabled = false;
  bool m_pause_on_panic_enabled = false;
  bool m_is_mios = false;
  bool m_is_triforce = false;
  bool m_is_wii = false;
  bool m_branch_watch_ignore_apploader = false;
  u32 m_simulated_memory_size{0};
};
}  // namespace Core
