// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Resources/Resource.h"

#include "Common/Logging/Log.h"

namespace VideoCommon
{
Resource::Resource(ResourceContext resource_context)
    : m_resource_context(std::move(resource_context))
{
}

void Resource::Process()
{
  MarkAsActive();
  if (m_data_processed != TaskComplete::Error)
    Advance();
}

void Resource::Advance()
{
  for (;;)
  {
    TaskComplete outcome;
    Step following;
    switch (m_step)
    {
    case Step::Reset:
      ResetData();
      outcome = TaskComplete::Yes;
      following = Step::CollectPrimary;
      break;
    case Step::CollectPrimary:
      outcome = CollectPrimaryData();
      following = Step::CollectDependencies;
      // Still waiting on the asset: ask for it to be loaded sooner.
      if (outcome == TaskComplete::No)
        MarkAsPending();
      break;
    case Step::CollectDependencies:
      outcome = CollectDependencyData();
      following = Step::Process;
      break;
    case Step::Process:
      outcome = ProcessData();
      following = Step::Done;
      break;
    case Step::Done:
      return;
    default:
      ERROR_LOG_FMT(VIDEO, "Unknown task '{}' for resource '{}'", static_cast<int>(m_step),
                    m_resource_context.primary_asset_id);
      return;
    }

    if (outcome == TaskComplete::No)
      return;  // try the same step again next time
    if (outcome == TaskComplete::Error)
    {
      // Stays failed until the asset changes.
      m_data_processed = TaskComplete::Error;
      return;
    }

    m_step = following;
    if (m_step == Step::Done)
    {
      m_data_processed = TaskComplete::Yes;
      return;
    }
  }
}

void Resource::Restart(bool failed)
{
  m_data_processed = failed ? TaskComplete::Error : TaskComplete::No;
  m_step = Step::Reset;
  for (Resource* dependent : m_dependents)
    dependent->Restart(failed);
}

void Resource::PropagateUnload()
{
  OnUnloadRequested();
  for (Resource* dependent : m_dependents)
    dependent->PropagateUnload();
}

void Resource::AddReference(Resource* reference)
{
  m_dependents.insert(reference);
}

void Resource::RemoveReference(Resource* reference)
{
  m_dependents.erase(reference);
}

// AssetListener: a load (successful or not) restarts the build; an unload frees what was built.

void Resource::AssetUnloaded()
{
  PropagateUnload();
}

void Resource::NotifyAssetLoadFailed()
{
  Restart(true);
}

void Resource::NotifyAssetLoadSuccess()
{
  Restart(false);
}

// Default build steps: nothing to do, so each completes at once.

Resource::TaskComplete Resource::ProcessData()
{
  return TaskComplete::Yes;
}

Resource::TaskComplete Resource::CollectDependencyData()
{
  return TaskComplete::Yes;
}

Resource::TaskComplete Resource::CollectPrimaryData()
{
  return TaskComplete::Yes;
}

void Resource::ResetData()
{
}

void Resource::OnUnloadRequested()
{
}
}  // namespace VideoCommon
