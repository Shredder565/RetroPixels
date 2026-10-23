// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Model.cpp

#include "Model.h"

namespace New3D {

namespace
{
  constexpr size_t kMaxSavedLevels = 128;
}

NodeStateStack::NodeStateStack()
{
  Reset();
}

bool NodeStateStack::Push()
{
  if (m_saved.size() >= kMaxSavedLevels)
    return false;
  m_saved.push_back({ texOffsetX, texOffsetY, page, modelScale, modelAlpha, disableCulling });
  return true;
}

bool NodeStateStack::Pop()
{
  if (m_saved.empty())
    return false;
  const Saved &s = m_saved.back();
  texOffsetX = s.texOffsetX;
  texOffsetY = s.texOffsetY;
  page = s.page;
  modelScale = s.modelScale;
  modelAlpha = s.modelAlpha;
  disableCulling = s.disableCulling;
  m_saved.pop_back();
  return true;
}

void NodeStateStack::Reset()
{
  texOffsetX = 0;
  texOffsetY = 0;
  page = 0;
  modelScale = 1.0f;
  modelAlpha = 1.0f;
  disableCulling = false;
  m_saved.clear();
}

} // New3D
