// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

/*
 * RetroPixels.cpp
 *
 * Additions made for RetroPixels: the player's own folder (SUPERMODEL_HOME)
 * and the game recorder (-rp-record). See RetroPixels.h.
 */

#include "OSD/RetroPixels.h"
#include "OSD/Logger.h"

#include <GL/glew.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace RetroPixels
{
  /****************************************************************************
   The player's own folder
  ****************************************************************************/

#ifdef _WIN32
  // Narrow (ANSI code page) paths are what the rest of Supermodel opens files
  // with, so a folder whose name doesn't fit the code page (a user name with
  // letters from another alphabet) is turned into its 8.3 short form.
  static std::string FromWide(const std::wstring &w)
  {
    if (w.empty())
      return std::string();
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &lossy);
    if (n <= 0)
      return std::string();
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, &s[0], n, nullptr, &lossy);
    s.resize(n - 1);
    if (lossy)
    {
      DWORD m = GetShortPathNameW(w.c_str(), nullptr, 0);
      if (m > 0)
      {
        std::wstring shortName(m, L'\0');
        m = GetShortPathNameW(w.c_str(), &shortName[0], m);
        shortName.resize(m);
        if (m > 0)
          return FromWide(shortName);
      }
    }
    return s;
  }

  static std::string HomeDir()
  {
    DWORD n = GetEnvironmentVariableW(L"SUPERMODEL_HOME", nullptr, 0);
    if (n == 0)
      return std::string();
    std::wstring w(n, L'\0');
    n = GetEnvironmentVariableW(L"SUPERMODEL_HOME", &w[0], n);
    w.resize(n);
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/'))
      w.pop_back();
    if (w.empty())
      return std::string();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(w), ec); // before the short name is asked for
    return FromWide(w);
  }
#else
  static std::string HomeDir()
  {
    const char *env = getenv("SUPERMODEL_HOME");
    std::string s = env ? env : "";
    while (!s.empty() && s.back() == '/')
      s.pop_back();
    return s;
  }
#endif

  std::string JoinPath(const std::string &dir, const std::string &name)
  {
    if (dir.empty())
      return name;
    char last = dir.back();
    if (last == '/' || last == '\\')
      return dir + name;
    return dir + "/" + name;
  }

  std::string HomePath(FileSystemPath::PathType pathType)
  {
    const char *sub = nullptr;
    switch (pathType)
    {
    case FileSystemPath::Analysis:    sub = "Analysis"; break;
    case FileSystemPath::Config:      sub = "Config"; break;
    case FileSystemPath::Log:         sub = "Log"; break;
    case FileSystemPath::NVRAM:       sub = "NVRAM"; break;
    case FileSystemPath::Saves:       sub = "Saves"; break;
    case FileSystemPath::Screenshots: sub = "Screenshots"; break;
    default:                          return std::string(); // Data, Assets: read-only files that come with Supermodel
    }
    std::string home = HomeDir();
    if (home.empty())
      return std::string();
    std::string dir = JoinPath(home, sub);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    return dir + "/";
  }

  /****************************************************************************
   Recorder
  ****************************************************************************/

  namespace Recorder
  {
    static constexpr int    kFPS = 60;               // Supermodel makes 735 samples (44100/60) of sound per frame
    static constexpr int    kSampleRate = 44100;
    static constexpr size_t kMaxQueuedFrames = 6;    // the game waits for ffmpeg rather than dropping frames

    // ffmpeg with a pipe to its standard input
    class Pipe
    {
    public:
      ~Pipe() { Close(); }

      bool Open(const std::string &exe, const std::vector<std::string> &args, const std::string &logFile);
      bool Write(const void *data, size_t size);
      void Close();

    private:
#ifdef _WIN32
      HANDLE m_write = nullptr;
      HANDLE m_process = nullptr;
#else
      int m_write = -1;
      pid_t m_pid = -1;
#endif
    };

#ifdef _WIN32
    static std::wstring Widen(const std::string &s)
    {
      if (s.empty())
        return std::wstring();
      int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
      std::wstring w(n, L'\0');
      MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], n);
      w.resize(n - 1);
      return w;
    }

    // Quotes one argument the way CommandLineToArgvW reads it back.
    static std::wstring Quote(const std::wstring &arg)
    {
      if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
      std::wstring out = L"\"";
      for (auto it = arg.begin(); ; ++it)
      {
        size_t slashes = 0;
        while (it != arg.end() && *it == L'\\')
        {
          ++it;
          ++slashes;
        }
        if (it == arg.end())
        {
          out.append(slashes * 2, L'\\');
          break;
        }
        if (*it == L'"')
        {
          out.append(slashes * 2 + 1, L'\\');
          out.push_back(L'"');
        }
        else
        {
          out.append(slashes, L'\\');
          out.push_back(*it);
        }
      }
      out.push_back(L'"');
      return out;
    }

    bool Pipe::Open(const std::string &exe, const std::vector<std::string> &args, const std::string &logFile)
    {
      Close();
      SECURITY_ATTRIBUTES sa{};
      sa.nLength = sizeof(sa);
      sa.bInheritHandle = TRUE;
      HANDLE readEnd = nullptr, writeEnd = nullptr;
      if (!CreatePipe(&readEnd, &writeEnd, &sa, 4 * 1024 * 1024))
        return false;
      // Only the read end goes to ffmpeg, or it would never see the stream end.
      SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);
      HANDLE out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      HANDLE err = CreateFileW(Widen(logFile).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (err == INVALID_HANDLE_VALUE)
        err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

      std::wstring cmd = Quote(Widen(exe));
      for (auto &a: args)
      {
        cmd.push_back(L' ');
        cmd += Quote(Widen(a));
      }

      // Hand ffmpeg exactly these handles and nothing else.
      HANDLE inherit[3];
      DWORD numInherit = 0;
      inherit[numInherit++] = readEnd;
      if (out != INVALID_HANDLE_VALUE)
        inherit[numInherit++] = out;
      if (err != INVALID_HANDLE_VALUE)
        inherit[numInherit++] = err;
      SIZE_T attrSize = 0;
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
      std::vector<uint8_t> attrBuf(attrSize);
      auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST) attrBuf.data();
      bool haveAttrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) != FALSE;
      if (haveAttrs)
        haveAttrs = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(HANDLE) * numInherit, nullptr, nullptr) != FALSE;

      STARTUPINFOEXW si{};
      si.StartupInfo.cb = sizeof(si);
      si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
      si.StartupInfo.wShowWindow = SW_HIDE;
      si.StartupInfo.hStdInput = readEnd;
      si.StartupInfo.hStdOutput = out;
      si.StartupInfo.hStdError = err;
      DWORD flags = CREATE_NO_WINDOW;
      if (haveAttrs)
      {
        si.lpAttributeList = attrs;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
      }
      PROCESS_INFORMATION pi{};
      std::wstring exeW = Widen(exe);
      BOOL ok = CreateProcessW(exeW.c_str(), &cmd[0], nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
      if (haveAttrs)
        DeleteProcThreadAttributeList(attrs);
      CloseHandle(readEnd);
      if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
      if (err != INVALID_HANDLE_VALUE)
        CloseHandle(err);
      if (!ok)
      {
        CloseHandle(writeEnd);
        return false;
      }
      CloseHandle(pi.hThread);
      m_process = pi.hProcess;
      m_write = writeEnd;
      return true;
    }

    bool Pipe::Write(const void *data, size_t size)
    {
      if (!m_write)
        return false;
      auto p = (const uint8_t *) data;
      while (size > 0)
      {
        DWORD chunk = (DWORD) std::min<size_t>(size, 16 * 1024 * 1024), written = 0;
        if (!WriteFile(m_write, p, chunk, &written, nullptr) || written == 0)
          return false;
        p += written;
        size -= written;
      }
      return true;
    }

    void Pipe::Close()
    {
      if (m_write)
      {
        CloseHandle(m_write);
        m_write = nullptr;
      }
      if (m_process)
      {
        // ffmpeg finishes the file once its input ends.
        if (WaitForSingleObject(m_process, 120 * 1000) != WAIT_OBJECT_0)
        {
          TerminateProcess(m_process, 1);
          WaitForSingleObject(m_process, 5000);
        }
        CloseHandle(m_process);
        m_process = nullptr;
      }
    }
#else
    bool Pipe::Open(const std::string &exe, const std::vector<std::string> &args, const std::string &logFile)
    {
      Close();
      signal(SIGPIPE, SIG_IGN); // a closed pipe is a write error, not the end of the emulator
      int fds[2];
      if (pipe(fds) != 0)
        return false;
      fcntl(fds[1], F_SETFD, FD_CLOEXEC);
      std::vector<char *> argv;
      argv.push_back((char *) exe.c_str());
      for (auto &a: args)
        argv.push_back((char *) a.c_str());
      argv.push_back(nullptr);
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, fds[0], 0);
      posix_spawn_file_actions_addclose(&actions, fds[0]);
      posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&actions, 2, logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
      pid_t pid = -1;
      int result = posix_spawnp(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      close(fds[0]);
      if (result != 0)
      {
        close(fds[1]);
        return false;
      }
      m_pid = pid;
      m_write = fds[1];
      return true;
    }

    bool Pipe::Write(const void *data, size_t size)
    {
      if (m_write < 0)
        return false;
      auto p = (const uint8_t *) data;
      while (size > 0)
      {
        ssize_t n = write(m_write, p, size);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          return false;
        p += n;
        size -= (size_t) n;
      }
      return true;
    }

    void Pipe::Close()
    {
      if (m_write >= 0)
      {
        close(m_write);
        m_write = -1;
      }
      if (m_pid > 0)
      {
        int status = 0;
        waitpid(m_pid, &status, 0);
        m_pid = -1;
      }
    }
#endif

    struct VideoFrame
    {
      unsigned width = 0, height = 0;
      std::vector<uint8_t> rgba;
    };

    static std::mutex s_mutex;
    static std::condition_variable s_cv;
    static std::deque<VideoFrame> s_queue;
    static std::thread s_thread;
    static bool s_active = false;
    static bool s_stopping = false;
    static bool s_paused = false;
    static bool s_failed = false;
    static std::string s_dir, s_ffmpeg;

    static std::mutex s_audioMutex;
    static FILE *s_wav = nullptr;
    static uint32_t s_wavBytes = 0;
    static unsigned s_wavFramesSinceHeader = 0;

    static void PutLE32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
    static void PutLE16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

    // The header's sizes are kept up to date as the file grows, so the file
    // can be read even if Supermodel is ended without saying goodbye.
    static void WriteWavHeader()
    {
      uint8_t h[44];
      memcpy(h, "RIFF", 4);
      PutLE32(h + 4, 36 + s_wavBytes);
      memcpy(h + 8, "WAVEfmt ", 8);
      PutLE32(h + 16, 16);
      PutLE16(h + 20, 1);                    // PCM
      PutLE16(h + 22, 2);                    // stereo
      PutLE32(h + 24, kSampleRate);
      PutLE32(h + 28, kSampleRate * 2 * 2);  // bytes per second
      PutLE16(h + 32, 4);                    // bytes per sample frame
      PutLE16(h + 34, 16);
      memcpy(h + 36, "data", 4);
      PutLE32(h + 40, s_wavBytes);
      long pos = ftell(s_wav);
      fseek(s_wav, 0, SEEK_SET);
      fwrite(h, 1, sizeof(h), s_wav);
      if (pos > 0)
        fseek(s_wav, pos, SEEK_SET);
      fflush(s_wav);
    }

    static std::vector<std::string> FFmpegArgs(unsigned width, unsigned height, const std::string &out)
    {
      return {
        "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgba", "-video_size", std::to_string(width) + "x" + std::to_string(height),
        "-framerate", std::to_string(kFPS), "-i", "pipe:0",
        "-an",
        // OpenGL reads the picture bottom row first
        "-vf", "vflip,crop=trunc(iw/2)*2:trunc(ih/2)*2,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
        "-c:v", "libx264", "-preset", "superfast", "-crf", "14", "-pix_fmt", "yuv420p",
        "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv",
        out,
      };
    }

    static void Worker()
    {
      Pipe pipe;
      unsigned width = 0, height = 0;
      int fileIndex = 0;
      bool failed = false;
      const std::string log = JoinPath(s_dir, "supermodel_ffmpeg.log");
      for (;;)
      {
        VideoFrame f;
        {
          std::unique_lock<std::mutex> lock(s_mutex);
          s_cv.wait(lock, [] { return s_stopping || !s_queue.empty(); });
          if (s_queue.empty())
            break; // stopping, and everything is written
          f = std::move(s_queue.front());
          s_queue.pop_front();
        }
        s_cv.notify_all(); // room in the queue again
        if (failed)
          continue;
        if (f.width != width || f.height != height)
        {
          pipe.Close();
          std::string out = JoinPath(JoinPath(s_dir, "Frames"), "supermodel_framedump" + std::to_string(fileIndex++) + ".mkv");
          if (!pipe.Open(s_ffmpeg, FFmpegArgs(f.width, f.height, out), log))
          {
            ErrorLog("Recorder: couldn't start ffmpeg (%s); the picture isn't being recorded.", s_ffmpeg.c_str());
            failed = true;
            continue;
          }
          InfoLog("Recorder: writing %ux%u video to %s", f.width, f.height, out.c_str());
          width = f.width;
          height = f.height;
        }
        if (!pipe.Write(f.rgba.data(), f.rgba.size()))
        {
          ErrorLog("Recorder: writing to ffmpeg failed, the picture isn't being recorded any more (see %s).", log.c_str());
          pipe.Close();
          failed = true;
        }
      }
      pipe.Close();
      std::lock_guard<std::mutex> lock(s_mutex);
      s_failed = failed;
    }

    bool Start(const std::string &dir, const std::string &ffmpeg)
    {
      Stop();
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(JoinPath(dir, "Frames")), ec);
      std::filesystem::create_directories(std::filesystem::path(JoinPath(dir, "Audio")), ec);
      std::string wav = JoinPath(JoinPath(dir, "Audio"), "supermodel_dspdump.wav");
      FILE *f = fopen(wav.c_str(), "wb");
      if (!f)
      {
        ErrorLog("Recorder: couldn't make %s; this game isn't being recorded.", wav.c_str());
        return false;
      }
      {
        std::lock_guard<std::mutex> lock(s_audioMutex);
        s_wav = f;
        s_wavBytes = 0;
        s_wavFramesSinceHeader = 0;
        WriteWavHeader();
      }
      {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_dir = dir;
        s_ffmpeg = ffmpeg;
        s_queue.clear();
        s_stopping = false;
        s_paused = false;
        s_failed = false;
        s_active = true;
      }
      s_thread = std::thread(Worker);
      InfoLog("Recorder: recording into %s (ffmpeg: %s)", dir.c_str(), ffmpeg.c_str());
      return true;
    }

    bool Active()
    {
      std::lock_guard<std::mutex> lock(s_mutex);
      return s_active;
    }

    void SetPaused(bool paused)
    {
      std::lock_guard<std::mutex> lock(s_mutex);
      s_paused = paused;
    }

    // The picture is recorded at most kMaxRecordHeight high: a bigger window
    // (a 1440p or 4K screen) is scaled down on the graphics card first, so
    // reading it back and encoding it keep up with the game.
    static constexpr unsigned kMaxRecordHeight = 1080;
    static constexpr int kReadBuffers = 3;   // frames read back without waiting (one frame late)

    struct Readback
    {
      GLuint fbo = 0, colour = 0;
      unsigned fboW = 0, fboH = 0;
      GLuint pbo[kReadBuffers] = {};
      unsigned w[kReadBuffers] = {}, h[kReadBuffers] = {};
      bool pending[kReadBuffers] = {};
      int next = 0;
    };
    static Readback s_rb;

    static void Queue(VideoFrame &&f)
    {
      std::unique_lock<std::mutex> lock(s_mutex);
      // Wait for ffmpeg rather than drop a frame: the sound keeps every
      // frame's samples, so the two stay the same length.
      s_cv.wait(lock, [] { return s_stopping || s_queue.size() < kMaxQueuedFrames; });
      if (s_stopping)
        return;
      s_queue.push_back(std::move(f));
      lock.unlock();
      s_cv.notify_all();
    }

    // Copies the frame read into buffer i to the queue.
    static void Collect(int i)
    {
      if (!s_rb.pending[i])
        return;
      s_rb.pending[i] = false;
      VideoFrame f;
      f.width = s_rb.w[i];
      f.height = s_rb.h[i];
      f.rgba.resize(size_t(f.width) * f.height * 4);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, s_rb.pbo[i]);
      if (const void *p = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY))
      {
        memcpy(f.rgba.data(), p, f.rgba.size());
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      Queue(std::move(f));
    }

    // Queues the frames still being read, oldest first.
    static void CollectAll()
    {
      if (s_rb.pbo[0] == 0)
        return;
      GLint packBuffer = 0;
      glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
      for (int k = 0; k < kReadBuffers; k++)
        Collect((s_rb.next + k) % kReadBuffers);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
    }

    static void FreeReadback()
    {
      if (s_rb.pbo[0])
        glDeleteBuffers(kReadBuffers, s_rb.pbo);
      if (s_rb.fbo)
        glDeleteFramebuffers(1, &s_rb.fbo);
      if (s_rb.colour)
        glDeleteRenderbuffers(1, &s_rb.colour);
      s_rb = Readback();
    }

    void Frame(unsigned width, unsigned height)
    {
      {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_active || s_paused || s_failed || width == 0 || height == 0)
          return;
      }
      // The recorded size: the window's, or scaled down to kMaxRecordHeight.
      unsigned outW = width, outH = height;
      if (height > kMaxRecordHeight)
      {
        outH = kMaxRecordHeight;
        outW = unsigned(double(width) * outH / height + 0.5);
        outW += outW & 1;
      }

      // Leave the GL state as it was.
      GLint readFBO = 0, drawFBO = 0, packBuffer = 0, readBuffer = 0, packAlign = 4, renderbuffer = 0;
      GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFBO);
      glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
      glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
      glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glGetIntegerv(GL_READ_BUFFER, &readBuffer);

      if (s_rb.pbo[0] == 0)
        glGenBuffers(kReadBuffers, s_rb.pbo);

      // The oldest buffer is reused: its frame (read kReadBuffers - 1
      // frames ago, so long finished) goes to the queue first.
      const int i = s_rb.next;
      s_rb.next = (i + 1) % kReadBuffers;
      Collect(i);

      glReadBuffer(GL_BACK);
      if (outW != width || outH != height)
      {
        if (s_rb.fbo == 0 || s_rb.fboW != outW || s_rb.fboH != outH)
        {
          if (s_rb.fbo == 0)
          {
            glGenFramebuffers(1, &s_rb.fbo);
            glGenRenderbuffers(1, &s_rb.colour);
          }
          glBindRenderbuffer(GL_RENDERBUFFER, s_rb.colour);
          glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, outW, outH);
          glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_rb.fbo);
          glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_rb.colour);
          s_rb.fboW = outW;
          s_rb.fboH = outH;
        }
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_rb.fbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, outW, outH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_rb.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
      }

      // Read into the buffer without waiting; it's collected later.
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, s_rb.pbo[i]);
      const GLsizeiptr bytes = GLsizeiptr(outW) * outH * 4;
      if (s_rb.w[i] != outW || s_rb.h[i] != outH)
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
      glReadPixels(0, 0, outW, outH, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      s_rb.w[i] = outW;
      s_rb.h[i] = outH;
      s_rb.pending[i] = true;

      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glReadBuffer(readBuffer);
      glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, readFBO);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFBO);
      glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
      if (scissor)
        glEnable(GL_SCISSOR_TEST);
    }

    static int16_t Clamp16(float x)
    {
      if (x > 32767.f)
        return 32767;
      if (x < -32768.f)
        return -32768;
      return (int16_t) x;
    }

    void Audio(unsigned numSamples, const float *leftFront, const float *rightFront, const float *leftRear, const float *rightRear)
    {
      {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_active || s_paused)
          return;
      }
      std::lock_guard<std::mutex> lock(s_audioMutex);
      if (!s_wav)
        return;
      std::vector<uint8_t> buf(size_t(numSamples) * 4);
      for (unsigned i = 0; i < numSamples; i++)
      {
        // Front and rear speakers mixed down to stereo, as with 2 host channels.
        PutLE16(&buf[i * 4 + 0], (uint16_t) Clamp16((leftFront[i] + leftRear[i]) * 0.5f));
        PutLE16(&buf[i * 4 + 2], (uint16_t) Clamp16((rightFront[i] + rightRear[i]) * 0.5f));
      }
      if (fwrite(buf.data(), 1, buf.size(), s_wav) != buf.size())
        return;
      s_wavBytes += (uint32_t) buf.size();
      if (++s_wavFramesSinceHeader >= kFPS)
      {
        s_wavFramesSinceHeader = 0;
        WriteWavHeader();
      }
    }

    void Stop()
    {
      {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_active)
          return;
      }
      // The last frames still being read back (the GL context is current
      // here: Stop() is called from the thread that draws).
      CollectAll();
      FreeReadback();
      {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stopping = true;
      }
      s_cv.notify_all();
      if (s_thread.joinable())
        s_thread.join();
      {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_active = false;
        s_stopping = false;
        s_queue.clear();
      }
      std::lock_guard<std::mutex> lock(s_audioMutex);
      if (s_wav)
      {
        WriteWavHeader();
        fclose(s_wav);
        s_wav = nullptr;
      }
      InfoLog("Recorder: finished.");
    }
  }
}
