// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/SetupUnit.h"

#include "Common/Logging/Log.h"
#include "VideoBackends/Software/Clipper.h"
#include "VideoCommon/OpcodeDecoding.h"

using OpcodeDecoder::Primitive;

void SetupUnit::Init(Primitive primitive_type)
{
  m_primitive = primitive_type;
  m_received = 0;
}

// Which of the three slots vertex number `index` of the run is stored in. Each scheme keeps
// every vertex a later primitive still needs.
std::size_t SetupUnit::SlotFor(u32 index) const
{
  switch (m_primitive)
  {
  case Primitive::GX_DRAW_QUADS:
  case Primitive::GX_DRAW_QUADS_2:
  {
    // The fourth corner replaces the second, which the quad's second triangle doesn't use.
    const u32 corner = index % 4;
    return corner == 3 ? 1 : corner;
  }
  case Primitive::GX_DRAW_TRIANGLE_FAN:
    // The centre keeps slot 0; the rim alternates between 1 and 2.
    return index == 0 ? 0 : 1 + (index - 1) % 2;
  case Primitive::GX_DRAW_LINES:
  case Primitive::GX_DRAW_LINE_STRIP:
    return index % 2;
  case Primitive::GX_DRAW_POINTS:
    return 0;
  case Primitive::GX_DRAW_TRIANGLES:
  case Primitive::GX_DRAW_TRIANGLE_STRIP:
  default:
    return index % 3;
  }
}

OutputVertexData* SetupUnit::GetVertex()
{
  OutputVertexData* vertex = Slot(m_received);
  *vertex = OutputVertexData{};
  return vertex;
}

void SetupUnit::SetupVertex()
{
  const u32 n = m_received++;

  switch (m_primitive)
  {
  case Primitive::GX_DRAW_QUADS_2:
    WARN_LOG_FMT(VIDEO, "Non-standard primitive drawing command GL_DRAW_QUADS_2");
    [[fallthrough]];
  case Primitive::GX_DRAW_QUADS:
    // Quad a b c d is drawn as a b c and a c d.
    if (n % 4 == 2)
      Clipper::ProcessTriangle(Slot(n - 2), Slot(n - 1), Slot(n));
    else if (n % 4 == 3)
      Clipper::ProcessTriangle(Slot(n - 3), Slot(n - 1), Slot(n));
    break;

  case Primitive::GX_DRAW_TRIANGLES:
    if (n % 3 == 2)
      Clipper::ProcessTriangle(Slot(n - 2), Slot(n - 1), Slot(n));
    break;

  case Primitive::GX_DRAW_TRIANGLE_STRIP:
    // Every other triangle swaps its last two vertices to keep the strip's winding.
    if (n >= 2)
    {
      const u32 first = n - 2;
      if (first % 2 == 0)
        Clipper::ProcessTriangle(Slot(first), Slot(first + 1), Slot(n));
      else
        Clipper::ProcessTriangle(Slot(first), Slot(n), Slot(first + 1));
    }
    break;

  case Primitive::GX_DRAW_TRIANGLE_FAN:
    if (n >= 2)
      Clipper::ProcessTriangle(Slot(0), Slot(n - 1), Slot(n));
    break;

  case Primitive::GX_DRAW_LINES:
    if (n % 2 == 1)
      Clipper::ProcessLine(Slot(n - 1), Slot(n));
    break;

  case Primitive::GX_DRAW_LINE_STRIP:
    if (n >= 1)
      Clipper::ProcessLine(Slot(n - 1), Slot(n));
    break;

  case Primitive::GX_DRAW_POINTS:
    Clipper::ProcessPoint(Slot(n));
    break;
  }
}
