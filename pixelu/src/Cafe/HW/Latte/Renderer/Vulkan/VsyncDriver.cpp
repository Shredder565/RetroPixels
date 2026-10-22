// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VsyncDriver.h"

#if BOOST_OS_WINDOWS

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <thread>

#include "WindowSystem.h"

namespace
{
	// Kernel mode display thunks exported by gdi32 (declared here because the DDK headers are not available).
	using KmtHandle = UINT32;
	using KmtSourceId = UINT;

	struct KmtOpenAdapterFromHdc
	{
		HDC hDc;
		KmtHandle hAdapter;
		LUID AdapterLuid;
		KmtSourceId VidPnSourceId;
	};

	struct KmtWaitForVBlank
	{
		KmtHandle hAdapter;
		KmtHandle hDevice;
		KmtSourceId VidPnSourceId;
	};

	using PfnOpenAdapterFromHdc = LONG(APIENTRY*)(KmtOpenAdapterFromHdc*);
	using PfnWaitForVBlank = LONG(APIENTRY*)(const KmtWaitForVBlank*);

	constexpr DWORD kRetryDelayMs = 1000 / 60;
	constexpr int kFailuresBeforeReopen = 10;

	class VBlankThread
	{
	public:
		explicit VBlankThread(void (*callback)()) : m_callback(callback)
		{
			HMODULE gdi = LoadLibraryA("gdi32.dll");
			m_openAdapter = reinterpret_cast<PfnOpenAdapterFromHdc>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc"));
			m_waitVBlank = reinterpret_cast<PfnWaitForVBlank>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"));
			m_thread = std::thread(&VBlankThread::Run, this);
		}

		~VBlankThread()
		{
			m_stop = true;
			if (m_thread.joinable())
				m_thread.join();
		}

		void RequestMonitorCheck() { m_monitorCheckPending = true; }

	private:
		static HWND MainWindow()
		{
			return (HWND)WindowSystem::GetWindowInfo().canvas_main.surface;
		}

		// device name of the monitor nearest to the main window, false if there is no window or no monitor info
		static bool QueryMonitorDevice(MONITORINFOEXW& info)
		{
			HWND wnd = MainWindow();
			if (!wnd)
				return false;
			info = {};
			info.cbSize = sizeof(info);
			return GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &info) != 0;
		}

		bool MonitorChanged()
		{
			MONITORINFOEXW info;
			if (!QueryMonitorDevice(info))
				return true;
			return wcscmp(info.szDevice, m_monitorDevice) != 0;
		}

		// opens the adapter driving the main window's monitor, remembering that monitor on success
		bool OpenAdapter()
		{
			if (!MainWindow())
				return false;
			m_monitorDevice[0] = L'\0';
			m_monitorCheckPending = false;
			m_adapter = 0;
			m_source = 0;
			MONITORINFOEXW info;
			if (!QueryMonitorDevice(info))
				return false;
			HDC dc = CreateDCW(nullptr, info.szDevice, nullptr, nullptr);
			if (!dc)
				return false;
			KmtOpenAdapterFromHdc request{};
			request.hDc = dc;
			const bool opened = m_openAdapter(&request) == 0;
			DeleteDC(dc);
			if (!opened)
				return false;
			m_adapter = request.hAdapter;
			m_source = request.VidPnSourceId;
			wcsncpy(m_monitorDevice, info.szDevice, kDeviceNameLength);
			return true;
		}

		// retries until an adapter is open, false when asked to stop meanwhile
		bool ReopenAdapter()
		{
			while (!OpenAdapter())
			{
				Sleep(kRetryDelayMs);
				if (m_stop)
					return false;
			}
			return true;
		}

		void Run()
		{
			OpenAdapter();
			int failures = 0;
			while (!m_stop)
			{
				KmtWaitForVBlank wait{};
				wait.hAdapter = m_adapter;
				wait.hDevice = 0;
				wait.VidPnSourceId = m_source;
				if (m_waitVBlank(&wait) == 0)
				{
					if (m_callback)
						m_callback();
				}
				else
				{
					Sleep(kRetryDelayMs);
					if (++failures >= kFailuresBeforeReopen)
					{
						if (!ReopenAdapter())
							return;
						failures = 0;
					}
				}
				if (m_monitorCheckPending)
				{
					m_monitorCheckPending = false;
					if (MonitorChanged() && !ReopenAdapter())
						return;
				}
			}
		}

		static constexpr size_t kDeviceNameLength = 32;

		void (*m_callback)();
		PfnOpenAdapterFromHdc m_openAdapter = nullptr;
		PfnWaitForVBlank m_waitVBlank = nullptr;
		std::thread m_thread;
		std::atomic<bool> m_stop{ false };
		std::atomic<bool> m_monitorCheckPending{ false };
		KmtHandle m_adapter = 0;
		KmtSourceId m_source = 0;
		WCHAR m_monitorDevice[kDeviceNameLength]{};
	};

	std::mutex s_vblankMutex;
	VBlankThread* s_vblankThread = nullptr;
}

void VsyncDriver_startThread(void(*cbVSync)())
{
	std::lock_guard<std::mutex> lock(s_vblankMutex);
	if (!s_vblankThread)
		s_vblankThread = new VBlankThread(cbVSync);
}

void VsyncDriver_notifyWindowPosChanged()
{
	std::lock_guard<std::mutex> lock(s_vblankMutex);
	if (s_vblankThread)
		s_vblankThread->RequestMonitorCheck();
}

#else

void VsyncDriver_startThread(void(*cbVSync)())
{
	// display driven pacing is only implemented for Windows
	cemu_assert_unimplemented();
}

void VsyncDriver_notifyWindowPosChanged()
{
}

#endif
