// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/FifoPlayer/FifoPlayer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/TextureDecoder.h"  // s_tex_mem
#include "VideoCommon/VideoCommon.h"

bool IsPlayingBackFifologWithBrokenEFBCopies = false;

namespace
{
// Splits each recorded frame into parts so that playback can leave out individual objects:
// runs of state-setting commands, the primitive data of one object, and EFB copies.
class FramePartitioner final : public OpcodeDecoder::Callback
{
public:
  explicit FramePartitioner(const u32* cp_registers) : m_cp_state(cp_registers) {}

  // The CP state carries over from one frame to the next, as it did while recording.
  void Partition(const FifoFrameInfo& frame, AnalyzedFrameInfo& out);

  OPCODE_CALLBACK(void OnXF(u16, u8, const u8*)) {}
  OPCODE_CALLBACK(void OnCP(u8 command, u32 value)) { GetCPState().LoadCPReg(command, value); }
  OPCODE_CALLBACK(void OnBP(u8 command, u32))
  {
    if (command == BPMEM_TRIGGER_EFB_COPY)
      m_saw_copy = true;
  }
  OPCODE_CALLBACK(void OnIndexedLoad(CPArray, u32, u16, u8)) {}
  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive, u8, u32, u16, const u8*))
  {
    m_saw_primitive = true;
  }
  OPCODE_CALLBACK(void OnDisplayList(u32, u32)) {}
  OPCODE_CALLBACK(void OnNop(u32)) { m_saw_nop = true; }
  OPCODE_CALLBACK(void OnUnknown(u8, const u8*)) {}
  OPCODE_CALLBACK(void OnCommand(const u8*, u32)) { Classify(); }
  OPCODE_CALLBACK(CPState& GetCPState()) { return m_cp_state; }

private:
  // What the command just decoded means for the part boundaries.
  enum class Boundary
  {
    None,
    ObjectBegins,  // first primitive after other commands
    ObjectEnds,    // first other command after primitives
    EfbCopy,       // a copy was triggered (outside a run of primitives)
  };

  void Classify();

  CPState m_cp_state;
  Boundary m_boundary = Boundary::None;
  // In a run of primitive commands (NOPs neither start nor end one).
  bool m_in_primitives = false;
  // Seen while decoding the current command.
  bool m_saw_primitive = false;
  bool m_saw_copy = false;
  bool m_saw_nop = false;
};

void FramePartitioner::Classify()
{
  m_boundary = Boundary::None;
  if (!m_saw_nop)
  {
    if (m_saw_primitive != m_in_primitives)
      m_boundary = m_saw_primitive ? Boundary::ObjectBegins : Boundary::ObjectEnds;
    else if (m_saw_copy)
      m_boundary = Boundary::EfbCopy;
    m_in_primitives = m_saw_primitive;
  }
  m_saw_primitive = false;
  m_saw_copy = false;
  m_saw_nop = false;
}

void FramePartitioner::Partition(const FifoFrameInfo& frame, AnalyzedFrameInfo& out)
{
  const u8* const data = frame.fifoData.data();
  const u32 size = static_cast<u32>(frame.fifoData.size());

  u32 position = 0;
  u32 part_begin = 0;
  // The CP state in effect for an object's primitives. It is taken when the primitives begin:
  // the command that ends them comes later and may already change it.
  CPState object_state;

  while (position < size)
  {
    const u32 command_size = OpcodeDecoder::RunCommand(data + position, size - position, *this);

    switch (m_boundary)
    {
    case Boundary::ObjectBegins:
      out.AddPart(FramePartType::Commands, part_begin, position, m_cp_state);
      part_begin = position;
      static_assert(std::is_trivially_copyable_v<CPState>);
      std::memcpy(static_cast<void*>(&object_state), static_cast<const void*>(&m_cp_state),
                  sizeof(CPState));
      break;
    case Boundary::ObjectEnds:
      out.AddPart(FramePartType::PrimitiveData, part_begin, position, object_state);
      part_begin = position;
      break;
    default:
      break;
    }

    position += command_size;

    // The copy part includes the command that triggers it.
    if (m_boundary == Boundary::EfbCopy)
    {
      out.AddPart(FramePartType::EFBCopy, part_begin, position, m_cp_state);
      part_begin = position;
    }
  }

  // Frames end with the copy to the XFB, so nothing should be left over.
  ASSERT(part_begin == size);
  ASSERT(position == size);
}

// Command bytes of the GP FIFO protocol used to set up GPU state.
constexpr u8 GX_LOAD_CP_REG = 0x08;
constexpr u8 GX_LOAD_XF_REG = 0x10;
constexpr u8 GX_LOAD_BP_REG = 0x61;

// Hardware register blocks, as seen by the CPU.
constexpr u32 CP_REGISTER_BASE = 0xCC000000;
constexpr u32 PI_REGISTER_BASE = 0xCC003000;
}  // namespace

// FifoPlayer::CPUCore -----------------------------------------------------------------------

// Runs in place of the PowerPC core: each "step" plays one frame of the log.
class FifoPlayer::CPUCore final : public CPUCoreBase
{
public:
  explicit CPUCore(FifoPlayer* player) : m_player(*player) {}
  CPUCore(const CPUCore&) = delete;
  CPUCore& operator=(const CPUCore&) = delete;
  ~CPUCore() override = default;

  void Init() override
  {
    IsPlayingBackFifologWithBrokenEFBCopies = m_player.m_File->HasBrokenEFBCopies();
    // The GPU FIFO stays disabled while the CPU is stepping, and ClearEfb() waits for the GPU,
    // so stepping has to be off before the initial state is loaded (dual core would hang).
    m_player.m_system.GetCPU().SetStepping(false);
    m_player.m_CurrentFrame = m_player.m_FrameRangeStart;
    m_player.LoadMemory();
  }

  void Shutdown() override { IsPlayingBackFifologWithBrokenEFBCopies = false; }

  // There is no translated code to throw away.
  void ClearCache() override {}

  void SingleStep() override
  {
    // A frame only completes once the GPU has drawn it, which never happens while stepping
    // keeps the FIFO disabled.
    PanicAlertFmtT("Cannot SingleStep the FIFO. Use Frame Advance instead.");
  }

  const char* GetName() const override { return "FifoPlayer"; }

  void Run() override
  {
    CPU::CPUManager& cpu = m_player.m_system.GetCPU();
    while (cpu.GetState() == CPU::State::Running)
    {
      const CPU::State next = m_player.AdvanceFrame();
      if (next == CPU::State::Running)
        continue;
      cpu.Break();
      // The end of a log that doesn't loop also ends emulation.
      if (next == CPU::State::PowerDown)
        Host_Message(HostMessageID::WMUserStop);
    }
  }

private:
  FifoPlayer& m_player;
};

// FifoPlayer --------------------------------------------------------------------------------

FifoPlayer::FifoPlayer(Core::System& system) : m_system(system)
{
  m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();
}

FifoPlayer::~FifoPlayer()
{
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
}

void FifoPlayer::RefreshConfig()
{
  m_Loop = Config::Get(Config::MAIN_FIFOPLAYER_LOOP_REPLAY);
  m_EarlyMemoryUpdates = Config::Get(Config::MAIN_FIFOPLAYER_EARLY_MEMORY_UPDATES);
}

bool FifoPlayer::Open(const std::string& filename)
{
  Close();

  m_File = FifoDataFile::Load(filename, false);
  if (m_File != nullptr)
  {
    FramePartitioner partitioner(m_File->GetCPMem());
    m_FrameInfo.clear();
    m_FrameInfo.resize(m_File->GetFrameCount());
    for (u32 index = 0; index < m_File->GetFrameCount(); ++index)
      partitioner.Partition(m_File->GetFrame(index), m_FrameInfo[index]);

    // Play everything by default.
    m_FrameRangeEnd = m_File->GetFrameCount() - 1;
  }

  if (m_FileLoadedCb)
    m_FileLoadedCb();
  return m_File != nullptr;
}

void FifoPlayer::Close()
{
  m_File.reset();
  m_FrameRangeStart = 0;
  m_FrameRangeEnd = 0;
}

bool FifoPlayer::IsPlaying() const
{
  return m_File != nullptr && Core::IsRunning(m_system);
}

std::unique_ptr<CPUCoreBase> FifoPlayer::GetCPUCore()
{
  if (m_File == nullptr || m_File->GetFrameCount() == 0)
    return nullptr;
  return std::make_unique<CPUCore>(this);
}

bool FifoPlayer::IsRunningWithFakeVideoInterfaceUpdates() const
{
  const bool has_frames = m_File != nullptr && m_File->GetFrameCount() != 0;
  return has_frames && m_File->ShouldGenerateFakeVIUpdates();
}

void FifoPlayer::SetFileLoadedCallback(CallbackFunc callback)
{
  m_FileLoadedCb = std::move(callback);
  // Someone registering late still learns about the file that is already open.
  if (m_File != nullptr)
    m_FileLoadedCb();
}

u32 FifoPlayer::GetFrameObjectCount(u32 frame) const
{
  if (frame >= m_FrameInfo.size())
    return 0;
  return m_FrameInfo[frame].part_type_counts[FramePartType::PrimitiveData];
}

u32 FifoPlayer::GetCurrentFrameObjectCount() const
{
  return GetFrameObjectCount(m_CurrentFrame);
}

u32 FifoPlayer::GetMaxObjectCount() const
{
  u32 most = 0;
  for (const AnalyzedFrameInfo& info : m_FrameInfo)
    most = std::max(most, info.part_type_counts[FramePartType::PrimitiveData]);
  return most;
}

void FifoPlayer::SetFrameRangeStart(u32 start)
{
  if (m_File == nullptr)
    return;

  m_FrameRangeStart = std::min(start, m_File->GetFrameCount() - 1);
  // Keep the range non-empty and the current frame inside it.
  m_FrameRangeEnd = std::max(m_FrameRangeEnd, m_FrameRangeStart);
  m_CurrentFrame = std::max(m_CurrentFrame, m_FrameRangeStart);
}

void FifoPlayer::SetFrameRangeEnd(u32 end)
{
  if (m_File == nullptr)
    return;

  m_FrameRangeEnd = std::min(end, m_File->GetFrameCount() - 1);
  m_FrameRangeStart = std::min(m_FrameRangeStart, m_FrameRangeEnd);
  // Restart from the beginning of the range when the current frame falls off its end.
  if (m_CurrentFrame >= m_FrameRangeEnd)
    m_CurrentFrame = m_FrameRangeStart;
}

CPU::State FifoPlayer::AdvanceFrame()
{
  if (m_CurrentFrame > m_FrameRangeEnd)
  {
    if (!m_Loop)
      return CPU::State::PowerDown;

    // Every pass over the range starts from the recorded GPU state, so all passes look alike.
    m_CurrentFrame = m_FrameRangeStart;
    LoadRegisters();
    LoadTextureMemory();
    FlushWGP();
  }

  if (m_FrameWrittenCb)
    m_FrameWrittenCb();

  if (m_EarlyMemoryUpdates && m_CurrentFrame == m_FrameRangeStart)
    WriteAllMemoryUpdates();

  WriteFrame(m_File->GetFrame(m_CurrentFrame), m_FrameInfo[m_CurrentFrame]);
  ++m_CurrentFrame;
  return CPU::State::Running;
}

void FifoPlayer::WriteFrame(const FifoFrameInfo& frame, const AnalyzedFrameInfo& info)
{
  // Spread one video frame's worth of CPU time over the frame's FIFO bytes.
  const VideoInterface::VideoInterfaceManager& vi = m_system.GetVideoInterface();
  const u64 ticks_per_second = m_system.GetSystemTimers().GetTicksPerSecond();
  m_CyclesPerFrame =
      ticks_per_second * vi.GetTargetRefreshRateDenominator() / vi.GetTargetRefreshRateNumerator();
  m_ElapsedCycles = 0;
  m_FrameFifoSize = static_cast<u32>(frame.fifoData.size());

  // With early updates every memory update has been written already.
  u32 next_update = m_EarlyMemoryUpdates ? static_cast<u32>(frame.memoryUpdates.size()) : 0;

  // State-setting commands and copies always play, as later objects depend on them; only the
  // primitives of objects outside the object range are left out.
  u32 object_index = 0;
  for (const FramePart& part : info.parts)
  {
    if (part.m_type == FramePartType::PrimitiveData)
    {
      const u32 object = object_index++;
      if (object < m_ObjectRangeStart || object > m_ObjectRangeEnd)
        continue;
    }
    WriteFramePart(part, &next_update, frame);
  }

  FlushWGP();
  WaitForGPUInactive();
}

void FifoPlayer::WriteFramePart(const FramePart& part, u32* next_mem_update,
                                const FifoFrameInfo& frame)
{
  const std::vector<MemoryUpdate>& updates = frame.memoryUpdates;
  const u8* const data = frame.fifoData.data();
  u32 position = part.m_start;

  // Memory updates recorded inside this part go in at the FIFO position they were seen at.
  while (position < part.m_end && *next_mem_update < updates.size())
  {
    const MemoryUpdate& update = updates[*next_mem_update];
    if (update.fifoPosition >= part.m_end)
      break;
    if (position < update.fifoPosition)
    {
      WriteFifo(data, position, update.fifoPosition);
      position = update.fifoPosition;
    }
    WriteMemory(update);
    ++*next_mem_update;
  }

  if (position < part.m_end)
    WriteFifo(data, position, part.m_end);
}

void FifoPlayer::WriteAllMemoryUpdates()
{
  ASSERT(m_File);
  for (u32 index = 0; index < m_File->GetFrameCount(); ++index)
  {
    for (const MemoryUpdate& update : m_File->GetFrame(index).memoryUpdates)
      WriteMemory(update);
  }
}

void FifoPlayer::WriteMemory(const MemoryUpdate& memUpdate)
{
  // Bit 28 selects MEM2 (Wii only); the rest of the address is wrapped into the RAM size.
  Memory::MemoryManager& memory = m_system.GetMemory();
  const bool in_mem2 = (memUpdate.address & 0x10000000) != 0;
  u8* const target = in_mem2 ? &memory.GetEXRAM()[memUpdate.address & memory.GetExRamMask()] :
                               &memory.GetRAM()[memUpdate.address & memory.GetRamMask()];
  std::ranges::copy(memUpdate.data, target);
}

void FifoPlayer::WriteFifo(const u8* data, u32 start, u32 end)
{
  CPU::CPUManager& cpu = m_system.GetCPU();
  CoreTiming::CoreTimingManager& timing = m_system.GetCoreTiming();
  GPFifo::GPFifoManager& gather_pipe = m_system.GetGPFifo();
  PowerPC::PowerPCState& ppc_state = m_system.GetPPCState();

  // Bursts of up to 256 bytes: all but the last byte go straight into the gather pipe buffer,
  // the last one through a normal write so that the pipe gets processed. The final burst stops
  // short of `end` the same way.
  u32 position = start;
  while (position < end)
  {
    // Let the GPU catch up while the FIFO is nearly full (unless emulation is stopping).
    while (IsHighWatermarkSet() && cpu.GetState() == CPU::State::Running)
    {
      timing.Idle();
      timing.Advance();
    }

    const u32 direct_end = std::min(position + 255, end - 1);
    ppc_state.gather_pipe_ptr =
        std::copy(data + position, data + direct_end, ppc_state.gather_pipe_ptr);
    position = direct_end;
    gather_pipe.Write8(data[position]);
    ++position;

    // Charge the CPU time proportional to the share of the frame written so far.
    const u32 elapsed =
        static_cast<u32>(static_cast<u64>(position) * m_CyclesPerFrame / m_FrameFifoSize);
    const u32 used = elapsed - m_ElapsedCycles;
    m_ElapsedCycles = elapsed;
    ppc_state.downcount = static_cast<s32>(static_cast<u32>(ppc_state.downcount) - used);
    timing.Advance();
  }
}

// Initial state ------------------------------------------------------------------------------

void FifoPlayer::LoadMemory()
{
  PowerPC::PowerPCState& ppc_state = m_system.GetPPCState();

  // Translation on, with the standard BAT mapping of an SDK title: cached and uncached views of
  // MEM1 (and of MEM2 on Wii).
  UReg_MSR msr;
  msr.IR = 1;
  msr.DR = 1;
  ppc_state.msr.Hex = msr.Hex;

  ppc_state.spr[SPR_IBAT0U] = 0x80001fff;
  ppc_state.spr[SPR_IBAT0L] = 0x00000002;
  ppc_state.spr[SPR_DBAT0U] = 0x80001fff;
  ppc_state.spr[SPR_DBAT0L] = 0x00000002;
  ppc_state.spr[SPR_DBAT1U] = 0xc0001fff;
  ppc_state.spr[SPR_DBAT1L] = 0x0000002a;
  if (m_File->GetIsWii())
  {
    ppc_state.spr[SPR_IBAT4U] = 0x90001fff;
    ppc_state.spr[SPR_IBAT4L] = 0x10000002;
    ppc_state.spr[SPR_DBAT4U] = 0x90001fff;
    ppc_state.spr[SPR_DBAT4L] = 0x10000002;
    ppc_state.spr[SPR_DBAT5U] = 0xd0001fff;
    ppc_state.spr[SPR_DBAT5L] = 0x1000002a;
    // The upper BATs are only used with SBE set.
    HID4(ppc_state).SBE = 1;
  }

  m_system.GetPowerPC().MSRUpdated();
  PowerPC::MMU& mmu = m_system.GetMMU();
  mmu.DBATUpdated();
  mmu.IBATUpdated();

  SetupFifo();
  LoadRegisters();
  ClearEfb();
  LoadTextureMemory();
  FlushWGP();
}

void FifoPlayer::SetupFifo()
{
  const FifoFrameInfo& frame = m_File->GetFrame(m_CurrentFrame);
  const u32 base = frame.fifoStart;
  const u32 end = frame.fifoEnd;
  // High watermark at three quarters of the FIFO, low watermark at its start.
  const u32 high_watermark = (end - base) * 3 / 4;

  // Each 32-bit CP value goes into a pair of 16-bit registers (the writes truncate).
  const std::pair<u32, u32> cp_writes[] = {
      {CommandProcessor::CTRL_REGISTER, 0},   // reads, breakpoints and interrupts off
      {CommandProcessor::CLEAR_REGISTER, 7},  // clear overflow, underflow and metrics
      {CommandProcessor::FIFO_BASE_LO, base},
      {CommandProcessor::FIFO_BASE_HI, base >> 16},
      {CommandProcessor::FIFO_END_LO, end},
      {CommandProcessor::FIFO_END_HI, end >> 16},
      {CommandProcessor::FIFO_HI_WATERMARK_LO, high_watermark},
      {CommandProcessor::FIFO_HI_WATERMARK_HI, high_watermark >> 16},
      {CommandProcessor::FIFO_LO_WATERMARK_LO, 0},
      {CommandProcessor::FIFO_LO_WATERMARK_HI, 0},
      // Empty FIFO: both pointers at the base.
      {CommandProcessor::FIFO_RW_DISTANCE_LO, 0},
      {CommandProcessor::FIFO_RW_DISTANCE_HI, 0},
      {CommandProcessor::FIFO_WRITE_POINTER_LO, base},
      {CommandProcessor::FIFO_WRITE_POINTER_HI, base >> 16},
      {CommandProcessor::FIFO_READ_POINTER_LO, base},
      {CommandProcessor::FIFO_READ_POINTER_HI, base >> 16},
  };
  for (const auto& [reg, value] : cp_writes)
    WriteCP(reg, static_cast<u16>(value));

  // The CPU side of the same FIFO.
  WritePI(ProcessorInterface::PI_FIFO_BASE, base);
  WritePI(ProcessorInterface::PI_FIFO_END, end);
  WritePI(ProcessorInterface::PI_FIFO_WPTR, base);
  FlushWGP();
  WritePI(ProcessorInterface::PI_FIFO_WPTR, base);

  WriteCP(CommandProcessor::CTRL_REGISTER, 17);  // reads on, GP link on
}

void FifoPlayer::LoadRegisters()
{
  const u32* const bp = m_File->GetBPMem();
  for (u32 reg = 0; reg < FifoDataFile::BP_MEM_SIZE; ++reg)
  {
    if (ShouldLoadBP(static_cast<u8>(reg)))
      LoadBPReg(static_cast<u8>(reg), bp[reg]);
  }

  // The CP registers that hold state (the rest are commands or unused).
  const u32* const cp = m_File->GetCPMem();
  const auto load_cp = [this, cp](u32 reg) { LoadCPReg(static_cast<u8>(reg), cp[reg]); };
  for (const u32 reg : {MATINDEX_A, MATINDEX_B, VCD_LO, VCD_HI})
    load_cp(reg);
  for (u32 vat = 0; vat < CP_NUM_VAT_REG; ++vat)
  {
    load_cp(CP_VAT_REG_A + vat);
    load_cp(CP_VAT_REG_B + vat);
    load_cp(CP_VAT_REG_C + vat);
  }
  for (u32 array = 0; array < CP_NUM_ARRAYS; ++array)
  {
    load_cp(ARRAY_BASE + array);
    load_cp(ARRAY_STRIDE + array);
  }

  const u32* const xf_memory = m_File->GetXFMem();
  for (u32 address = 0; address < FifoDataFile::XF_MEM_SIZE; address += 16)
    LoadXFMem16(static_cast<u16>(address), xf_memory + address);

  const u32* const xf_registers = m_File->GetXFRegs();
  for (u32 reg = 0; reg < FifoDataFile::XF_REGS_SIZE; ++reg)
  {
    if (ShouldLoadXF(static_cast<u8>(reg)))
      LoadXFReg(static_cast<u16>(reg), xf_registers[reg]);
  }
}

void FifoPlayer::LoadTextureMemory()
{
  static_assert(static_cast<size_t>(TMEM_SIZE) == static_cast<size_t>(FifoDataFile::TEX_MEM_SIZE),
                "the log stores all of TMEM");
  std::memcpy(s_tex_mem.data(), m_File->GetTexMem(), FifoDataFile::TEX_MEM_SIZE);
}

void FifoPlayer::ClearEfb()
{
  // Clear the EFB with a copy of the whole buffer to address 0 (anything recorded there comes
  // back with LoadTextureMemory and the memory updates). Clear colour and depth come from the
  // registers loaded just before.
  // (Each register is read just before it is replaced: the writes may already reach the GPU.)
  X10Y10 top_left = bpmem.copyTexSrcXY;
  top_left.x = 0;
  top_left.y = 0;
  LoadBPReg(BPMEM_EFB_TL, top_left.hex);
  X10Y10 size = bpmem.copyTexSrcWH;
  size.x = EFB_WIDTH - 1;
  size.y = EFB_HEIGHT - 1;
  LoadBPReg(BPMEM_EFB_WH, size.hex);
  LoadBPReg(BPMEM_EFB_STRIDE, 0x140);
  LoadBPReg(BPMEM_EFB_ADDR, 0);

  UPE_Copy copy = bpmem.triggerEFBCopy;
  copy.clear = true;
  copy.target_pixel_format = static_cast<u32>(EFBCopyFormat::RGBA8) << 1;
  copy.gamma = GammaCorrection::Gamma1_0;
  copy.frame_to_field = FrameToField::Progressive;
  copy.clamp_top = false;
  copy.clamp_bottom = false;
  copy.unknown_bit = false;
  copy.half_scale = false;
  copy.scale_invert = false;
  copy.copy_to_xfb = false;
  copy.intensity_fmt = false;
  copy.auto_conv = false;
  LoadBPReg(BPMEM_TRIGGER_EFB_COPY, copy.Hex);

  // Put the recorded copy rectangle back, for logs that rely on it being set at the start.
  const u32* const bp = m_File->GetBPMem();
  for (const u8 reg : {BPMEM_EFB_TL, BPMEM_EFB_WH, BPMEM_EFB_STRIDE, BPMEM_EFB_ADDR})
    LoadBPReg(reg, bp[reg]);

  // Let the copy finish now, so that it can't overwrite memory updates written later.
  FlushWGP();
  WaitForGPUInactive();
}

bool FifoPlayer::ShouldLoadBP(u8 address)
{
  // Registers that trigger actions (or interrupts) rather than holding state.
  switch (address)
  {
  case BPMEM_SETDRAWDONE:
  case BPMEM_PE_TOKEN_ID:
  case BPMEM_PE_TOKEN_INT_ID:
  case BPMEM_TRIGGER_EFB_COPY:
  case BPMEM_LOADTLUT1:
  case BPMEM_PRELOAD_MODE:
  case BPMEM_PERF1:
    return false;
  default:
    return true;
  }
}

bool FifoPlayer::ShouldLoadXF(u8 reg)
{
  // Skip the register addresses with no known function.
  const u16 address = static_cast<u16>(reg + 0x1000);
  const auto within = [address](u16 first, u16 last) {
    return address >= first && address <= last;
  };
  if (address == XFMEM_UNKNOWN_1007)
    return false;
  return !within(XFMEM_UNKNOWN_GROUP_1_START, XFMEM_UNKNOWN_GROUP_1_END) &&
         !within(XFMEM_UNKNOWN_GROUP_2_START, XFMEM_UNKNOWN_GROUP_2_END) &&
         !within(XFMEM_UNKNOWN_GROUP_3_START, XFMEM_UNKNOWN_GROUP_3_END);
}

// GPU access ---------------------------------------------------------------------------------

void FifoPlayer::WriteCP(u32 address, u16 value)
{
  m_system.GetMMU().Write<u16>(value, CP_REGISTER_BASE | address);
}

void FifoPlayer::WritePI(u32 address, u32 value)
{
  m_system.GetMMU().Write<u32>(value, PI_REGISTER_BASE | address);
}

bool FifoPlayer::IsIdleSet() const
{
  const CommandProcessor::UCPStatusReg status{
      m_system.GetMMU().Read<u16>(CP_REGISTER_BASE | CommandProcessor::STATUS_REGISTER)};
  return status.CommandIdle;
}

bool FifoPlayer::IsHighWatermarkSet() const
{
  const CommandProcessor::UCPStatusReg status{
      m_system.GetMMU().Read<u16>(CP_REGISTER_BASE | CommandProcessor::STATUS_REGISTER)};
  return status.OverflowHiWatermark;
}

void FifoPlayer::WaitForGPUInactive()
{
  CoreTiming::CoreTimingManager& timing = m_system.GetCoreTiming();
  const CPU::CPUManager& cpu = m_system.GetCPU();
  while (!IsIdleSet() && cpu.GetState() != CPU::State::PowerDown)
  {
    timing.Idle();
    timing.Advance();
  }
}

void FifoPlayer::FlushWGP()
{
  // 31 zero bytes push out whatever is still sitting in the gather pipe.
  GPFifo::GPFifoManager& gather_pipe = m_system.GetGPFifo();
  for (u32 word = 0; word < 7; ++word)
    gather_pipe.Write32(0);
  gather_pipe.Write16(0);
  gather_pipe.Write8(0);
  gather_pipe.ResetGatherPipe();
}

void FifoPlayer::LoadBPReg(u8 reg, u32 value)
{
  // One word: register number in the top byte, 24 bits of value.
  GPFifo::GPFifoManager& gather_pipe = m_system.GetGPFifo();
  gather_pipe.Write8(GX_LOAD_BP_REG);
  gather_pipe.Write32((static_cast<u32>(reg) << 24) | (value & 0x00ffffff));
}

void FifoPlayer::LoadCPReg(u8 reg, u32 value)
{
  GPFifo::GPFifoManager& gather_pipe = m_system.GetGPFifo();
  gather_pipe.Write8(GX_LOAD_CP_REG);
  gather_pipe.Write8(reg);
  gather_pipe.Write32(value);
}

void FifoPlayer::LoadXFReg(u16 reg, u32 value)
{
  // XF loads name a count (minus one) in the upper half and the address in the lower half;
  // registers live at 0x1000 and up.
  GPFifo::GPFifoManager& gather_pipe = m_system.GetGPFifo();
  gather_pipe.Write8(GX_LOAD_XF_REG);
  gather_pipe.Write32(0x1000 | (reg & 0x0fff));
  gather_pipe.Write32(value);
}

void FifoPlayer::LoadXFMem16(u16 address, const u32* data)
{
  GPFifo::GPFifoManager& gather_pipe = m_system.GetGPFifo();
  gather_pipe.Write8(GX_LOAD_XF_REG);
  gather_pipe.Write32((15u << 16) | address);
  for (const u32 word : std::span<const u32, 16>(data, 16))
    gather_pipe.Write32(word);
}
