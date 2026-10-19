// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "CemuLogging.h"
#include "Common/precompiled.h"
#include "util/helpers/helpers.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"

#include <condition_variable>
#include <deque>

uint64 s_loggingFlagMask = cemuLog_getFlag(LogType::Force);

namespace
{
	// Display names handed to the logging window as the filter string. These
	// are user-visible (the window's filter box matches them).
	const char* CategoryDisplayName(LogType type)
	{
		switch (type)
		{
		case LogType::UnsupportedAPI: return "Unsupported API calls";
		case LogType::APIErrors: return "Invalid API usage";
		case LogType::CoreinitLogging: return "Coreinit Logging";
		case LogType::CoreinitFile: return "Coreinit File-Access";
		case LogType::CoreinitThreadSync: return "Coreinit Thread-Synchronization";
		case LogType::CoreinitMem: return "Coreinit Memory";
		case LogType::CoreinitMP: return "Coreinit MP";
		case LogType::CoreinitThread: return "Coreinit Thread";
		case LogType::NN_NFP: return "nn::nfp";
		case LogType::NN_FP: return "nn::fp";
		case LogType::NN_BOSS: return "nn::boss";
		case LogType::GX2: return "GX2";
		case LogType::SoundAPI: return "Audio";
		case LogType::InputAPI: return "Input";
		case LogType::Socket: return "Socket";
		case LogType::Save: return "Save";
		case LogType::H264: return "H264";
		case LogType::NFC: return "NFC";
		case LogType::NTAG: return "NTAG";
		case LogType::Patches: return "Graphic pack patches";
		case LogType::TextureCache: return "Texture cache";
		case LogType::TextureReadback: return "Texture readback";
		case LogType::OpenGLLogging: return "OpenGL debug output";
		case LogType::VulkanValidation: return "Vulkan validation layer";
		default: return "";
		}
	}

	// the observer slot; a no-op observer is installed while no window listens
	class ObserverSlot
	{
	public:
		template<typename TMsg>
		void Forward(std::string_view filter, TMsg message)
		{
			std::shared_lock guard(m_lock);
			m_current->Log(filter, message);
		}

		void Install(LoggingCallbacks* observer)
		{
			std::unique_lock guard(m_lock);
			cemu_assert_debug(m_current == &m_idle);
			m_current = observer;
		}

		void Remove()
		{
			std::unique_lock guard(m_lock);
			cemu_assert_debug(m_current != &m_idle);
			m_current = &m_idle;
		}

	private:
		LoggingCallbacks m_idle;
		LoggingCallbacks* m_current = &m_idle;
		std::shared_mutex m_lock;
	};

	// Text is queued under a lock and appended to log.txt by a writer thread.
	// Text queued before the file exists is kept and written once it opens.
	class LogFileWriter
	{
	public:
		~LogFileWriter()
		{
			{
				std::unique_lock guard(lock);
				m_quit = true;
			}
			m_wake.notify_all();
			if (m_thread.joinable())
				m_thread.join();
		}

		void Push(std::string&& text)
		{
			// caller holds 'lock'
			m_queue.emplace_back(std::move(text));
		}

		void Notify()
		{
			m_wake.notify_all();
		}

		bool Open(const fs::path& path)
		{
			std::unique_lock guard(lock);
			if (m_out.is_open())
				return true;
			m_out.open(path, std::ios::out);
			if (!m_out.is_open() || m_out.fail())
				return false;
			m_thread = std::thread(&LogFileWriter::WriterLoop, this);
			return true;
		}

		bool IsOpen()
		{
			std::unique_lock guard(lock);
			return m_out.is_open();
		}

		// wait until the queue is empty and the writer has finished its last batch
		void Drain()
		{
			std::unique_lock guard(lock);
			while (!m_queue.empty() || m_writing)
			{
				guard.unlock();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				guard.lock();
			}
		}

		std::recursive_mutex lock;

	private:
		void WriterLoop()
		{
			SetThreadName("cemuLog_thread");
			std::unique_lock guard(lock);
			while (true)
			{
				m_wake.wait(guard, [this] { return m_quit || !m_queue.empty(); });
				if (m_queue.empty())
					break; // quit requested and nothing left
				std::deque<std::string> batch;
				batch.swap(m_queue);
				m_writing = true;
				guard.unlock();
				for (const std::string& piece : batch)
					m_out.write(piece.data(), (std::streamsize)piece.size());
				m_out.flush();
				guard.lock();
				m_writing = false;
			}
		}

		std::deque<std::string> m_queue;
		std::ofstream m_out;
		std::thread m_thread;
		std::condition_variable_any m_wake;
		bool m_writing = false;
		bool m_quit = false;
	};

	ObserverSlot g_observer;
	LogFileWriter g_logFile;

	std::string TimestampPrefix()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t secondsSinceEpoch = std::chrono::system_clock::to_time_t(now);
		const std::tm local = *std::localtime(&secondsSinceEpoch);
		const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
		return fmt::format("[{:02d}:{:02d}:{:02d}.{:03d}] ", local.tm_hour, local.tm_min, local.tm_sec, (int)millis);
	}
}

void cemuLog_setCallbacks(LoggingCallbacks* loggingCallbacks)
{
	g_observer.Install(loggingCallbacks);
}

void cemuLog_clearCallbacks()
{
	g_observer.Remove();
}

bool cemuLog_advancedPPCLoggingEnabled()
{
	return GetConfig().advanced_ppc_logging;
}

fs::path cemuLog_GetLogFilePath()
{
	return ActiveSettings::GetUserDataPath("log.txt");
}

void cemuLog_createLogFile(bool triggeredByCrash)
{
	if (!g_logFile.Open(cemuLog_GetLogFilePath()))
		cemu_assert_debug(false);
}

void cemuLog_writeLineToLog(std::string_view text, bool date, bool new_line)
{
	std::string line;
	if (date)
		line = TimestampPrefix();
	line.append(text);
	if (new_line)
		line.push_back('\n');
	{
		std::unique_lock guard(g_logFile.lock);
		g_logFile.Push(std::move(line));
	}
	g_logFile.Notify();
}

bool cemuLog_log(LogType type, std::string_view text)
{
	if (!cemuLog_isLoggingEnabled(type))
		return false;
	if (LaunchSettings::Verbose())
		std::cout << text << std::endl;
	cemuLog_writeLineToLog(text);
	g_observer.Forward(CategoryDisplayName(type), text);
	return true;
}

bool cemuLog_log(LogType type, std::u8string_view text)
{
	return cemuLog_log(type, std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
}

void cemuLog_logHexDump(LogType type, const void* data, size_t size, size_t lineSize)
{
	if (lineSize == 0)
		return;
	const uint8* bytes = static_cast<const uint8*>(data);
	for (size_t offset = 0; offset < size; offset += lineSize)
	{
		std::string line = fmt::format("{:04X}: ", (int)offset);
		const size_t end = std::min(size, offset + lineSize);
		for (size_t pos = offset; pos < end; pos++)
			line.append(fmt::format("{:02X} ", bytes[pos]));
		cemuLog_log(type, std::string_view(line));
	}
}

void cemuLog_waitForFlush()
{
	cemuLog_createLogFile(false);
	if (g_logFile.IsOpen())
		g_logFile.Drain();
}

std::unique_lock<std::recursive_mutex> cemuLog_acquire()
{
	return std::unique_lock<std::recursive_mutex>(g_logFile.lock);
}

void cemuLog_setActiveLoggingFlags(uint64 flagMask)
{
	s_loggingFlagMask = flagMask | cemuLog_getFlag(LogType::Force);
}
