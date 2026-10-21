// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"

#include "VideoCommon/BoundingBox.h"

namespace OGL
{
// The bounding box lives in a small shader storage buffer at binding 0 that pixel shaders
// update with atomics.
class OGLBoundingBox final : public BoundingBox
{
public:
  ~OGLBoundingBox() override;

  bool Initialize() override;

protected:
  std::vector<BBoxType> Read(u32 index, u32 length) override;
  void Write(u32 index, std::span<const BBoxType> values) override;

private:
  GLuint m_ssbo = 0;
};

}  // namespace OGL
