// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "Common/WorkQueueThread.h"
#endif

struct cubeb;

namespace CubebUtils
{
// The process-wide cubeb context, created on first use and destroyed with its last user.
std::shared_ptr<cubeb> GetContext();

// Enabled devices as (device ID, display name) pairs.
std::vector<std::pair<std::string, std::string>> ListInputDevices();
std::vector<std::pair<std::string, std::string>> ListOutputDevices();

// The cubeb_devid of the device with this ID, or null for "" or an unknown ID (system default).
const void* GetInputDeviceById(std::string_view id);
const void* GetOutputDeviceById(std::string_view id);

// Runs functions on a worker thread that has joined COM's multithreaded apartment, which cubeb's
// WASAPI backend requires. Elsewhere the functions run on the calling thread.
class CoInitSyncWorker
{
public:
  using FunctionType = std::function<void()>;

  CoInitSyncWorker(std::string worker_name);
  ~CoInitSyncWorker();

  // Returns false, without running f, if COM could not be initialised on the worker.
  bool Execute(FunctionType f);

#ifdef _WIN32
private:
  Common::AsyncWorkThread m_thread;
  bool m_com_ready = false;
  bool m_com_needs_uninit = false;
#endif
};
}  // namespace CubebUtils
