// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"

// Something a graphics mod does when one of its targets is hit. Each hook receives the event
// data it may inspect or change; the defaults do nothing.
class GraphicsModAction
{
public:
  GraphicsModAction() = default;
  virtual ~GraphicsModAction() = default;
  GraphicsModAction(const GraphicsModAction&) = default;
  GraphicsModAction(GraphicsModAction&&) = default;
  GraphicsModAction& operator=(const GraphicsModAction&) = default;
  GraphicsModAction& operator=(GraphicsModAction&&) = default;

  virtual void OnDrawStarted(GraphicsModActionData::DrawStarted*) {}
  virtual void BeforeEFB(GraphicsModActionData::PreEFB*) {}
  virtual void AfterEFB(GraphicsModActionData::PostEFB*) {}
  virtual void BeforeXFB() {}
  virtual void OnProjection(GraphicsModActionData::Projection*) {}
  virtual void OnProjectionAndTexture(GraphicsModActionData::Projection*) {}
  virtual void OnTextureLoad(GraphicsModActionData::TextureLoad*) {}
  virtual void OnTextureCreate(GraphicsModActionData::TextureCreate*) {}
  virtual void OnFrameEnd() {}
};
