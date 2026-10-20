// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VideoState.h"

#include "Common/ChunkFile.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BPStructs.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/Widescreen.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

namespace
{
// Serialises one block of state and then stamps its marker, which lets a load detect where a
// mismatch began.
template <typename Fn>
void Section(PointerWrap& p, const char* marker, Fn&& serialise)
{
  serialise();
  p.DoMarker(marker);
}
}  // namespace

void VideoCommon_DoState(PointerWrap& p)
{
  // Leading flag: states written by the software renderer are not loadable here. Switching the
  // wrapper into verify mode makes the remainder a dry run so the load is rejected cleanly.
  bool from_software_renderer = false;
  p.Do(from_software_renderer);
  if (from_software_renderer && p.IsReadMode())
    p.SetVerifyMode();

  Core::System& system = Core::System::GetInstance();

  // Register files. Only the CPU-side CP state is stored: the GPU is synchronised around
  // savestates, so the preprocessing copy can be rebuilt from it.
  Section(p, "BP Memory", [&] { p.Do(bpmem); });
  Section(p, "CP Memory", [&] { p.Do(g_main_cp_state); });
  // (The mode is re-queried each time: a failed read switches the wrapper out of read mode.)
  if (p.IsReadMode())
    CopyPreprocessCPStateFromMain();
  Section(p, "XF Memory", [&] { p.Do(xfmem); });

  // Texture memory, both the decoder's view and the TMEM cache bookkeeping.
  Section(p, "texMem", [&] { p.DoArray(s_tex_mem); });
  Section(p, "TMEM", [&] { TMEM::DoState(p); });

  // Command pipeline and pixel engine.
  Section(p, "Fifo", [&] { system.GetFifo().DoState(p); });
  Section(p, "CommandProcessor", [&] { system.GetCommandProcessor().DoState(p); });
  Section(p, "PixelEngine", [&] { system.GetPixelEngine().DoState(p); });

  // Shader constant managers keep their own copies; replaying BP writes would not restore them.
  Section(p, "PixelShaderManager", [&] { system.GetPixelShaderManager().DoState(p); });
  Section(p, "VertexShaderManager", [&] { system.GetVertexShaderManager().DoState(p); });
  Section(p, "GeometryShaderManager", [&] { system.GetGeometryShaderManager().DoState(p); });

  // Backend-side objects.
  Section(p, "VertexManager", [&] { g_vertex_manager->DoState(p); });
  Section(p, "FramebufferManager", [&] { g_framebuffer_manager->DoState(p); });
  Section(p, "TextureCache", [&] { g_texture_cache->DoState(p); });
  Section(p, "Presenter", [&] {
    g_presenter->DoState(p);
    g_frame_dumper->DoState(p);
  });
  Section(p, "Bounding Box", [&] { g_bounding_box->DoState(p); });
  Section(p, "Widescreen", [&] { g_widescreen->DoState(p); });
  Section(p, "XFStateManager", [&] { system.GetXFStateManager().DoState(p); });

  if (!p.IsReadMode())
    return;

  // Push the restored registers back out to the backend and force vertex loaders to rebuild.
  BPReload();
  VertexLoaderManager::MarkAllDirty();
}
