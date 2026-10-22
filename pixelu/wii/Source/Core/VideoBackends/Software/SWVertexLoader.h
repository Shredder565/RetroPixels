// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

#include "VideoBackends/Software/NativeVertexFormat.h"
#include "VideoBackends/Software/SetupUnit.h"

#include "VideoCommon/VertexManagerBase.h"

struct PortableVertexDeclaration;

// The software renderer's vertex manager: takes the batches VideoCommon's vertex loaders build
// and runs each indexed vertex through the transform unit and primitive setup.
class SWVertexLoader final : public VertexManagerBase
{
public:
  SWVertexLoader();
  ~SWVertexLoader() override;

  DataReader PrepareForAdditionalData(OpcodeDecoder::Primitive primitive, u32 count, u32 stride,
                                      bool cullall) override;

protected:
  void DrawCurrentBatch(u32 base_index, u32 num_indices, u32 base_vertex) override;

  // Loads the XF matrix indices into m_vertex (vertex data may override them).
  void SetFormat();
  // Decodes vertex number `index` of the batch into m_vertex.
  void ParseVertex(const PortableVertexDeclaration& vdec, int index);

  InputVertexData m_vertex{};
  SetupUnit m_setup_unit;
};
