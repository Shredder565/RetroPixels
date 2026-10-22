// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/System.h"

#include <memory>

#include "AudioCommon/SoundStream.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/HSP/HSP.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/MemoryInterface.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/Sram.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/IOS/USB/Emulated/Infinity.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/USBScanner.h"
#include "Core/Movie.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/XFStateManager.h"

namespace Core
{
// The members are constructed top to bottom and destroyed bottom to top.
// That order is load-bearing: some constructors register hooks with objects
// declared above them (the video events come first for that reason), and the
// MMU and interpreter take references to the PowerPC core and memory.
struct System::Parts
{
  explicit Parts(System& owner)
      : audio_interface(owner), core_timing(owner), command_processor{owner}, cpu(owner),
        dsp(owner), dvd_interface(owner), dvd_thread(owner), expansion_interface(owner),
        fifo{owner}, gp_fifo(owner), wii_ipc(owner), memory(owner), pixel_engine{owner},
        power_pc(owner), mmu(owner, memory, power_pc), processor_interface(owner),
        serial_interface(owner), system_timers(owner), video_interface(owner),
        interpreter(owner, power_pc.GetPPCState(), mmu, power_pc.GetBranchWatch(),
                    power_pc.GetSymbolDB()),
        jit_interface(owner), fifo_player(owner), fifo_recorder(owner), movie(owner)
  {
  }

  VideoEvents video_events;

  std::unique_ptr<SoundStream> sound_stream;
  bool sound_stream_running = false;
  bool audio_dump_started = false;

  std::unique_ptr<IOS::HLE::EmulationKernel> ios;

  AudioInterface::AudioInterfaceManager audio_interface;
  CoreTiming::CoreTimingManager core_timing;
  CommandProcessor::CommandProcessorManager command_processor;
  CPU::CPUManager cpu;
  DSP::DSPManager dsp;
  DVD::DVDInterface dvd_interface;
  DVD::DVDThread dvd_thread;
  ExpansionInterface::ExpansionInterfaceManager expansion_interface;
  Fifo::FifoManager fifo;
  GeometryShaderManager geometry_shaders;
  GPFifo::GPFifoManager gp_fifo;
  HSP::HSPManager hsp;
  IOS::HLE::USB::InfinityBase infinity_base;
  IOS::HLE::USB::SkylanderPortal skylander_portal;
  IOS::WiiIPC wii_ipc;
  Memory::MemoryManager memory;
  MemoryInterface::MemoryInterfaceManager memory_interface;
  PerformanceMetrics perf_metrics;
  PixelEngine::PixelEngineManager pixel_engine;
  PixelShaderManager pixel_shaders;
  PowerPC::PowerPCManager power_pc;
  PowerPC::MMU mmu;
  ProcessorInterface::ProcessorInterfaceManager processor_interface;
  SerialInterface::SerialInterfaceManager serial_interface;
  Sram sram;
  SystemTimers::SystemTimersManager system_timers;
  IOS::HLE::USBScanner usb_scanner;
  VertexShaderManager vertex_shaders;
  XFStateManager xf_state;
  VideoInterface::VideoInterfaceManager video_interface;
  Interpreter interpreter;
  JitInterface jit_interface;
  VideoCommon::CustomResourceManager custom_resources;
  FifoPlayer fifo_player;
  FifoRecorder fifo_recorder;
  Movie::MovieManager movie;
};

System::System() : m_parts{std::make_unique<Parts>(*this)}
{
}

System::~System() = default;

void System::Initialize()
{
  m_separate_cpu_and_gpu_threads = Config::Get(Config::MAIN_CPU_THREAD);
  m_mmu_enabled = Config::Get(Config::MAIN_MMU);
  m_pause_on_panic_enabled = Config::Get(Config::MAIN_PAUSE_ON_PANIC);
  m_simulated_memory_size = SConfig::GetInstance().GetSimulatedMemorySize();
}

// Sound output state.

SoundStream* System::GetSoundStream() const
{
  return m_parts->sound_stream.get();
}

void System::SetSoundStream(std::unique_ptr<SoundStream> sound_stream)
{
  m_parts->sound_stream = std::move(sound_stream);
}

bool System::IsSoundStreamRunning() const
{
  return m_parts->sound_stream_running;
}

void System::SetSoundStreamRunning(bool running)
{
  m_parts->sound_stream_running = running;
}

bool System::IsAudioDumpStarted() const
{
  return m_parts->audio_dump_started;
}

void System::SetAudioDumpStarted(bool started)
{
  m_parts->audio_dump_started = started;
}

// The IOS kernel only exists while a Wii title runs.

IOS::HLE::EmulationKernel* System::GetIOS() const
{
  return m_parts->ios.get();
}

void System::SetIOS(std::unique_ptr<IOS::HLE::EmulationKernel> ios)
{
  m_parts->ios = std::move(ios);
}

// Hardware.

AudioInterface::AudioInterfaceManager& System::GetAudioInterface() const
{
  return m_parts->audio_interface;
}

CPU::CPUManager& System::GetCPU() const
{
  return m_parts->cpu;
}

CoreTiming::CoreTimingManager& System::GetCoreTiming() const
{
  return m_parts->core_timing;
}

DSP::DSPManager& System::GetDSP() const
{
  return m_parts->dsp;
}

DVD::DVDInterface& System::GetDVDInterface() const
{
  return m_parts->dvd_interface;
}

DVD::DVDThread& System::GetDVDThread() const
{
  return m_parts->dvd_thread;
}

ExpansionInterface::ExpansionInterfaceManager& System::GetExpansionInterface() const
{
  return m_parts->expansion_interface;
}

GPFifo::GPFifoManager& System::GetGPFifo() const
{
  return m_parts->gp_fifo;
}

HSP::HSPManager& System::GetHSP() const
{
  return m_parts->hsp;
}

IOS::WiiIPC& System::GetWiiIPC() const
{
  return m_parts->wii_ipc;
}

Memory::MemoryManager& System::GetMemory() const
{
  return m_parts->memory;
}

MemoryInterface::MemoryInterfaceManager& System::GetMemoryInterface() const
{
  return m_parts->memory_interface;
}

ProcessorInterface::ProcessorInterfaceManager& System::GetProcessorInterface() const
{
  return m_parts->processor_interface;
}

SerialInterface::SerialInterfaceManager& System::GetSerialInterface() const
{
  return m_parts->serial_interface;
}

Sram& System::GetSRAM() const
{
  return m_parts->sram;
}

SystemTimers::SystemTimersManager& System::GetSystemTimers() const
{
  return m_parts->system_timers;
}

VideoInterface::VideoInterfaceManager& System::GetVideoInterface() const
{
  return m_parts->video_interface;
}

// CPU emulation.

PowerPC::PowerPCManager& System::GetPowerPC() const
{
  return m_parts->power_pc;
}

PowerPC::PowerPCState& System::GetPPCState() const
{
  return m_parts->power_pc.GetPPCState();
}

PPCSymbolDB& System::GetPPCSymbolDB() const
{
  return m_parts->power_pc.GetSymbolDB();
}

PowerPC::MMU& System::GetMMU() const
{
  return m_parts->mmu;
}

Interpreter& System::GetInterpreter() const
{
  return m_parts->interpreter;
}

JitInterface& System::GetJitInterface() const
{
  return m_parts->jit_interface;
}

// Emulated USB toys and the USB device scanner.

IOS::HLE::USB::SkylanderPortal& System::GetSkylanderPortal() const
{
  return m_parts->skylander_portal;
}

IOS::HLE::USB::InfinityBase& System::GetInfinityBase() const
{
  return m_parts->infinity_base;
}

IOS::HLE::USBScanner& System::GetUSBScanner() const
{
  return m_parts->usb_scanner;
}

// Graphics.

CommandProcessor::CommandProcessorManager& System::GetCommandProcessor() const
{
  return m_parts->command_processor;
}

Fifo::FifoManager& System::GetFifo() const
{
  return m_parts->fifo;
}

PixelEngine::PixelEngineManager& System::GetPixelEngine() const
{
  return m_parts->pixel_engine;
}

GeometryShaderManager& System::GetGeometryShaderManager() const
{
  return m_parts->geometry_shaders;
}

PixelShaderManager& System::GetPixelShaderManager() const
{
  return m_parts->pixel_shaders;
}

VertexShaderManager& System::GetVertexShaderManager() const
{
  return m_parts->vertex_shaders;
}

XFStateManager& System::GetXFStateManager() const
{
  return m_parts->xf_state;
}

VideoCommon::CustomResourceManager& System::GetCustomResourceManager() const
{
  return m_parts->custom_resources;
}

VideoEvents& System::GetVideoEvents() const
{
  return m_parts->video_events;
}

PerformanceMetrics& System::GetPerfMetrics() const
{
  return m_parts->perf_metrics;
}

// FIFO logs and movies.

FifoPlayer& System::GetFifoPlayer() const
{
  return m_parts->fifo_player;
}

FifoRecorder& System::GetFifoRecorder() const
{
  return m_parts->fifo_recorder;
}

Movie::MovieManager& System::GetMovie() const
{
  return m_parts->movie;
}
}  // namespace Core

// Declared in VideoCommon/VideoEvents.h for code that has no System at hand.
VideoEvents& GetVideoEvents()
{
  return Core::System::GetInstance().GetVideoEvents();
}
