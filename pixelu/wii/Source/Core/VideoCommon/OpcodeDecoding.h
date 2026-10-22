// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <concepts>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Common/Inline.h"
#include "Common/Swap.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoaderBase.h"

struct CPState;
class DataReader;

// Splits a GX command stream into commands and hands each one to a callback object.
namespace OpcodeDecoder
{
// Set while the FIFO recorder is capturing; the main decode pass then forwards every command.
extern bool g_record_fifo_data;

// First byte of each GX command.
enum class Opcode
{
  GX_NOP = 0x00,

  GX_LOAD_BP_REG = 0x61,
  GX_LOAD_CP_REG = 0x08,
  GX_LOAD_XF_REG = 0x10,
  GX_LOAD_INDX_A = 0x20,
  GX_LOAD_INDX_B = 0x28,
  GX_LOAD_INDX_C = 0x30,
  GX_LOAD_INDX_D = 0x38,

  GX_CMD_CALL_DL = 0x40,
  GX_CMD_UNKNOWN_METRICS = 0x44,
  GX_CMD_INVL_VC = 0x48,

  GX_PRIMITIVE_START = 0x80,
  GX_PRIMITIVE_END = 0xbf,
};

// A primitive opcode is 1ppppvvv: p selects the primitive type, v the vertex attribute table.
constexpr u8 GX_PRIMITIVE_MASK = 0x78;
constexpr u32 GX_PRIMITIVE_SHIFT = 3;
constexpr u8 GX_VAT_MASK = 0x07;

// Primitive type field of a primitive opcode (the opcode byte is given alongside each).
// QUADS_2 is not used by the SDK but draws like QUADS.
enum class Primitive : u8
{
  GX_DRAW_QUADS = 0x0,           // 0x80
  GX_DRAW_QUADS_2 = 0x1,         // 0x88
  GX_DRAW_TRIANGLES = 0x2,       // 0x90
  GX_DRAW_TRIANGLE_STRIP = 0x3,  // 0x98
  GX_DRAW_TRIANGLE_FAN = 0x4,    // 0xA0
  GX_DRAW_LINES = 0x5,           // 0xA8
  GX_DRAW_LINE_STRIP = 0x6,      // 0xB0
  GX_DRAW_POINTS = 0x7           // 0xB8
};

// Callback objects implement the interface below. Run/RunCommand are templates over the concrete
// callback type, so calls are resolved statically and can be inlined; the virtual base mainly
// documents and enforces the required set of handlers. Undefining
// OPCODE_CALLBACK_USE_INHERITANCE drops the virtual functions (useful to get inlining
// diagnostics), at the cost of less helpful errors for a missing handler.
#define OPCODE_CALLBACK_USE_INHERITANCE

#ifdef OPCODE_CALLBACK_USE_INHERITANCE
#define OPCODE_CALLBACK(sig) DOLPHIN_FORCE_INLINE sig override
#define OPCODE_CALLBACK_NOINLINE(sig) sig override
#else
#define OPCODE_CALLBACK(sig) DOLPHIN_FORCE_INLINE sig
#define OPCODE_CALLBACK_NOINLINE(sig) sig
#endif
class Callback
{
#ifdef OPCODE_CALLBACK_USE_INHERITANCE
public:
  virtual ~Callback() = default;

  // An XF register block write: `count` big-endian words starting at `address`.
  virtual void OnXF(u16 address, u8 count, const u8* data) = 0;
  // A CP register write. Implementations must apply it to GetCPState() (LoadCPReg) or later
  // primitives will be sized wrongly.
  virtual void OnCP(u8 command, u32 value) = 0;
  // A BP register write (24-bit value).
  virtual void OnBP(u8 command, u32 value) = 0;
  // An indexed XF load from one of the XF_A..XF_D arrays.
  virtual void OnIndexedLoad(CPArray array, u32 index, u16 address, u8 size) = 0;
  // A draw command together with its vertex data.
  virtual void OnPrimitiveCommand(OpcodeDecoder::Primitive primitive, u8 vat, u32 vertex_size,
                                  u16 num_vertices, const u8* vertex_data) = 0;
  // A display list call (address and size already rounded down to 32 bytes).
  virtual void OnDisplayList(u32 address, u32 size) = 0;
  // A run of NOP bytes, reported once for the whole run.
  virtual void OnNop(u32 count) = 0;
  // Any opcode not handled above; data[0] is the opcode byte.
  virtual void OnUnknown(u8 opcode, const u8* data) = 0;

  // Follows every successfully decoded command, whatever its kind. data[0] is the opcode and
  // size is the full command length (always at least 1).
  virtual void OnCommand(const u8* data, u32 size) = 0;

  // CP state used to size vertices; CP commands update it.
  virtual CPState& GetCPState() = 0;

  virtual u32 GetVertexSize(u8 vat)
  {
    const CPState& state = GetCPState();
    return VertexLoaderBase::GetVertexSize(state.vtx_desc, state.vtx_attr[vat]);
  }
#endif
};

namespace detail
{
// Decodes the command at `data` and returns its length, or 0 when `available` bytes are not
// enough to hold all of it (nothing is reported in that case). OnCommand is left to the caller.
static DOLPHIN_FORCE_INLINE u32 RunCommand(const u8* data, u32 available,
                                           std::derived_from<Callback> auto& callback)
{
  if (available == 0)
    return 0;

  const u8 opcode = data[0];

  if (opcode >= static_cast<u8>(Opcode::GX_PRIMITIVE_START) &&
      opcode <= static_cast<u8>(Opcode::GX_PRIMITIVE_END))
  {
    constexpr u32 header = 3;  // opcode + 16-bit vertex count
    if (available < header)
      return 0;
    const auto type = static_cast<Primitive>((opcode & GX_PRIMITIVE_MASK) >> GX_PRIMITIVE_SHIFT);
    const u8 vat = opcode & GX_VAT_MASK;
    // The vertex size is needed (and queried) before knowing whether the data is all here.
    const u32 stride = callback.GetVertexSize(vat);
    const u16 vertex_count = Common::swap16(data + 1);
    const u32 total = header + vertex_count * stride;
    if (available < total)
      return 0;
    callback.OnPrimitiveCommand(type, vat, stride, vertex_count, data + header);
    return total;
  }

  switch (static_cast<Opcode>(opcode))
  {
  case Opcode::GX_NOP:
  {
    u32 run = 1;
    while (run != available && data[run] == static_cast<u8>(Opcode::GX_NOP))
      ++run;
    callback.OnNop(run);
    return run;
  }

  case Opcode::GX_LOAD_BP_REG:
    if (available < 5)
      return 0;
    callback.OnBP(data[1], Common::swap24(data + 2));
    return 5;

  case Opcode::GX_LOAD_CP_REG:
    if (available < 6)
      return 0;
    callback.OnCP(data[1], Common::swap32(data + 2));
    return 6;

  case Opcode::GX_LOAD_XF_REG:
  {
    if (available < 5)
      return 0;
    // Header word: high half is (word count - 1), low half the first register.
    const u32 header = Common::swap32(data + 1);
    const u16 count_field = static_cast<u16>(header >> 16);
    ASSERT_MSG(VIDEO, count_field < 16, "cmd2 = 0x{:08X}", header);
    const u8 words = static_cast<u8>((count_field & 0xf) + 1);
    const u32 total = 5 + u32{words} * 4;
    if (available < total)
      return 0;
    callback.OnXF(static_cast<u16>(header & 0xffff), words, data + 5);
    return total;
  }

  case Opcode::GX_LOAD_INDX_A:
  case Opcode::GX_LOAD_INDX_B:
  case Opcode::GX_LOAD_INDX_C:
  case Opcode::GX_LOAD_INDX_D:
  {
    if (available < 5)
      return 0;
    // Word layout: index (16 bits) | length - 1 (4 bits) | XF address (12 bits).
    const u32 word = Common::swap32(data + 1);
    // 0x20, 0x28, 0x30, 0x38 select arrays 12..15 (XF_A..XF_D).
    const auto array = static_cast<CPArray>(8 + (opcode >> 3));
    callback.OnIndexedLoad(array, word >> 16, static_cast<u16>(word & 0xFFF),
                           static_cast<u8>(((word >> 12) & 0xF) + 1));
    return 5;
  }

  case Opcode::GX_CMD_CALL_DL:
  {
    if (available < 9)
      return 0;
    // The hardware ignores the low five bits of both fields.
    constexpr u32 align = ~u32{31};
    callback.OnDisplayList(Common::swap32(data + 1) & align, Common::swap32(data + 5) & align);
    return 9;
  }

  default:
    break;
  }

  callback.OnUnknown(opcode, data);
  return 1;
}
}  // namespace detail

// Decodes one command, then reports it through OnCommand. Returns its length, or 0 if it is
// incomplete.
DOLPHIN_FORCE_INLINE u32 RunCommand(const u8* data, u32 available,
                                    std::derived_from<Callback> auto& callback)
{
  const u32 length = detail::RunCommand(data, available, callback);
  if (length != 0)
    callback.OnCommand(data, length);
  return length;
}

// Decodes commands until the data runs out or the next command is incomplete. Returns the
// number of bytes consumed.
DOLPHIN_FORCE_INLINE u32 Run(const u8* data, u32 available,
                             std::derived_from<Callback> auto& callback)
{
  u32 done = 0;
  for (;;)
  {
    if (done >= available)
      break;
    const u32 length = RunCommand(data + done, available - done, callback);
    if (length == 0)
      break;
    done += length;
  }
  return done;
}

// Executes (or, with is_preprocess, only pre-parses) the commands in `src`. Returns a pointer just
// past the last complete command; the cycle estimate goes to `cycles` when it is non-null.
template <bool is_preprocess = false>
u8* RunFifo(DataReader src, u32* cycles);

}  // namespace OpcodeDecoder

template <>
struct fmt::formatter<OpcodeDecoder::Primitive>
    : EnumFormatter<OpcodeDecoder::Primitive::GX_DRAW_POINTS>
{
  static constexpr array_type names = {
      "GX_DRAW_QUADS",
      "GX_DRAW_QUADS_2 (nonstandard)",
      "GX_DRAW_TRIANGLES",
      "GX_DRAW_TRIANGLE_STRIP",
      "GX_DRAW_TRIANGLE_FAN",
      "GX_DRAW_LINES",
      "GX_DRAW_LINE_STRIP",
      "GX_DRAW_POINTS",
  };
  constexpr formatter() : EnumFormatter(names) {}
};
