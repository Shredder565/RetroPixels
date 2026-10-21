// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteRecorder.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/LaunchSettings.h"
#include "util/helpers/helpers.h"

#include <condition_variable>
#include <deque>

#if !BOOST_OS_WINDOWS
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace LatteRecorder
{
	using Clock = std::chrono::steady_clock;

	constexpr sint64 kVideoFPS = 60;
	constexpr sint64 kAudioSampleRate = 48000;
	constexpr size_t kMaxQueuedFrames = 8;
	constexpr size_t kMaxQueuedBytes = 512 * 1024 * 1024;

	/* ffmpeg child process with a pipe connected to its stdin */

	class FFmpegPipe
	{
	public:
		~FFmpegPipe()
		{
			Close();
		}

		bool Open(const fs::path& executable, const std::vector<std::string>& args, const fs::path& logFile);
		bool Write(const void* data, size_t size);
		void Close();
		bool IsOpen() const;

	private:
#if BOOST_OS_WINDOWS
		HANDLE m_stdinWrite = nullptr;
		HANDLE m_process = nullptr;
#else
		int m_stdinWrite = -1;
		pid_t m_pid = -1;
#endif
	};

#if BOOST_OS_WINDOWS
	// quote one argument the way CommandLineToArgvW and the MSVC runtime parse it
	static std::wstring QuoteWindowsArgument(const std::wstring& arg)
	{
		if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
			return arg;
		std::wstring out = L"\"";
		for (auto it = arg.begin();; ++it)
		{
			size_t backslashCount = 0;
			while (it != arg.end() && *it == L'\\')
			{
				++it;
				++backslashCount;
			}
			if (it == arg.end())
			{
				out.append(backslashCount * 2, L'\\');
				break;
			}
			if (*it == L'"')
			{
				out.append(backslashCount * 2 + 1, L'\\');
				out.push_back(L'"');
			}
			else
			{
				out.append(backslashCount, L'\\');
				out.push_back(*it);
			}
		}
		out.push_back(L'"');
		return out;
	}

	bool FFmpegPipe::Open(const fs::path& executable, const std::vector<std::string>& args, const fs::path& logFile)
	{
		Close();
		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		HANDLE stdinRead = nullptr;
		HANDLE stdinWrite = nullptr;
		if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 4 * 1024 * 1024))
		{
			cemuLog_log(LogType::Force, "Recorder: CreatePipe failed ({})", GetLastError());
			return false;
		}
		// only the read end goes to the child, otherwise ffmpeg never sees the end of the stream
		SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
		HANDLE stdoutHandle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		HANDLE stderrHandle = CreateFileW(logFile.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (stderrHandle == INVALID_HANDLE_VALUE)
			stderrHandle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

		std::wstring commandLine = QuoteWindowsArgument(executable.wstring());
		for (const auto& arg : args)
		{
			commandLine.push_back(L' ');
			commandLine.append(QuoteWindowsArgument(boost::nowide::widen(arg)));
		}

		// pass exactly these handles to the child and nothing else this process may have marked inheritable
		HANDLE inheritedHandles[3];
		DWORD inheritedHandleCount = 0;
		inheritedHandles[inheritedHandleCount++] = stdinRead;
		if (stdoutHandle != INVALID_HANDLE_VALUE)
			inheritedHandles[inheritedHandleCount++] = stdoutHandle;
		if (stderrHandle != INVALID_HANDLE_VALUE)
			inheritedHandles[inheritedHandleCount++] = stderrHandle;

		SIZE_T attributeListSize = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
		std::vector<uint8> attributeListBuffer(attributeListSize);
		LPPROC_THREAD_ATTRIBUTE_LIST attributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attributeListBuffer.data();
		bool hasAttributeList = InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeListSize) != FALSE;
		if (hasAttributeList)
			hasAttributeList = UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles, sizeof(HANDLE) * inheritedHandleCount, nullptr, nullptr) != FALSE;

		STARTUPINFOEXW startupInfo{};
		startupInfo.StartupInfo.cb = sizeof(startupInfo);
		startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		startupInfo.StartupInfo.wShowWindow = SW_HIDE;
		startupInfo.StartupInfo.hStdInput = stdinRead;
		startupInfo.StartupInfo.hStdOutput = stdoutHandle;
		startupInfo.StartupInfo.hStdError = stderrHandle;
		DWORD creationFlags = CREATE_NO_WINDOW;
		if (hasAttributeList)
		{
			startupInfo.lpAttributeList = attributeList;
			creationFlags |= EXTENDED_STARTUPINFO_PRESENT;
		}

		const std::wstring executableW = executable.wstring();
		PROCESS_INFORMATION processInfo{};
		const BOOL created = CreateProcessW(executableW.c_str(), commandLine.data(), nullptr, nullptr, TRUE, creationFlags, nullptr, nullptr, &startupInfo.StartupInfo, &processInfo);
		const DWORD createError = GetLastError();

		if (hasAttributeList)
			DeleteProcThreadAttributeList(attributeList);
		CloseHandle(stdinRead);
		if (stdoutHandle != INVALID_HANDLE_VALUE)
			CloseHandle(stdoutHandle);
		if (stderrHandle != INVALID_HANDLE_VALUE)
			CloseHandle(stderrHandle);

		if (!created)
		{
			CloseHandle(stdinWrite);
			cemuLog_log(LogType::Force, "Recorder: failed to start ffmpeg at {} (error {})", _pathToUtf8(executable), createError);
			return false;
		}
		CloseHandle(processInfo.hThread);
		m_process = processInfo.hProcess;
		m_stdinWrite = stdinWrite;
		return true;
	}

	bool FFmpegPipe::Write(const void* data, size_t size)
	{
		if (!m_stdinWrite)
			return false;
		const uint8* ptr = (const uint8*)data;
		while (size > 0)
		{
			const DWORD chunkSize = (DWORD)std::min<size_t>(size, 64 * 1024 * 1024);
			DWORD written = 0;
			if (!WriteFile(m_stdinWrite, ptr, chunkSize, &written, nullptr) || written == 0)
				return false;
			ptr += written;
			size -= written;
		}
		return true;
	}

	void FFmpegPipe::Close()
	{
		if (m_stdinWrite)
		{
			CloseHandle(m_stdinWrite);
			m_stdinWrite = nullptr;
		}
		if (m_process)
		{
			// ffmpeg finishes the file once its input ends
			if (WaitForSingleObject(m_process, 120 * 1000) != WAIT_OBJECT_0)
			{
				cemuLog_log(LogType::Force, "Recorder: ffmpeg did not exit in time, terminating it");
				TerminateProcess(m_process, 1);
				WaitForSingleObject(m_process, 5000);
			}
			DWORD exitCode = 0;
			if (GetExitCodeProcess(m_process, &exitCode) && exitCode != 0)
				cemuLog_log(LogType::Force, "Recorder: ffmpeg exited with code {}", exitCode);
			CloseHandle(m_process);
			m_process = nullptr;
		}
	}

	bool FFmpegPipe::IsOpen() const
	{
		return m_stdinWrite != nullptr;
	}
#else
	bool FFmpegPipe::Open(const fs::path& executable, const std::vector<std::string>& args, const fs::path& logFile)
	{
		Close();
		// a broken pipe must turn into a write error instead of killing the emulator
		signal(SIGPIPE, SIG_IGN);
		int fds[2];
		if (pipe(fds) != 0)
		{
			cemuLog_log(LogType::Force, "Recorder: pipe() failed");
			return false;
		}
		fcntl(fds[1], F_SETFD, FD_CLOEXEC);
		const std::string executableStr = _pathToUtf8(executable);
		const std::string logFileStr = _pathToUtf8(logFile);
		std::vector<char*> argv;
		argv.emplace_back((char*)executableStr.c_str());
		for (const auto& arg : args)
			argv.emplace_back((char*)arg.c_str());
		argv.emplace_back(nullptr);

		posix_spawn_file_actions_t fileActions;
		posix_spawn_file_actions_init(&fileActions);
		posix_spawn_file_actions_adddup2(&fileActions, fds[0], 0);
		posix_spawn_file_actions_addclose(&fileActions, fds[0]);
		posix_spawn_file_actions_addopen(&fileActions, 1, "/dev/null", O_WRONLY, 0);
		posix_spawn_file_actions_addopen(&fileActions, 2, logFileStr.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
		pid_t pid = -1;
		const int result = posix_spawn(&pid, executableStr.c_str(), &fileActions, nullptr, argv.data(), environ);
		posix_spawn_file_actions_destroy(&fileActions);
		close(fds[0]);
		if (result != 0)
		{
			close(fds[1]);
			cemuLog_log(LogType::Force, "Recorder: failed to start ffmpeg at {} (error {})", executableStr, result);
			return false;
		}
		m_pid = pid;
		m_stdinWrite = fds[1];
		return true;
	}

	bool FFmpegPipe::Write(const void* data, size_t size)
	{
		if (m_stdinWrite < 0)
			return false;
		const uint8* ptr = (const uint8*)data;
		while (size > 0)
		{
			const ssize_t written = write(m_stdinWrite, ptr, size);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				return false;
			ptr += written;
			size -= (size_t)written;
		}
		return true;
	}

	void FFmpegPipe::Close()
	{
		if (m_stdinWrite >= 0)
		{
			close(m_stdinWrite);
			m_stdinWrite = -1;
		}
		if (m_pid > 0)
		{
			// like on Windows: ffmpeg gets two minutes to finish the file, then it is ended
			int status = 0;
			const auto giveUpAt = std::chrono::steady_clock::now() + std::chrono::seconds(120);
			pid_t waited;
			while ((waited = waitpid(m_pid, &status, WNOHANG)) == 0 || (waited < 0 && errno == EINTR))
			{
				if (std::chrono::steady_clock::now() >= giveUpAt)
				{
					cemuLog_log(LogType::Force, "Recorder: ffmpeg did not exit in time, terminating it");
					kill(m_pid, SIGKILL);
					while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
				cemuLog_log(LogType::Force, "Recorder: ffmpeg exited with code {}", WEXITSTATUS(status));
			m_pid = -1;
		}
	}

	bool FFmpegPipe::IsOpen() const
	{
		return m_stdinWrite >= 0;
	}
#endif

	/* frame buffer pool, so 8-32 MB frames are not reallocated all the time */

	static std::mutex s_poolMutex;
	static std::vector<std::vector<uint8>> s_bufferPool;

	std::vector<uint8> AcquireFrameBuffer(size_t size)
	{
		std::vector<uint8> buffer;
		{
			std::unique_lock lock(s_poolMutex);
			if (!s_bufferPool.empty())
			{
				buffer = std::move(s_bufferPool.back());
				s_bufferPool.pop_back();
			}
		}
		buffer.resize(size);
		return buffer;
	}

	static void ReleaseFrameBuffer(std::vector<uint8>&& buffer)
	{
		if (buffer.capacity() == 0)
			return;
		std::unique_lock lock(s_poolMutex);
		if (s_bufferPool.size() < 4)
			s_bufferPool.emplace_back(std::move(buffer));
	}

	static std::atomic<uint64> s_droppedFrames{0};

	void NotifyFrameDropped(const char* reason)
	{
		const uint64 count = ++s_droppedFrames;
		if (count == 1 || (count % 60) == 0)
			cemuLog_log(LogType::Force, "Recorder: dropped a frame ({}), {} dropped so far", reason, count);
	}

	/* video encoder: worker thread that pipes frames into ffmpeg at a constant 60 fps */

	static uint32 s_nextVideoFileIndex = 0; // keeps counting across sessions (title switches)

	struct VideoFrame
	{
		std::vector<uint8> data;
		uint32 width;
		uint32 height;
		sint64 slot;
		ColorConversion colorConversion;
	};

	class VideoEncoder
	{
	public:
		VideoEncoder(fs::path ffmpegPath, fs::path framesDir, fs::path logFile)
			: m_ffmpegPath(std::move(ffmpegPath)), m_framesDir(std::move(framesDir)), m_logFile(std::move(logFile))
		{
			m_thread = std::thread(&VideoEncoder::WorkerThread, this);
		}

		~VideoEncoder()
		{
			Finish(0);
		}

		void Push(VideoFrame&& frame)
		{
			{
				std::unique_lock lock(m_mutex);
				if (!m_stopRequested && m_queue.size() < kMaxQueuedFrames && (m_queue.empty() || m_queuedBytes + frame.data.size() <= kMaxQueuedBytes))
				{
					m_queuedBytes += frame.data.size();
					m_queue.emplace_back(std::move(frame));
					m_cv.notify_one();
					return;
				}
			}
			NotifyFrameDropped("encoder queue is full");
			ReleaseFrameBuffer(std::move(frame.data));
		}

		// encode everything still queued, repeat the last picture up to totalFrameCount and close the file
		void Finish(sint64 totalFrameCount)
		{
			{
				std::unique_lock lock(m_mutex);
				if (m_stopRequested)
					return;
				m_stopRequested = true;
				m_finalFrameCount = totalFrameCount;
				m_cv.notify_one();
			}
			if (m_thread.joinable())
				m_thread.join();
		}

	private:
		void WorkerThread()
		{
			SetThreadName("RecorderVideo");
			while (true)
			{
				VideoFrame frame;
				{
					std::unique_lock lock(m_mutex);
					m_cv.wait(lock, [this] { return !m_queue.empty() || m_stopRequested; });
					if (m_queue.empty())
						break;
					frame = std::move(m_queue.front());
					m_queue.pop_front();
					m_queuedBytes -= frame.data.size();
				}
				ProcessFrame(frame);
				ReleaseFrameBuffer(std::move(frame.data));
			}
			sint64 finalFrameCount;
			{
				std::unique_lock lock(m_mutex);
				finalFrameCount = m_finalFrameCount;
			}
			if (m_pipe.IsOpen() && !m_failed && finalFrameCount > m_framesWritten)
				WriteRepeated(finalFrameCount - m_framesWritten);
			CloseFile();
			if (m_framesWritten > 0)
				cemuLog_log(LogType::Force, "Recorder: video finished, {} frames ({} repeated, {} skipped)", m_framesWritten, m_repeatedFrames, m_skippedFrames);
		}

		void ProcessFrame(VideoFrame& frame)
		{
			if (m_failed)
				return;
			if (frame.slot < m_framesWritten)
			{
				m_skippedFrames++; // more than one picture for this 1/60 s
				return;
			}
			if (m_pipe.IsOpen() && (frame.width != m_width || frame.height != m_height))
			{
				// keep the old picture up until the new one starts, then continue in the next file
				WriteRepeated(frame.slot - m_framesWritten);
				CloseFile();
				if (m_failed)
					return;
			}
			if (!m_pipe.IsOpen())
			{
				if (!OpenFile(frame.width, frame.height))
				{
					m_failed = true;
					return;
				}
				if (m_lastFrame.empty())
					m_lastFrame.assign((size_t)frame.width * frame.height * 4, 0); // black until the game shows its first picture
			}
			ApplyColorConversion(frame);
			// fill the time since the previous picture with copies of it (keeps the video on the wall clock)
			WriteRepeated(frame.slot - m_framesWritten);
			if (m_failed)
				return;
			if (!m_pipe.Write(frame.data.data(), frame.data.size()))
			{
				OnWriteError();
				return;
			}
			m_framesWritten = frame.slot + 1;
			std::swap(m_lastFrame, frame.data);
		}

		void WriteRepeated(sint64 count)
		{
			for (sint64 i = 0; i < count; i++)
			{
				if (!m_pipe.Write(m_lastFrame.data(), m_lastFrame.size()))
				{
					OnWriteError();
					return;
				}
				m_framesWritten++;
				m_repeatedFrames++;
			}
		}

		void OnWriteError()
		{
			cemuLog_log(LogType::Force, "Recorder: writing to ffmpeg failed, video recording stopped (see pixelu_ffmpeg.log in the dump folder)");
			m_failed = true;
			m_pipe.Close();
		}

		bool OpenFile(uint32 width, uint32 height)
		{
			const fs::path outputFile = m_framesDir / fmt::format("pixelu_framedump{}.mkv", s_nextVideoFileIndex++);
			const std::vector<std::string> args = GetFFmpegArguments(width, height, _pathToUtf8(outputFile));
			if (!m_pipe.Open(m_ffmpegPath, args, m_logFile))
				return false;
			m_width = width;
			m_height = height;
			cemuLog_log(LogType::Force, "Recorder: writing {}x{} video to {}", width, height, _pathToUtf8(outputFile));
			return true;
		}

		void CloseFile()
		{
			m_pipe.Close();
			m_lastFrame.clear();
		}

		static std::vector<std::string> GetFFmpegArguments(uint32 width, uint32 height, const std::string& outputFile)
		{
			return {
				"-hide_banner", "-loglevel", "warning", "-y",
				"-f", "rawvideo", "-pix_fmt", "rgba", "-video_size", fmt::format("{}x{}", width, height), "-framerate", "60", "-i", "pipe:0",
				"-an",
				"-vf", "crop=trunc(iw/2)*2:trunc(ih/2)*2,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
				"-c:v", "libx264", "-preset", "veryfast", "-crf", "14", "-pix_fmt", "yuv420p",
				"-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv",
				"-r", "60",
				outputFile,
			};
		}

		static void ApplyColorConversion(VideoFrame& frame)
		{
			if (frame.colorConversion == ColorConversion::None)
				return;
			static const auto s_toLinear = [] {
				std::array<uint8, 256> lut;
				for (int i = 0; i < 256; i++)
				{
					const float cs = (float)i / 255.0f;
					const float cl = cs <= 0.04045f ? cs / 12.92f : std::pow((cs + 0.055f) / 1.055f, 2.4f);
					lut[i] = (uint8)std::clamp(cl * 255.0f + 0.5f, 0.0f, 255.0f);
				}
				return lut;
			}();
			static const auto s_toSRGB = [] {
				std::array<uint8, 256> lut;
				for (int i = 0; i < 256; i++)
				{
					const float cl = (float)i / 255.0f;
					const float cs = cl < 0.0031308f ? 12.92f * cl : 1.055f * std::pow(cl, 1.0f / 2.4f) - 0.055f;
					lut[i] = (uint8)std::clamp(cs * 255.0f + 0.5f, 0.0f, 255.0f);
				}
				return lut;
			}();
			const auto& lut = frame.colorConversion == ColorConversion::SRGBToLinear ? s_toLinear : s_toSRGB;
			uint8* ptr = frame.data.data();
			uint8* end = ptr + frame.data.size();
			for (; ptr + 4 <= end; ptr += 4)
			{
				ptr[0] = lut[ptr[0]];
				ptr[1] = lut[ptr[1]];
				ptr[2] = lut[ptr[2]];
			}
		}

		const fs::path m_ffmpegPath;
		const fs::path m_framesDir;
		const fs::path m_logFile;
		std::thread m_thread;

		// shared with the producer
		std::mutex m_mutex;
		std::condition_variable m_cv;
		std::deque<VideoFrame> m_queue;
		size_t m_queuedBytes = 0;
		bool m_stopRequested = false;
		sint64 m_finalFrameCount = 0;

		// worker thread only
		FFmpegPipe m_pipe;
		uint32 m_width = 0;
		uint32 m_height = 0;
		sint64 m_framesWritten = 0;
		std::vector<uint8> m_lastFrame;
		bool m_failed = false;
		uint64 m_repeatedFrames = 0;
		uint64 m_skippedFrames = 0;
	};

	/* audio: 48 kHz s16 stereo WAV, written by a worker thread */

	static bool s_audioFileCreated = false; // later sessions (title switches) append to the same file

	class AudioWriter
	{
	public:
		~AudioWriter()
		{
			Finish(0);
		}

		bool Open(const fs::path& path, bool append)
		{
			m_path = path;
			if (append)
			{
				m_file.open(path, std::ios::in | std::ios::out | std::ios::binary);
				if (m_file.is_open())
				{
					m_file.seekp(0, std::ios::end);
					const uint64 fileSize = (uint64)m_file.tellp();
					m_dataBytes = fileSize > 44 ? fileSize - 44 : 0;
				}
			}
			if (!m_file.is_open())
			{
				m_file.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
				if (!m_file.is_open())
				{
					cemuLog_log(LogType::Force, "Recorder: cannot create {}", _pathToUtf8(path));
					return false;
				}
				m_dataBytes = 0;
				WriteHeader();
			}
			m_thread = std::thread(&AudioWriter::WorkerThread, this);
			return true;
		}

		// stereo frames for the current session, sessionFrameTarget is where the wall clock says this block should end
		void Push(const sint16* stereo, sint64 frameCount, sint64 sessionFrameTarget)
		{
			std::unique_lock lock(m_mutex);
			if (m_stopRequested)
				return;
			const sint64 behind = sessionFrameTarget - (m_sessionFrames + frameCount);
			if (behind > kAudioSampleRate / 10)
			{
				// the game produced too little audio (stutter or pause): fill the gap with silence to stay in sync with the video
				AddSilenceLocked(behind);
			}
			else if (m_sessionFrames - sessionFrameTarget > kAudioSampleRate / 4)
			{
				// more than 250 ms ahead of the wall clock, drop this block
				return;
			}
			if (m_pendingFrames > kAudioSampleRate * 10)
			{
				if (!m_overflowLogged)
					cemuLog_log(LogType::Force, "Recorder: audio writer is falling behind, dropping audio");
				m_overflowLogged = true;
				return;
			}
			if (m_segments.empty())
				m_segments.emplace_back();
			auto& samples = m_segments.back().samples;
			samples.insert(samples.end(), stereo, stereo + frameCount * 2);
			m_sessionFrames += frameCount;
			m_pendingFrames += frameCount;
			if (m_pendingFrames >= kAudioSampleRate / 4)
				m_cv.notify_one();
		}

		// write what is pending, pad with silence up to sessionFrameCount and fix up the header
		void Finish(sint64 sessionFrameCount)
		{
			{
				std::unique_lock lock(m_mutex);
				if (m_stopRequested)
					return;
				if (sessionFrameCount > m_sessionFrames)
					AddSilenceLocked(sessionFrameCount - m_sessionFrames);
				m_stopRequested = true;
				m_cv.notify_one();
			}
			if (m_thread.joinable())
				m_thread.join();
			if (m_file.is_open())
			{
				WriteHeader();
				m_file.close();
				cemuLog_log(LogType::Force, "Recorder: audio finished, {:.1f} seconds in {}", (double)m_dataBytes / (kAudioSampleRate * 4), _pathToUtf8(m_path));
			}
		}

	private:
		struct Segment
		{
			sint64 silenceFrames = 0; // written before the samples
			std::vector<sint16> samples;
		};

		void AddSilenceLocked(sint64 frames)
		{
			Segment segment;
			segment.silenceFrames = frames;
			m_segments.emplace_back(std::move(segment));
			m_sessionFrames += frames;
		}

		void WorkerThread()
		{
			SetThreadName("RecorderAudio");
			auto lastHeaderUpdate = Clock::now();
			while (true)
			{
				std::deque<Segment> segments;
				bool stop;
				{
					std::unique_lock lock(m_mutex);
					m_cv.wait_for(lock, std::chrono::milliseconds(250), [this] { return m_stopRequested || m_pendingFrames >= kAudioSampleRate / 4; });
					segments.swap(m_segments);
					m_pendingFrames = 0;
					stop = m_stopRequested;
				}
				for (auto& segment : segments)
				{
					WriteSilence(segment.silenceFrames);
					WriteSamples(segment.samples.data(), segment.samples.size());
				}
				if (stop)
					break;
				// keep the header valid while recording, in case the process gets killed
				if (Clock::now() - lastHeaderUpdate > std::chrono::seconds(2))
				{
					WriteHeader();
					lastHeaderUpdate = Clock::now();
				}
			}
		}

		void WriteSamples(const sint16* samples, size_t count)
		{
			if (count == 0 || !m_file.is_open())
				return;
			// WAV is little-endian, same as every host PixelU runs on
			m_file.write((const char*)samples, count * sizeof(sint16));
			m_dataBytes += count * sizeof(sint16);
		}

		void WriteSilence(sint64 frames)
		{
			static const std::vector<sint16> s_silence(4800 * 2, 0);
			while (frames > 0)
			{
				const sint64 chunk = std::min<sint64>(frames, 4800);
				WriteSamples(s_silence.data(), (size_t)chunk * 2);
				frames -= chunk;
			}
		}

		static void PutU32(uint8* p, uint32 v)
		{
			p[0] = (uint8)v;
			p[1] = (uint8)(v >> 8);
			p[2] = (uint8)(v >> 16);
			p[3] = (uint8)(v >> 24);
		}

		static void PutU16(uint8* p, uint16 v)
		{
			p[0] = (uint8)v;
			p[1] = (uint8)(v >> 8);
		}

		void WriteHeader()
		{
			const uint32 dataBytes = (uint32)std::min<uint64>(m_dataBytes, 0xFFFFFFFFull - 36);
			uint8 header[44];
			memcpy(header + 0, "RIFF", 4);
			PutU32(header + 4, 36 + dataBytes);
			memcpy(header + 8, "WAVE", 4);
			memcpy(header + 12, "fmt ", 4);
			PutU32(header + 16, 16); // fmt chunk size
			PutU16(header + 20, 1); // PCM
			PutU16(header + 22, 2); // channels
			PutU32(header + 24, (uint32)kAudioSampleRate);
			PutU32(header + 28, (uint32)kAudioSampleRate * 4); // byte rate
			PutU16(header + 32, 4); // block align
			PutU16(header + 34, 16); // bits per sample
			memcpy(header + 36, "data", 4);
			PutU32(header + 40, dataBytes);
			m_file.seekp(0, std::ios::beg);
			m_file.write((const char*)header, sizeof(header));
			m_file.seekp(0, std::ios::end);
			m_file.flush();
		}

		fs::path m_path;
		std::fstream m_file;
		uint64 m_dataBytes = 0;
		std::thread m_thread;

		std::mutex m_mutex;
		std::condition_variable m_cv;
		std::deque<Segment> m_segments;
		sint64 m_pendingFrames = 0;
		sint64 m_sessionFrames = 0; // frames accounted for in this session (written, pending and silence)
		bool m_stopRequested = false;
		bool m_overflowLogged = false;
	};

	/* session */

	static std::shared_mutex s_sessionMutex;
	static std::atomic_bool s_recording{false};
	static Clock::time_point s_sessionStart;
	static std::atomic<sint64> s_lastFrameSlot{-1};
	static std::unique_ptr<VideoEncoder> s_video;
	static std::unique_ptr<AudioWriter> s_audio;

	bool IsEnabled()
	{
		return LaunchSettings::GetRPRecordPath().has_value() && LaunchSettings::GetRPFFmpegPath().has_value();
	}

	bool IsRecording()
	{
		return s_recording.load(std::memory_order_acquire);
	}

	void Start()
	{
		if (!IsEnabled())
			return;
		std::unique_lock lock(s_sessionMutex);
		if (s_recording)
			return;
		const fs::path dumpDir = *LaunchSettings::GetRPRecordPath();
		const fs::path ffmpegPath = *LaunchSettings::GetRPFFmpegPath();
		std::error_code ec;
		fs::create_directories(dumpDir / "Frames", ec);
		fs::create_directories(dumpDir / "Audio", ec);
		if (!fs::exists(ffmpegPath, ec))
			cemuLog_log(LogType::Force, "Recorder: ffmpeg not found at {}", _pathToUtf8(ffmpegPath));

		static bool s_atExitRegistered = false;
		if (!s_atExitRegistered)
		{
			std::atexit([] { Stop(); });
			s_atExitRegistered = true;
		}

		s_sessionStart = Clock::now();
		s_lastFrameSlot = -1;
		s_droppedFrames = 0;
		s_video = std::make_unique<VideoEncoder>(ffmpegPath, dumpDir / "Frames", dumpDir / "pixelu_ffmpeg.log");
		s_audio = std::make_unique<AudioWriter>();
		if (!s_audio->Open(dumpDir / "Audio" / "pixelu_dspdump.wav", s_audioFileCreated))
			s_audio.reset();
		else
			s_audioFileCreated = true;
		s_recording.store(true, std::memory_order_release);
		cemuLog_log(LogType::Force, "Recorder: recording the TV picture and audio to {}", _pathToUtf8(dumpDir));
	}

	void Stop()
	{
		std::unique_ptr<VideoEncoder> video;
		std::unique_ptr<AudioWriter> audio;
		double elapsedSeconds;
		{
			std::unique_lock lock(s_sessionMutex);
			if (!s_recording)
				return;
			s_recording.store(false, std::memory_order_release);
			elapsedSeconds = std::chrono::duration<double>(Clock::now() - s_sessionStart).count();
			video = std::move(s_video);
			audio = std::move(s_audio);
		}
		// both streams end at the same wall clock time
		if (video)
			video->Finish((sint64)std::llround(elapsedSeconds * kVideoFPS));
		if (audio)
			audio->Finish((sint64)std::llround(elapsedSeconds * kAudioSampleRate));
		cemuLog_log(LogType::Force, "Recorder: stopped after {:.1f} seconds", elapsedSeconds);
	}

	// pick the 1/60 s slot a picture shown right now belongs to, false if this slot already has one
	// a picture stays in the slot after the previous one unless it is clearly late, so small timing jitter
	// does not turn into skipped and repeated frames
	static bool AllocateFrameSlot(sint64& slotOut)
	{
		const double ideal = std::chrono::duration<double>(Clock::now() - s_sessionStart).count() * (double)kVideoFPS;
		const sint64 lastSlot = s_lastFrameSlot.load();
		const double lag = ideal - (double)(lastSlot + 1);
		if (lag < -0.6)
			return false; // faster than 60 fps, the previous picture keeps this slot
		sint64 slot = lastSlot + 1;
		if (lag > 0.0)
			slot += (sint64)std::floor(lag + 0.1);
		s_lastFrameSlot = slot;
		slotOut = slot;
		return true;
	}

	void OnTVScanBufferCopy(LatteTextureView* texView)
	{
		if (!IsRecording() || !g_renderer)
			return;
		g_renderer->recorder_pollFrames();
		sint64 slot;
		if (!AllocateFrameSlot(slot))
			return;
		LatteTexture_UpdateDataToLatest(texView->baseTexture);
		g_renderer->recorder_captureTVFrame(texView, slot);
	}

	void SubmitVideoFrame(std::vector<uint8>&& rgba, uint32 width, uint32 height, sint64 frameSlot, ColorConversion colorConversion)
	{
		std::shared_lock lock(s_sessionMutex);
		if (!s_recording || !s_video || width == 0 || height == 0 || rgba.size() < (size_t)width * height * 4)
		{
			ReleaseFrameBuffer(std::move(rgba));
			return;
		}
		VideoFrame frame;
		frame.data = std::move(rgba);
		frame.width = width;
		frame.height = height;
		frame.slot = frameSlot;
		frame.colorConversion = colorConversion;
		s_video->Push(std::move(frame));
	}

	void SubmitTVAudioBE(const sint16* samples, sint32 sampleCountPerChannel, sint32 channelCount)
	{
		if (!IsRecording() || sampleCountPerChannel <= 0 || channelCount <= 0)
			return;
		std::shared_lock lock(s_sessionMutex);
		if (!s_recording || !s_audio)
			return;
		// downmix to stereo
		sint16 stereo[2 * 1024];
		sint32 offset = 0;
		while (offset < sampleCountPerChannel)
		{
			const sint32 count = std::min<sint32>(sampleCountPerChannel - offset, 1024);
			for (sint32 i = 0; i < count; i++)
			{
				const sint16* s = samples + (size_t)(offset + i) * channelCount;
				sint32 left, right;
				if (channelCount == 1)
				{
					left = right = _swapEndianS16(s[0]);
				}
				else if (channelCount >= 6)
				{
					// FL, FR, FC, LFE, BL, BR
					const float fl = _swapEndianS16(s[0]);
					const float fr = _swapEndianS16(s[1]);
					const float fc = _swapEndianS16(s[2]);
					const float bl = _swapEndianS16(s[4]);
					const float br = _swapEndianS16(s[5]);
					left = (sint32)std::lround(fl + 0.7071f * fc + 0.7071f * bl);
					right = (sint32)std::lround(fr + 0.7071f * fc + 0.7071f * br);
				}
				else
				{
					left = _swapEndianS16(s[0]);
					right = _swapEndianS16(s[1]);
				}
				stereo[i * 2 + 0] = (sint16)std::clamp(left, -32768, 32767);
				stereo[i * 2 + 1] = (sint16)std::clamp(right, -32768, 32767);
			}
			offset += count;
			const sint64 target = (sint64)(std::chrono::duration<double>(Clock::now() - s_sessionStart).count() * (double)kAudioSampleRate);
			s_audio->Push(stereo, count, target);
		}
	}
}
