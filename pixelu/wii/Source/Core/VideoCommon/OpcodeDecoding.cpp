// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Executes GX command streams. The same decoder runs in two passes:
//  - the main pass applies every command to the emulated GPU state and draws;
//  - the preprocess pass (deterministic dual core, on the CPU thread) only tracks what is needed
//    to size later commands -- CP vertex state and some BP state -- and captures display list
//    contents so the GPU thread later sees exactly what the CPU saw.
// Display lists cannot be translated ahead of time: how long each vertex is depends on the CP
// state at the moment the list is called, so they are decoded every time they run.

#include "VideoCommon/OpcodeDecoding.h"

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

namespace OpcodeDecoder
{
bool g_record_fifo_data = false;

namespace
{
// Rough GPU cost model, in CPU cycles.
constexpr u32 COST_REGISTER_LOAD = 12;  // one BP or CP write
constexpr u32 COST_XF_BASE = 18;
constexpr u32 COST_XF_PER_WORD = 6;
constexpr u32 COST_SHORT_COMMAND = 6;  // indexed load, DL call, NOP byte, 0x44/0x48
constexpr u32 COST_PER_VERTEX = 4 * 3;  // four GPU clocks, three CPU cycles each

bool IsVatRegister(u8 sub_command)
{
  return sub_command == CP_VAT_REG_A || sub_command == CP_VAT_REG_B ||
         sub_command == CP_VAT_REG_C;
}

bool IsVertexDescriptor(u8 sub_command)
{
  return sub_command == VCD_LO || sub_command == VCD_HI;
}
}  // namespace

template <bool is_preprocess>
class RunCallback final : public Callback
{
public:
  OPCODE_CALLBACK(void OnXF(u16 address, u8 count, const u8* data))
  {
    m_cycles += COST_XF_BASE + COST_XF_PER_WORD * count;
    // The preprocess pass has no use for transform state.
    if constexpr (!is_preprocess)
    {
      LoadXFReg(address, count, data);
      INCSTAT(g_stats.this_frame.num_xf_loads);
    }
  }

  OPCODE_CALLBACK(void OnCP(u8 command, u32 value))
  {
    m_cycles += COST_REGISTER_LOAD;

    if constexpr (is_preprocess)
      MarkPreprocessFormatsDirty(command);
    else
      NoteMainCPWrite(command, value);

    GetCPState().LoadCPReg(command, value);
  }

  OPCODE_CALLBACK(void OnBP(u8 command, u32 value))
  {
    m_cycles += COST_REGISTER_LOAD;

    if constexpr (is_preprocess)
    {
      LoadBPRegPreprocess(command, value, static_cast<int>(m_cycles));
    }
    else
    {
      LoadBPReg(command, value, static_cast<int>(m_cycles));
      INCSTAT(g_stats.this_frame.num_bp_loads);
    }
  }

  OPCODE_CALLBACK(void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size))
  {
    m_cycles += COST_SHORT_COMMAND;

    if constexpr (is_preprocess)
      PreprocessIndexedXF(array, index, address, size);
    else
      LoadIndexedXF(array, index, address, size);
  }

  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          u32 vertex_size, u16 num_vertices, const u8* vertex_data))
  {
    const u32 consumed = static_cast<u32>(
        VertexLoaderManager::RunVertices<is_preprocess>(vat, primitive, num_vertices, vertex_data));
    // The loader must agree with the size the decoder used to delimit the command.
    ASSERT(consumed == vertex_size * num_vertices);

    m_cycles += num_vertices * COST_PER_VERTEX + COST_SHORT_COMMAND;
  }

  // Not force-inlined: decoding the list re-enters Run with this same object.
  OPCODE_CALLBACK_NOINLINE(void OnDisplayList(u32 address, u32 size))
  {
    m_cycles += COST_SHORT_COMMAND;

    // Lists calling lists are not supported; the nested call is skipped.
    if (m_in_display_list)
    {
      WARN_LOG_FMT(VIDEO, "recursive display list detected");
      return;
    }

    m_in_display_list = true;
    if constexpr (is_preprocess)
      PreprocessDisplayList(address, size);
    else
      ExecuteDisplayList(address, size);
    m_in_display_list = false;
  }

  OPCODE_CALLBACK(void OnNop(u32 count)) { m_cycles += COST_SHORT_COMMAND * count; }

  OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8* data))
  {
    switch (static_cast<Opcode>(opcode))
    {
    case Opcode::GX_CMD_UNKNOWN_METRICS:
      // Issued by at least one game before it reads back the performance metrics.
      m_cycles += COST_SHORT_COMMAND;
      DEBUG_LOG_FMT(VIDEO, "GX 0x44");
      break;
    case Opcode::GX_CMD_INVL_VC:
      // Vertex cache invalidation; there is no cache to flush.
      m_cycles += COST_SHORT_COMMAND;
      DEBUG_LOG_FMT(VIDEO, "Invalidate (vertex cache?)");
      break;
    default:
      Core::System::GetInstance().GetCommandProcessor().HandleUnknownOpcode(opcode, data,
                                                                            is_preprocess);
      m_cycles += 1;
      break;
    }
  }

  OPCODE_CALLBACK(void OnCommand(const u8* data, u32 size))
  {
    ASSERT(size >= 1);
    // A DL call is not recorded itself: its contents pass through this callback as they run and
    // are recorded command by command instead.
    if constexpr (!is_preprocess)
    {
      if (g_record_fifo_data && data[0] != static_cast<u8>(Opcode::GX_CMD_CALL_DL))
        Core::System::GetInstance().GetFifoRecorder().WriteGPCommand(data, size);
    }
  }

  OPCODE_CALLBACK(CPState& GetCPState())
  {
    return is_preprocess ? g_preprocess_cp_state : g_main_cp_state;
  }

  OPCODE_CALLBACK(u32 GetVertexSize(u8 vat))
  {
    return VertexLoaderManager::RefreshLoader<is_preprocess>(vat)->m_vertex_size;
  }

  u32 m_cycles = 0;
  bool m_in_display_list = false;

private:
  // Preprocess pass: only remember which vertex formats need rebuilding.
  static void MarkPreprocessFormatsDirty(u8 command)
  {
    const u8 sub_command = command & CP_COMMAND_MASK;
    if (IsVertexDescriptor(sub_command))
      VertexLoaderManager::g_preprocess_vat_dirty = BitSet8::AllTrue(CP_NUM_VAT_REG);
    else if (IsVatRegister(sub_command))
      VertexLoaderManager::g_preprocess_vat_dirty[command & CP_VAT_MASK] = true;
  }

  // Main pass: invalidate whatever the write affects before the CP state changes.
  static void NoteMainCPWrite(u8 command, u32 value)
  {
    namespace VLM = VertexLoaderManager;
    const u8 sub_command = command & CP_COMMAND_MASK;

    if (sub_command == MATINDEX_A || sub_command == MATINDEX_B)
    {
      VLM::g_needs_cp_xf_consistency_check = true;
      XFStateManager& xf_state = Core::System::GetInstance().GetXFStateManager();
      if (sub_command == MATINDEX_A)
        xf_state.SetTexMatrixChangedA(value);
      else
        xf_state.SetTexMatrixChangedB(value);
    }
    else if (IsVertexDescriptor(sub_command))
    {
      VLM::g_main_vat_dirty = BitSet8::AllTrue(CP_NUM_VAT_REG);
      VLM::g_bases_dirty = true;
      VLM::g_needs_cp_xf_consistency_check = true;
    }
    else if (IsVatRegister(sub_command))
    {
      VLM::g_main_vat_dirty[command & CP_VAT_MASK] = true;
      VLM::g_needs_cp_xf_consistency_check = true;
    }
    else if (sub_command == ARRAY_BASE)
    {
      VLM::g_bases_dirty = true;
    }

    INCSTAT(g_stats.this_frame.num_cp_loads);
  }

  // Preprocess pass: hand the list's bytes to the GPU thread (a null pointer still records the
  // size so both sides stay in step), then pre-parse them.
  void PreprocessDisplayList(u32 address, u32 size)
  {
    Core::System& system = Core::System::GetInstance();
    const u8* const list = system.GetMemory().GetPointerForRange(address, size);
    system.GetFifo().PushFifoAuxBuffer(list, size);
    if (list != nullptr)
      Run(list, size, *this);
  }

  // Main pass: take the bytes captured by the preprocess pass in deterministic mode, otherwise
  // read them from RAM, and run them. Unmapped lists are ignored.
  void ExecuteDisplayList(u32 address, u32 size)
  {
    Core::System& system = Core::System::GetInstance();
    Fifo::FifoManager& fifo = system.GetFifo();
    const u8* const list = fifo.UseDeterministicGPUThread() ?
                               static_cast<const u8*>(fifo.PopFifoAuxBuffer(size)) :
                               system.GetMemory().GetPointerForRange(address, size);
    if (list == nullptr)
      return;

    // Count the list's contents under the display-list statistics.
    g_stats.SwapDL();
    Run(list, size, *this);
    INCSTAT(g_stats.this_frame.num_dlists_called);
    g_stats.SwapDL();
  }
};

template <bool is_preprocess>
u8* RunFifo(DataReader src, u32* cycles)
{
  RunCallback<is_preprocess> callback;
  const u32 consumed = Run(src.GetPointer(), static_cast<u32>(src.size()), callback);
  if (cycles)
    *cycles = callback.m_cycles;
  src.Skip(consumed);
  return src.GetPointer();
}

template u8* RunFifo<true>(DataReader src, u32* cycles);
template u8* RunFifo<false>(DataReader src, u32* cycles);

}  // namespace OpcodeDecoder
