// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/WorkQueueThread.h"

#include "VideoCommon/Assets/AssetListener.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"

#include <memory>
#include <unordered_set>

class AbstractTexture;
namespace VideoCommon
{
class AsyncShaderCompiler;
class CustomAssetCache;
class CustomResourceManager;
class TexturePool;

// Runtime object built from one primary asset plus whatever it depends on (a material and
// its shader and textures, say), producing data the renderer can use directly.
//
// Building happens in steps, advanced by Process(): reset, collect the primary asset's data,
// collect dependencies, process. A step that is not ready yet is retried on the next Process
// call. Any change to the asset (or to a resource that references this one) starts over.
class Resource : public AssetListener
{
public:
  // Services a resource may use.
  struct ResourceContext
  {
    CustomAssetLibrary::AssetID primary_asset_id;
    std::shared_ptr<CustomAssetLibrary> asset_library;
    CustomAssetCache* asset_cache;
    CustomResourceManager* resource_manager;
    TexturePool* texture_pool;
    Common::AsyncWorkThreadSP* worker_queue;
    AsyncShaderCompiler* shader_compiler;
    AbstractTexture* invalid_array_texture;
    AbstractTexture* invalid_color_texture;
    AbstractTexture* invalid_cubemap_texture;
    AbstractTexture* invalid_transparent_texture;
  };
  explicit Resource(ResourceContext resource_context);

  enum class TaskComplete
  {
    Yes,
    No,
    Error,
  };

  // Marks the resource as in use and advances building as far as possible.
  void Process();
  // Yes once built, Error if building failed (until the asset changes), No otherwise.
  TaskComplete IsDataProcessed() const { return m_data_processed; }

  // `reference` depends on this resource and is reset whenever this one is.
  void AddReference(Resource* reference);
  void RemoveReference(Resource* reference);

  virtual void MarkAsActive() = 0;
  virtual void MarkAsPending() = 0;

protected:
  ResourceContext m_resource_context;

private:
  void Advance();
  void Restart(bool failed);
  void PropagateUnload();

  void NotifyAssetLoadSuccess() final;
  void NotifyAssetLoadFailed() final;
  void AssetUnloaded() final;
  virtual void OnUnloadRequested();

  // Build steps, overridden by each kind of resource. The defaults succeed immediately.
  friend class CustomResourceManager;
  virtual void ResetData();
  virtual TaskComplete CollectPrimaryData();
  virtual TaskComplete CollectDependencyData();
  virtual TaskComplete ProcessData();

  TaskComplete m_data_processed = TaskComplete::No;

  enum class Step
  {
    Reset,
    CollectPrimary,
    CollectDependencies,
    Process,
    Done,
  };
  Step m_step = Step::Reset;

  std::unordered_set<Resource*> m_dependents;
};
}  // namespace VideoCommon
