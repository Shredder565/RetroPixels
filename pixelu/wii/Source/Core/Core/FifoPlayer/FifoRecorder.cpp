// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/FifoPlayer/FifoRecorder.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoader_Color.h"
#include "VideoCommon/VertexLoader_Normal.h"
#include "VideoCommon/VertexLoader_Position.h"
#include "VideoCommon/VertexLoader_TextCoord.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/XFMemory.h"

// Watches the recorded commands for the main-memory data they make the GPU read (indexed vertex
// arrays and XF loads), so that the recorder can store that memory in the log.
class FifoRecorder::FifoRecordAnalyzer final : public OpcodeDecoder::Callback
{
public:
  FifoRecordAnalyzer(FifoRecorder* owner, const u32* cp_registers)
      : m_owner(*owner), m_cpmem(cp_registers)
  {
  }

  OPCODE_CALLBACK(void OnXF(u16, u8, const u8*)) {}
  OPCODE_CALLBACK(void OnCP(u8 command, u32 value)) { GetCPState().LoadCPReg(command, value); }
  OPCODE_CALLBACK(void OnBP(u8, u32)) {}
  OPCODE_CALLBACK(void OnIndexedLoad(CPArray array, u32 index, u16, u8 size))
  {
    // An XF load from an array element of `size` words.
    const u32 element = m_cpmem.array_bases[array] + m_cpmem.array_strides[array] * index;
    m_owner.UseMemory(element, static_cast<u32>(size) * 4, MemoryUpdate::Type::XFData);
  }
  OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat,
                                          u32 vertex_size, u16 num_vertices,
                                          const u8* vertex_data));
  OPCODE_CALLBACK(void OnDisplayList(u32 address, u32 size))
  {
    // The recorder expects display lists to have been expanded into the FIFO already.
    WARN_LOG_FMT(VIDEO,
                 "Unhandled display list call {:08x} {:08x}; should have been inlined earlier",
                 address, size);
  }
  OPCODE_CALLBACK(void OnNop(u32)) {}
  OPCODE_CALLBACK(void OnUnknown(u8, const u8*)) {}
  OPCODE_CALLBACK(void OnCommand(const u8*, u32)) {}
  OPCODE_CALLBACK(CPState& GetCPState()) { return m_cpmem; }

private:
  // The vertices of one primitive command, walked component by component.
  struct VertexStream
  {
    const u8* data;
    u32 stride;  // bytes per vertex
    u16 count;
  };

  // For an indexed component at `offset` within each vertex, records the part of its array
  // that the vertices reference: up to the highest index, plus one element of `element_size`.
  // `array_offset` shifts the start (for the extra vectors of 3-index normals).
  void RecordArrayUse(const VertexStream& vertices, CPArray array, VertexComponentFormat format,
                      u32 offset, u32 element_size, u32 array_offset = 0);

  FifoRecorder& m_owner;
  CPState m_cpmem;
};

void FifoRecorder::FifoRecordAnalyzer::RecordArrayUse(const VertexStream& vertices, CPArray array,
                                                      VertexComponentFormat format, u32 offset,
                                                      u32 element_size, u32 array_offset)
{
  if (!IsIndexed(format))
    return;

  // The all-ones index means "no vertex" and doesn't count.
  const bool wide = format == VertexComponentFormat::Index16;
  const u16 skip_index = wide ? 0xffff : 0xff;
  u16 highest = 0;
  const u8* field = vertices.data + offset;
  for (u16 n = 0; n < vertices.count; ++n, field += vertices.stride)
  {
    const u16 index = wide ? Common::swap16(field) : *field;
    if (index != skip_index)
      highest = std::max(highest, index);
  }

  const u32 start = m_cpmem.array_bases[array] + array_offset;
  const u32 length = m_cpmem.array_strides[array] * highest + element_size;
  m_owner.UseMemory(start, length, MemoryUpdate::Type::VertexStream);
}

void FifoRecorder::FifoRecordAnalyzer::OnPrimitiveCommand(OpcodeDecoder::Primitive, u8 vat,
                                                          u32 vertex_size, u16 num_vertices,
                                                          const u8* vertex_data)
{
  // Same component order and sizes as the vertex loaders. For indexed components the size in
  // the vertex is the index's, while the array element has the size of the direct form.
  const TVtxDesc& desc = m_cpmem.vtx_desc;
  const VAT& attributes = m_cpmem.vtx_attr[vat];
  const VertexStream vertices{vertex_data, vertex_size, num_vertices};
  constexpr VertexComponentFormat DIRECT = VertexComponentFormat::Direct;

  // Matrix indices come first, one byte each; they never reference memory.
  u32 offset = desc.low.PosMatIdx ? 1 : 0;
  for (const bool has_index : desc.low.TexMatIdx)
    offset += has_index ? 1 : 0;

  {
    const ComponentFormat format = attributes.g0.PosFormat;
    const CoordComponentCount elements = attributes.g0.PosElements;
    RecordArrayUse(vertices, CPArray::Position, desc.low.Position, offset,
                   VertexLoader_Position::GetSize(DIRECT, format, elements));
    offset += VertexLoader_Position::GetSize(desc.low.Position, format, elements);
  }

  {
    const VertexComponentFormat stored = desc.low.Normal;
    const ComponentFormat format = attributes.g0.NormalFormat;
    const NormalComponentCount elements = attributes.g0.NormalElements;
    const bool index3 = attributes.g0.NormalIndex3;
    const u32 stored_size = VertexLoader_Normal::GetSize(stored, format, elements, index3);

    if (index3 && IsIndexed(stored) && elements == NormalComponentCount::NTB)
    {
      // Three indices, for the normal, tangent and binormal. They all index the same array; the
      // tangent and binormal are read one and two vectors further on (a byte offset, so that
      // three equal indices behave like a single one whatever the array stride).
      const u32 index_size = stored == VertexComponentFormat::Index16 ? 2 : 1;
      ASSERT(stored_size == index_size * 3);
      const u32 vector_size = GetElementSize(format) * 3;
      for (u32 vector = 0; vector < 3; ++vector)
      {
        RecordArrayUse(vertices, CPArray::Normal, stored, offset + vector * index_size, vector_size,
                       vector * vector_size);
      }
    }
    else
    {
      RecordArrayUse(vertices, CPArray::Normal, stored, offset,
                     VertexLoader_Normal::GetSize(DIRECT, format, elements, index3));
    }
    offset += stored_size;
  }

  for (u32 channel = 0; channel < desc.low.Color.Size(); ++channel)
  {
    const ColorFormat format = attributes.GetColorFormat(channel);
    const VertexComponentFormat stored = desc.low.Color[channel];
    RecordArrayUse(vertices, CPArray::Color0 + static_cast<u8>(channel), stored, offset,
                   VertexLoader_Color::GetSize(DIRECT, format));
    offset += VertexLoader_Color::GetSize(stored, format);
  }

  for (u32 coord = 0; coord < desc.high.TexCoord.Size(); ++coord)
  {
    const ComponentFormat format = attributes.GetTexFormat(coord);
    const TexComponentCount elements = attributes.GetTexElements(coord);
    const VertexComponentFormat stored = desc.high.TexCoord[coord];
    RecordArrayUse(vertices, CPArray::TexCoord0 + static_cast<u8>(coord), stored, offset,
                   VertexLoader_TextCoord::GetSize(DIRECT, format, elements));
    offset += VertexLoader_TextCoord::GetSize(stored, format, elements);
  }

  ASSERT(offset == vertex_size);
}

// FifoRecorder ------------------------------------------------------------------------------
//
// Recording runs in three steps: StartRecording (UI thread) arms it, the first frame end seen
// afterwards snapshots the GPU state and starts collecting commands, and every frame end after
// that closes a frame (the commands of a frame arrive before its end is signalled, so a frame is
// stored with the first command written after its end).

FifoRecorder::FifoRecorder(Core::System& system) : m_system(system)
{
}

FifoRecorder::~FifoRecorder() = default;

void FifoRecorder::StartRecording(s32 numFrames, CallbackFunc finishedCb)
{
  std::lock_guard lock(m_mutex);

  m_File = std::make_unique<FifoDataFile>();
  m_File->SetIsWii(m_system.IsWii());

  // Shadow copies of RAM: what the log already contains. They stay allocated after recording,
  // since the video thread may still look at them while the end is being processed.
  const Memory::MemoryManager& memory = m_system.GetMemory();
  m_Ram.assign(memory.GetRamSize(), 0);
  m_ExRam.assign(memory.GetExRamSize(), 0);

  // A request while already recording only replaces the callback (and cancels a pending stop).
  if (!m_IsRecording)
  {
    m_IsRecording = true;
    m_WasRecording = false;
    m_RecordFramesRemaining = numFrames;
  }
  m_RequestedRecordingEnd = false;
  m_FinishedCb = std::move(finishedCb);

  m_end_of_frame_event =
      m_system.GetVideoEvents().after_frame_event.Register([this](const Core::System& system) {
        const bool already_recording = OpcodeDecoder::g_record_fifo_data;
        OpcodeDecoder::g_record_fifo_data = IsRecording();
        if (!OpcodeDecoder::g_record_fifo_data)
        {
          // Done: stop listening for frame ends.
          m_end_of_frame_event.reset();
          return;
        }

        if (!already_recording)
          RecordInitialVideoMemory();

        const auto& fifo = system.GetCommandProcessor().GetFifo();
        EndFrame(fifo.CPBase.load(std::memory_order_relaxed),
                 fifo.CPEnd.load(std::memory_order_relaxed));
      });
}

void FifoRecorder::StopRecording()
{
  std::lock_guard lock(m_mutex);
  m_RequestedRecordingEnd = true;
}

bool FifoRecorder::IsRecording() const
{
  return m_IsRecording;
}

bool FifoRecorder::IsRecordingDone() const
{
  return m_WasRecording && m_File != nullptr;
}

FifoDataFile* FifoRecorder::GetRecordedFile() const
{
  return m_File.get();
}

void FifoRecorder::RecordInitialVideoMemory()
{
  // The log keeps XF memory and the XF registers that follow it as separate blocks.
  const u32* const xf_words = reinterpret_cast<const u32*>(&xfmem);
  constexpr u32 xf_register_count = sizeof(XFMemory) / sizeof(u32) - FifoDataFile::XF_MEM_SIZE;

  u32 cp_registers[256] = {};
  g_main_cp_state.FillCPMemoryArray(cp_registers);

  SetVideoMemory(reinterpret_cast<const u32*>(&bpmem), cp_registers, xf_words,
                 xf_words + FifoDataFile::XF_MEM_SIZE, xf_register_count, s_tex_mem.data());
}

void FifoRecorder::SetVideoMemory(const u32* bpMem, const u32* cpMem, const u32* xfMem,
                                  const u32* xfRegs, u32 xfRegsSize, const u8* texMem)
{
  std::lock_guard lock(m_mutex);

  if (m_File != nullptr)
  {
    const auto copy_words = [](u32* to, const u32* from, u32 count) {
      std::memcpy(to, from, count * sizeof(u32));
    };
    copy_words(m_File->GetBPMem(), bpMem, FifoDataFile::BP_MEM_SIZE);
    copy_words(m_File->GetCPMem(), cpMem, FifoDataFile::CP_MEM_SIZE);
    copy_words(m_File->GetXFMem(), xfMem, FifoDataFile::XF_MEM_SIZE);
    copy_words(m_File->GetXFRegs(), xfRegs,
               std::min(static_cast<u32>(FifoDataFile::XF_REGS_SIZE), xfRegsSize));
    std::memcpy(m_File->GetTexMem(), texMem, FifoDataFile::TEX_MEM_SIZE);
  }

  // The analyzer starts from the same CP state as the log.
  m_record_analyzer = std::make_unique<FifoRecordAnalyzer>(this, cpMem);
}

void FifoRecorder::WriteGPCommand(const u8* data, u32 size)
{
  if (!m_SkipNextData)
  {
    // Decoding the command lets the analyzer record the memory it uses (via UseMemory), and
    // checks that the player will split the data into the same commands.
    const u32 decoded_size = OpcodeDecoder::RunCommand(data, size, *m_record_analyzer);
    if (decoded_size != size)
    {
      PanicAlertFmt("FifoRecorder: Expected command to be {} bytes long, we were given {} bytes",
                    decoded_size, size);
    }
    m_FifoData.insert(m_FifoData.end(), data, data + size);
  }

  // A frame whose end was signalled is complete now.
  if (m_FrameEnded && !m_FifoData.empty())
  {
    m_CurrentFrame.fifoData = m_FifoData;
    {
      std::lock_guard lock(m_mutex);
      m_File->AddFrame(m_CurrentFrame);
      if (m_RequestedRecordingEnd && m_FinishedCb)
        m_FinishedCb();
    }
    m_CurrentFrame.memoryUpdates.clear();
    m_FifoData.clear();
    m_FrameEnded = false;
  }

  m_SkipNextData = m_SkipFutureData;
}

void FifoRecorder::UseMemory(u32 address, u32 size, MemoryUpdate::Type type, bool dynamicUpdate)
{
  // Bit 28 selects MEM2; the rest is wrapped into the size of that RAM.
  Memory::MemoryManager& memory = m_system.GetMemory();
  const bool in_mem2 = (address & 0x10000000) != 0;
  const u32 offset = address & (in_mem2 ? memory.GetExRamMask() : memory.GetRamMask());
  u8* const recorded = in_mem2 ? &m_ExRam[offset] : &m_Ram[offset];
  const u8* const current = in_mem2 ? &memory.GetEXRAM()[offset] : &memory.GetRAM()[offset];

  if (dynamicUpdate)
  {
    // Written by the GPU itself (EFB copies): playback recreates it, so only remember it.
    std::memcpy(recorded, current, size);
    return;
  }
  if (std::memcmp(recorded, current, size) == 0)
    return;

  std::memcpy(recorded, current, size);
  MemoryUpdate update;
  update.fifoPosition = static_cast<u32>(m_FifoData.size());
  update.address = address;
  update.type = type;
  update.data.assign(current, current + size);
  m_CurrentFrame.memoryUpdates.push_back(std::move(update));
}

void FifoRecorder::EndFrame(u32 fifoStart, u32 fifoEnd)
{
  // Only called while recording.
  std::lock_guard lock(m_mutex);

  m_CurrentFrame.fifoStart = fifoStart;
  m_CurrentFrame.fifoEnd = fifoEnd;

  if (!m_WasRecording)
  {
    // The first frame end only starts the recording. The command that follows is the copy that
    // ended that frame, which belongs to no recorded frame.
    m_WasRecording = true;
    m_FrameEnded = false;
    m_SkipNextData = true;
    m_SkipFutureData = false;
    m_FifoData.reserve(4 * 1024 * 1024);
    m_FifoData.clear();
  }
  else
  {
    m_FrameEnded = true;
    // Count down a fixed-length recording.
    if (m_RecordFramesRemaining > 0 && --m_RecordFramesRemaining == 0)
      m_RequestedRecordingEnd = true;
  }

  if (m_RequestedRecordingEnd)
  {
    // Keep the next command (which completes this frame), drop everything after it, and tell
    // the video thread to stop signalling frame ends.
    m_SkipFutureData = true;
    m_IsRecording = false;
  }
}
