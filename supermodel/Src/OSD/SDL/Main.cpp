// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The program: reads Supermodel.ini and the command line, opens the OpenGL
// window, sets up inputs, outputs, sound and the renderers, and runs the
// Model 3 one frame at a time while handling the emulator's own keys (pause,
// save states, volume, full screen...).
//
// Build options: SUPERMODEL_WIN32 (Windows input systems and outputs),
// DEBUG (-gfx-state analysis).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <GL/glew.h>

#ifdef SUPERMODEL_WIN32
#include "DirectInputSystem.h"
#include "WinOutputs.h"
#endif
#include "NetOutputs.h"

#include "Supermodel.h"
#include "Util/Format.h"
#include "Util/NewConfig.h"
#include "Util/ConfigBuilders.h"
#include "Util/BMPFile.h"
#include "OSD/FileSystemPath.h"
#include "OSD/RetroPixels.h"
#include "OSD/Audio.h"
#include "OSD/DefaultConfigFile.h"
#include "GameLoader.h"
#include "SDLInputSystem.h"
#include "SDLIncludes.h"
#include "Graphics/Legacy3D/Legacy3D.h"
#include "Graphics/New3D/New3D.h"
#include "Graphics/New3D/VBO.h"
#include "Graphics/SuperAA.h"
#include "Model3/IEmulator.h"
#include "Model3/Model3.h"
#include "Sound/MPEG/MpegAudio.h"
#include "Crosshair.h"
#include "Gui.h"
#include "Main.h"

#ifdef DEBUG
#include "Model3/Model3GraphicsState.h"
#include "OSD/SDL/PolyAnalysis.h"
#endif

namespace
{
  // Where things are kept (the RetroPixels home folders when it is set).
  std::string ConfigPath() { return FileSystemPath::GetPath(FileSystemPath::Config) + "Supermodel.ini"; }
  std::string GamesXmlPath() { return FileSystemPath::GetPath(FileSystemPath::Data) + "Games.xml"; }
  std::string MusicXmlPath() { return FileSystemPath::GetPath(FileSystemPath::Data) + "Music.xml"; }
  std::string LogPath() { return FileSystemPath::GetPath(FileSystemPath::Log) + "Supermodel.log"; }

  // The settings in force: defaults, then Supermodel.ini (global, then the
  // game's section), then the command line.
  Util::Config::Node s_config("Global");

  // The Model 3's picture is 496x384.
  const unsigned kNativeWidth = 496;
  const unsigned kNativeHeight = 384;

  // The window and where in it the game is drawn.
  struct Screen
  {
    SDL_Window *window = nullptr;
    unsigned x = 0, y = 0;            // corner of the game's picture
    unsigned width = 0, height = 0;   // size of the game's picture
    unsigned fullWidth = 0, fullHeight = 0;   // the whole drawable area
    int supersampling = 1;
    CRTcolor colours = CRTcolor::None;
  };
  Screen s_screen;

  CCrosshair *s_crosshair = nullptr;
  CInputs *s_gunInputs = nullptr;     // set while a light gun game runs
  uint32_t s_gameInputs = 0;

  bool UseDesktopSize()
  {
    return s_config["FullScreen"].ValueAs<bool>() && s_config["DesktopResolution"].ValueAsDefault<bool>(false);
  }

  Uint32 FullScreenFlag()
  {
    // DesktopResolution: cover the desktop as it is, with no mode change.
    return s_config["DesktopResolution"].ValueAsDefault<bool>(false) ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
  }

  // Works out where the game goes in the window (s_screen.width/height hold
  // the size wanted on entry), clears the window and sets the scissor box.
  Result Layout(bool keepAspect)
  {
    Screen &s = s_screen;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(s.window, &winW, &winH);
    if (UseDesktopSize())
    {
      // Measure the drawable: on a scaled desktop it isn't the window size.
      int drawW = 0, drawH = 0;
      SDL_GL_GetDrawableSize(s.window, &drawW, &drawH);
      if (drawW > 0 && drawH > 0)
      {
        winW = drawW;
        winH = drawH;
      }
      s.width = unsigned(winW);
      s.height = unsigned(winH);
    }
    s.fullWidth = unsigned(winW);
    s.fullHeight = unsigned(winH);

    // Shrink one side to keep the Model 3's shape.
    float w = float(s.width), h = float(s.height);
    if (keepAspect)
    {
      const float shape = float(double(kNativeWidth) / double(kNativeHeight));
      if (h < w / shape)
        w = h * shape;
      if (w < h * shape)
        h = w / shape;
    }

    // Centred in the area asked for, and that area centred in the window.
    s.x = (s.width - unsigned(w)) / 2;
    s.y = (s.height - unsigned(h)) / 2;
    if (int(s.width) < winW)
      s.x += (unsigned(winW) - s.width) / 2;
    if (int(s.height) < winH)
      s.y += (unsigned(winH) - s.height) / 2;

    glViewport(0, 0, s.width, s.height);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClearDepth(1.0);
    glDepthFunc(GL_LESS);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    for (int i = 0; i < 3; i++)   // every back buffer black
    {
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      SDL_GL_SwapWindow(s.window);
    }

    s.width = unsigned(w);
    s.height = unsigned(h);

    // The 2D layers are two native pixels off at the edges: clip them.
    const unsigned edge = unsigned(((s.height / double(kNativeHeight)) * 2.0) + 0.5);
    const int aa = s.supersampling;
    glEnable(GL_SCISSOR_TEST);
    if (s_config["WideScreen"].ValueAsDefault<bool>(false))
      glScissor(0, edge * aa, s.fullWidth * aa, (s.fullHeight - edge * 2) * aa);
    else
      glScissor((s.x + edge) * aa, (s.y + edge) * aa, (s.width - edge * 2) * aa, (s.height - edge * 2) * aa);
    return Result::OKAY;
  }

  // RefreshRate in 57.5..58: use a 57 or 58 Hz full screen mode of this size
  // if the display has one (custom modes some graphics drivers allow).
  void UseNearModel3RefreshRate()
  {
    const float hz = std::abs(s_config["RefreshRate"].ValueAs<float>());
    if (!(hz > 57.f && hz < 58.f))
      return;
    const int modes = SDL_GetNumDisplayModes(0);
    for (int i = 0; i < modes; i++)
    {
      SDL_DisplayMode mode;
      if (SDL_GetDisplayMode(0, i, &mode) != 0)
        return;
      const bool size = unsigned(mode.w) == s_screen.fullWidth && unsigned(mode.h) == s_screen.fullHeight;
      if (SDL_BITSPERPIXEL(mode.format) >= 24 && size && (mode.refresh_rate == 57 || mode.refresh_rate == 58))
      {
        if (SDL_SetWindowDisplayMode(s_screen.window, &mode) == 0)
          printf("Custom fullscreen mode set: %ix%i@57.524 Hz\n", mode.w, mode.h);
        return;
      }
    }
  }

  // Opens the one window of the session (DirectInput doesn't like it being
  // made again; it is resized and switched instead).
  Result OpenWindow(bool coreProfile, bool quads, const std::string &title, bool raise, bool keepAspect, bool fullScreen)
  {
    if (s_screen.window)
      return ErrorLog("Internal error: CreateGLScreen() called more than once");
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
      return ErrorLog("Unable to initialize SDL video subsystem: %s\n", SDL_GetError());

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    // the new engine does its own depth and stencil in its own buffers
    const bool new3d = s_config["New3DEngine"].ValueAsDefault<bool>(true);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, new3d ? 0 : 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, new3d ? 0 : 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (coreProfile)
    {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, quads ? 5 : 1);
    }

    const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | (fullScreen ? FullScreenFlag() : 0);
    s_screen.window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, int(s_screen.width), int(s_screen.height), flags);
    if (!s_screen.window)
      return ErrorLog("Unable to create an OpenGL display: %s\n", SDL_GetError());
    if (raise)
      SDL_RaiseWindow(s_screen.window);

    SDL_GLContext context = SDL_GL_CreateContext(s_screen.window);
    if (!context)
      return ErrorLog("Unable to create OpenGL context: %s\n", SDL_GetError());
    SDL_GL_SetSwapInterval(s_config["VSync"].ValueAsDefault<bool>(false) ? 1 : 0);
    SDL_GL_MakeCurrent(s_screen.window, context);

    const GLenum glew = glewInit();
    if (glew != GLEW_OK)
      return ErrorLog("OpenGL initialization failed: %s\n", glewGetErrorString(glew));

    GLint profile = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    printf("GPU info: %s ", glGetString(GL_VERSION));
    if (profile & GL_CONTEXT_CORE_PROFILE_BIT)
      printf("(core profile)");
    if (profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT)
      printf("(compatibility profile)");
    printf("\n\n");

    return Layout(keepAspect);
  }

  void CloseWindow()
  {
    if (!s_screen.window)
      return;
    SDL_GL_DeleteContext(SDL_GL_GetCurrentContext());
    SDL_DestroyWindow(s_screen.window);
  }

  Result SwitchWindow(bool keepAspect, bool fullScreen)
  {
    if (SDL_SetWindowFullscreen(s_screen.window, fullScreen ? FullScreenFlag() : 0) < 0)
      return ErrorLog("Unable to enter %s mode: %s\n", fullScreen ? "fullscreen" : "windowed", SDL_GetError());
    return Layout(keepAspect);
  }

  // The size to ask for: XResolution x YResolution, or the desktop's.
  void WantedSize(unsigned *w, unsigned *h)
  {
    *w = s_config["XResolution"].ValueAs<unsigned>();
    *h = s_config["YResolution"].ValueAs<unsigned>();
    if (!UseDesktopSize())
      return;
    SDL_DisplayMode mode;
    const int display = s_screen.window ? SDL_GetWindowDisplayIndex(s_screen.window) : 0;
    if (SDL_GetDesktopDisplayMode(display < 0 ? 0 : display, &mode) == 0 && mode.w > 0 && mode.h > 0)
    {
      *w = unsigned(mode.w);
      *h = unsigned(mode.h);
    }
  }

  // The driver's details, on the console or into the log.
  void PrintGLInfo(bool openWindow, bool toLog, bool extensions)
  {
    if (openWindow)
    {
      s_screen.width = kNativeWidth;
      s_screen.height = kNativeHeight;
      if (OpenWindow(s_config["New3DEngine"].ValueAs<bool>(), s_config["QuadRendering"].ValueAs<bool>(), "Supermodel - Querying OpenGL Information...", false, false, false) != Result::OKAY)
      {
        ErrorLog("Unable to query OpenGL.\n");
        return;
      }
    }

    auto say = [toLog](const std::string &text)
    {
      if (toLog)
        InfoLog("%s", text.c_str());
      else
        printf("%s\n", text.c_str());
    };
    auto text = [](GLenum name) { const GLubyte *s = glGetString(name); return std::string(s ? (const char *)s : ""); };
    auto number = [](GLenum name) { GLint v = 0; glGetIntegerv(name, &v); return std::to_string(v); };

    say(toLog ? "OpenGL information:" : "OpenGL information:\n");
    say("  Vendor                   : " + text(GL_VENDOR));
    say("  Renderer                 : " + text(GL_RENDERER));
    say("  Version                  : " + text(GL_VERSION));
    say("  Shading Language Version : " + text(GL_SHADING_LANGUAGE_VERSION));
    say("  Maximum Vertex Array Size: " + number(GL_MAX_ELEMENTS_VERTICES) + " vertices");
    say("  Maximum Texture Size     : " + number(GL_MAX_TEXTURE_SIZE) + " texels");
    say("  Maximum Vertex Attributes: " + number(GL_MAX_VERTEX_ATTRIBS));
    say("  Maximum Vertex Uniforms  : " + number(GL_MAX_VERTEX_UNIFORM_COMPONENTS));
    say("  Maximum Texture Img Units: " + number(GL_MAX_TEXTURE_IMAGE_UNITS));
    if (extensions)
    {
      // one per line
      std::string all = text(GL_EXTENSIONS);
      std::string prefix = "  Supported Extensions     : ";
      size_t at = 0;
      while ((at = all.find_first_not_of(" \t\n", at)) != std::string::npos)
      {
        size_t end = all.find_first_of(" \t\n", at);
        say(prefix + all.substr(at, end == std::string::npos ? std::string::npos : end - at));
        prefix = std::string(prefix.size(), ' ');
        at = end;
      }
    }
    say("");
  }

  // The whole drawable area as a bitmap.
  void SaveFrameBuffer(const std::string &file)
  {
    const unsigned w = s_screen.fullWidth, h = s_screen.fullHeight;
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    Util::WriteSurfaceToBMP<Util::RGBA8>(file, pixels.data(), w, h, true);
  }

  // Screenshots/Screenshot_<date>_(<time>).bmp
  void Screenshot()
  {
    const time_t now = std::time(nullptr);
    char name[64];
    strftime(name, sizeof(name), "Screenshot_%Y-%m-%d_(%H-%M-%S).bmp", std::localtime(&now));
    const std::string file = FileSystemPath::GetPath(FileSystemPath::Screenshots) + name;
    std::cout << "Screenshot created: " << file << std::endl;
    SaveFrameBuffer(file);
  }

#ifdef DEBUG
  // -gfx-state: draws the loaded state once per highlighted bit, saves each
  // picture in GraphicsAnalysis/ and writes a page to browse them.
  std::string s_gfxStatePath;

  void AnalysePolygonBits(IEmulator *emulator)
  {
    const std::string folder = "GraphicsAnalysis/";
    std::string base = s_gfxStatePath;
    for (char sep : { '/', '\\' })
    {
      const size_t at = s_gfxStatePath.find_last_of(sep);
      if (at != std::string::npos)
        base = s_gfxStatePath.substr(at + 1);
    }

    // The bits whose meaning is still being looked into.
    const std::vector<uint32_t> polyBits = { 0xffffffff, 0x000000ab, 0x000000fc, 0x000000c0, 0x000000a0, 0xffffff60, 0xff0300ff };
    const std::vector<uint32_t> cullingBits = { 0xffffffff, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    GLint readBuffer = 0;
    glGetIntegerv(GL_READ_BUFFER, &readBuffer);
    glReadBuffer(GL_FRONT);
    s_config.Set("Debug/ForceFlushModels", true);

    auto render = [&](const char *kind, const std::vector<uint32_t> &bits, const char *idxKey, const char *maskKey)
    {
      for (int word = 0; word < int(bits.size()); word++)
      {
        for (int bit = 0; bit < 32; bit++)
        {
          const uint32_t mask = uint32_t(1) << bit;
          s_config.Set(idxKey, word);
          s_config.Set(maskKey, mask);
          if (bits[word] & mask)
          {
            emulator->RenderFrame();
            SaveFrameBuffer(folder + PolyAnalysis::ImageName(base, kind, word, mask));
          }
        }
      }
    };
    render("poly", polyBits, "Debug/HighlightPolyHeaderIdx", "Debug/HighlightPolyHeaderMask");
    render("culling", cullingBits, "Debug/HighlightCullingNodeIdx", "Debug/HighlightCullingNodeMask");
    glReadBuffer(readBuffer);

    const std::string page = folder + "_" + base + ".html";
    std::ofstream out(page);
    if (!out.good())
    {
      ErrorLog("Unable to open '%s' for writing.", page.c_str());
      return;
    }
    out << PolyAnalysis::Page(base, polyBits, cullingBits);
    printf("Produced: %s\n", page.c_str());
  }
#endif

  // Save states and NVRAM are block files: a header block ("Supermodel Save
  // State" / "Supermodel NVRAM State") holding the format version and the
  // ROM set's name, then each part of the machine's own blocks.
  const int32_t kStateVersion = 6;
  const int32_t kNVRAMVersion = 0;
  const char kStateBlock[] = "Supermodel Save State";
  const char kNVRAMBlock[] = "Supermodel NVRAM State";
  unsigned s_slot = 0;   // 0..9

  std::string StateFile(IEmulator *emulator)
  {
    return FileSystemPath::GetPath(FileSystemPath::Saves) + emulator->GetGame().name + ".st" + std::to_string(s_slot);
  }

  std::string NVRAMFile(IEmulator *emulator)
  {
    return FileSystemPath::GetPath(FileSystemPath::NVRAM) + emulator->GetGame().name + ".nv";
  }

  bool StartFile(CBlockFile &file, const std::string &path, const char *block, int32_t version, IEmulator *emulator)
  {
    if (file.Create(path, block, "Supermodel Version " SUPERMODEL_VERSION) != Result::OKAY)
      return false;
    file.Write(&version, sizeof(version));
    file.Write(emulator->GetGame().name);
    return true;
  }

  // Opens a file and checks its header; says why if it can't be used
  // (except that a missing file is left unsaid when quietIfMissing).
  bool OpenFile(CBlockFile &file, const std::string &path, const char *block, int32_t version, const char *kind, bool quietIfMissing)
  {
    if (file.Load(path) != Result::OKAY)
    {
      if (!quietIfMissing)
        ErrorLog("Unable to load state from '%s'.", path.c_str());
      return false;
    }
    if (file.FindBlock(block) != Result::OKAY)
    {
      ErrorLog("'%s' does not appear to be a valid %s file.", path.c_str(), kind);
      return false;
    }
    int32_t found = 0;
    file.Read(&found, sizeof(found));
    if (found != version)
    {
      ErrorLog("'%s' is incompatible with this version of Supermodel.", path.c_str());
      return false;
    }
    return true;
  }

  void SaveState(IEmulator *emulator)
  {
    CBlockFile file;
    const std::string path = StateFile(emulator);
    if (!StartFile(file, path, kStateBlock, kStateVersion, emulator))
    {
      ErrorLog("Unable to save state to '%s'.", path.c_str());
      return;
    }
    emulator->SaveState(&file);
    file.Close();
    printf("Saved state to '%s'.\n", path.c_str());
    InfoLog("Saved state to '%s'.", path.c_str());
  }

  void LoadState(IEmulator *emulator, std::string path = std::string())
  {
    if (path.empty())
      path = StateFile(emulator);
    CBlockFile file;
    if (!OpenFile(file, path, kStateBlock, kStateVersion, "save state", false))
      return;
    emulator->LoadState(&file);
    file.Close();
    printf("Loaded state from '%s'.\n", path.c_str());
    InfoLog("Loaded state from '%s'.", path.c_str());
  }

  void SaveNVRAM(IEmulator *emulator)
  {
    CBlockFile file;
    const std::string path = NVRAMFile(emulator);
    if (!StartFile(file, path, kNVRAMBlock, kNVRAMVersion, emulator))
    {
      ErrorLog("Unable to save NVRAM to '%s'. Make sure directory exists!", path.c_str());
      return;
    }
    emulator->SaveNVRAM(&file);
    file.Close();
    InfoLog("Saved NVRAM to '%s'.", path.c_str());
  }

  void LoadNVRAM(IEmulator *emulator)
  {
    CBlockFile file;
    const std::string path = NVRAMFile(emulator);
    if (!OpenFile(file, path, kNVRAMBlock, kNVRAMVersion, "NVRAM", true))   // none yet is normal
      return;
    emulator->LoadNVRAM(&file);
    file.Close();
    InfoLog("Loaded NVRAM from '%s'.", path.c_str());
  }

  // Waits for the performance counter to reach target: sleeps while more
  // than 2 ms are left, then spins.
  void WaitUntil(uint64_t target, uint64_t perSecond)
  {
    uint64_t now = SDL_GetPerformanceCounter();
    if (now >= target)
      return;
    while (int64_t(target - now) * 1000 > 2 * int64_t(perSecond))
    {
      SDL_Delay(1);
      now = SDL_GetPerformanceCounter();
    }
    while (int64_t(target - SDL_GetPerformanceCounter()) > 0)
    {
#ifdef SDL_CPUPauseInstruction
      SDL_CPUPauseInstruction();
#endif
    }
  }

  // The renderers and their supersampling target, rebuilt whenever the
  // window's size changes.
  struct Renderers
  {
    SuperAA *aa = nullptr;
    CRender2D *r2d = nullptr;
    IRender3D *r3d = nullptr;
    UpscaleMode upscale = UpscaleMode(0);

    ~Renderers()
    {
      delete r2d;
      delete r3d;
      delete aa;
    }

    // (Re)starts them on the current layout.
    bool Start(IEmulator *emulator)
    {
      const Screen &s = s_screen;
      const int k = s.supersampling;
      if (r2d->Init(s.x * k, s.y * k, s.width * k, s.height * k, s.fullWidth * k, s.fullHeight * k, aa->GetTargetID(), upscale) != Result::OKAY)
        return false;
      if (r3d->Init(s.x * k, s.y * k, s.width * k, s.height * k, s.fullWidth * k, s.fullHeight * k, aa->GetTargetID()) != Result::OKAY)
        return false;
      emulator->AttachRenderers(r2d, r3d, aa);
      return true;
    }

    // After the window changed: a new 2D renderer (the old one's GL objects
    // may be gone), the others started again.
    bool Rebuild(IEmulator *emulator)
    {
      aa->Init(s_screen.fullWidth, s_screen.fullHeight);
      r2d = new CRender2D(s_config);
      return Start(emulator);
    }
  };

  void SetTitle(const std::string &title)
  {
    SDL_SetWindowTitle(s_screen.window, title.c_str());
  }

  // Turns a volume setting up (to at most 200%) or down (to at least 0) by
  // 10 and reports it.
  void StepVolume(const char *key, const char *what, bool up)
  {
    const int now = s_config[key].ValueAs<int>();
    const int volume = up ? std::min(200, now + 10) : std::max(0, now - 10);
    s_config.Get(key).SetValue(volume);
    const char *limit = up ? (volume == 200 ? " (maximum)" : "") : (volume == 0 ? " (muted)" : "");
    printf("%s volume: %d%%%s\n", what, volume, limit);
  }

  // Runs the game until the player quits. 0 when it ended normally.
  int RunGame(const Game &game, ROMSet *romSet, IEmulator *emulator, CInputs *inputs, COutputs *outputs
    )
  {
    if (emulator->Init() != Result::OKAY)
      return 1;
    if (emulator->LoadGame(game, *romSet) != Result::OKAY)
      return 1;
    *romSet = ROMSet();   // the ROMs are in the emulator now

    MpegDec::LoadCustomTracks(MusicXmlPath(), game);
    LoadNVRAM(emulator);

    // The window, at the size and place asked for.
    const std::string title = "Supermodel - " + game.title;
    WantedSize(&s_screen.width, &s_screen.height);
    s_screen.fullWidth = s_screen.width;
    s_screen.fullHeight = s_screen.height;
    SetTitle(title);
    SDL_SetWindowSize(s_screen.window, int(s_screen.fullWidth), int(s_screen.fullHeight));
    SDL_SetWindowPosition(s_screen.window, s_config["WindowXPosition"].ValueAsDefault<int>(SDL_WINDOWPOS_CENTERED), s_config["WindowYPosition"].ValueAsDefault<int>(SDL_WINDOWPOS_CENTERED));
    if (s_config["BorderlessWindow"].ValueAs<bool>())
      SDL_SetWindowBordered(s_screen.window, SDL_FALSE);
    UseNearModel3RefreshRate();
    if (SwitchWindow(!s_config["Stretch"].ValueAs<bool>(), s_config["FullScreen"].ValueAs<bool>()) != Result::OKAY)
      return 1;
    PrintGLInfo(false, true, false);

    SetAudioType(game.audio);
    if (OpenAudio(s_config) != Result::OKAY)
      return 1;

    // RetroPixels: recording (-rp-record with -rp-ffmpeg)
    {
      auto setting = [](const char *key)
      {
        const Util::Config::Node *node = s_config.TryGet(key);
        return node && !node->Empty() ? node->ValueAs<std::string>() : std::string();
      };
      const std::string dir = setting("RPRecordDir"), ffmpeg = setting("RPFFmpeg");
      if (!dir.empty() && !ffmpeg.empty())
        RetroPixels::Recorder::Start(dir, ffmpeg);
      else if (!dir.empty() || !ffmpeg.empty())
        ErrorLog("-rp-record and -rp-ffmpeg go together; this game isn't being recorded.");
    }

    // No pointer in full screen; crosshairs for gun games.
    inputs->GetInputSystem()->SetMouseVisibility(!s_config["FullScreen"].ValueAs<bool>());
    const bool lightGuns = (game.inputs & (Game::INPUT_GUN1 | Game::INPUT_GUN2)) != 0 || game.name == "lostwsga";
    s_gameInputs = game.inputs;
    s_gunInputs = lightGuns ? inputs : nullptr;

    emulator->AttachInputs(inputs);
    if (outputs)
      emulator->AttachOutputs(outputs);

    const uint64_t perSecond = SDL_GetPerformanceFrequency();
    const uint64_t milliHz = uint64_t(1000.0 * std::abs(s_config["RefreshRate"].ValueAs<float>()));
    const uint64_t perFrame = perSecond * 1000 / milliHz;
    uint64_t nextFrame = 0;

    Renderers r;
    r.aa = new SuperAA(s_screen.supersampling, s_screen.colours);
    r.aa->Init(s_screen.fullWidth, s_screen.fullHeight);
    r.r2d = new CRender2D(s_config);
    if (s_config["New3DEngine"].ValueAs<bool>())
      r.r3d = new New3D::CNew3D(s_config, emulator->GetGame().name);
    else
      r.r3d = new Legacy3D::CLegacy3D(s_config);
    r.upscale = UpscaleMode(s_config["UpscaleMode"].ValueAs<int>());

    auto fail = [&]()
    {
      RetroPixels::Recorder::Stop();
      return 1;
    };
    if (!r.Start(emulator))
      return fail();

    emulator->Reset();
    const std::string firstState = s_config["InitStateFile"].ValueAs<std::string>();
    if (!firstState.empty())
      LoadState(emulator, firstState);


    bool quit = false;
    bool paused = false;
    bool timings = false;
    unsigned framesCounted = 0;
    uint64_t countStart = SDL_GetPerformanceCounter();
    int seenW = 0, seenH = 0, seenFrames = 0;   // a size someone else gave the window

#ifdef DEBUG
    if (dynamic_cast<CModel3GraphicsState *>(emulator))
    {
      AnalysePolygonBits(emulator);
      quit = true;
    }
#endif

    // Runs an action with the emulator's threads and sound stopped (unless
    // already paused).
    auto whileStopped = [&](const std::function<void()> &action)
    {
      if (!paused)
      {
        emulator->PauseThreads();
        SetAudioEnabled(false);
      }
      action();
      if (!paused)
      {
        emulator->ResumeThreads();
        SetAudioEnabled(true);
      }
    };

    // The emulator's own keys: the first one pressed this frame is handled.
    struct Key
    {
      std::shared_ptr<CSwitchInput> input;
      std::function<bool()> allowed;   // checked after the key
      std::function<void()> action;
    };
    const auto always = []() { return true; };
    bool failed = false;
    std::vector<Key> keys =
    {
      { inputs->uiExit, always, [&]() { quit = true; } },
      { inputs->uiReset, always, [&]()
        {
          whileStopped([&]()
          {
            emulator->Reset();
          });
          puts("Model 3 reset.");
        } },
      { inputs->uiPause, always, [&]()
        {
          paused = !paused;
          RetroPixels::Recorder::SetPaused(paused);
          if (paused)
          {
            emulator->PauseThreads();
            SetAudioEnabled(false);
            SetTitle(title + " (Paused)");
          }
          else
          {
            emulator->ResumeThreads();
            SetAudioEnabled(true);
            SetTitle(title);
          }
          if (outputs)
            outputs->SetValue(OutputPause, paused);
        } },
      { inputs->uiFullScreen, always, [&]()
        {
          s_config.Get("FullScreen").SetValue(!s_config["FullScreen"].ValueAs<bool>());
          // the GL context may be lost on the switch
          delete r.r2d;
          r.r2d = nullptr;
          WantedSize(&s_screen.width, &s_screen.height);
          s_screen.fullWidth = s_screen.width;
          s_screen.fullHeight = s_screen.height;
          const bool full = s_config["FullScreen"].ValueAs<bool>();
          if (SwitchWindow(!s_config["Stretch"].ValueAs<bool>(), full) != Result::OKAY || !r.Rebuild(emulator))
          {
            failed = true;
            return;
          }
          inputs->GetInputSystem()->SetMouseVisibility(!s_config["FullScreen"].ValueAs<bool>());
        } },
      { inputs->uiSaveState, always, [&]() { whileStopped([&]() { SaveState(emulator); }); } },
      { inputs->uiChangeSlot, always, [&]()
        {
          s_slot = (s_slot + 1) % 10;
          printf("Save slot: %d\n", s_slot);
        } },
      { inputs->uiLoadState, always, [&]()
        {
          whileStopped([&]()
          {
            LoadState(emulator);
          });
        } },
      { inputs->uiMusicVolUp, always, [&]()
        {
          if (emulator->GetGame().mpeg_board.empty())
            puts("This game does not have an MPEG music board.");
          else
            StepVolume("MusicVolume", "Music", true);
        } },
      { inputs->uiMusicVolDown, always, [&]()
        {
          if (emulator->GetGame().mpeg_board.empty())
            puts("This game does not have an MPEG music board.");
          else
            StepVolume("MusicVolume", "Music", false);
        } },
      { inputs->uiSoundVolUp, always, [&]() { StepVolume("SoundVolume", "Sound", true); } },
      { inputs->uiSoundVolDown, always, [&]() { StepVolume("SoundVolume", "Sound", false); } },
      { inputs->uiSelectCrosshairs, [&]() { return lightGuns; }, [&]()
        {
          // none -> P1 -> P2 -> both -> none
          const int which = int((s_config["Crosshairs"].ValueAs<unsigned>() + 1) & 3);
          s_config.Get("Crosshairs").SetValue(which);
          static const char *const said[4] = { "Crosshairs disabled.", "Showing Player 1 crosshair only.", "Showing Player 2 crosshair only.", "Crosshairs enabled." };
          puts(said[which]);
        } },
      { inputs->uiClearNVRAM, always, [&]()
        {
          emulator->ClearNVRAM();
          puts("NVRAM cleared.");
        } },
      { inputs->uiToggleFrLimit, always, [&]()
        {
          s_config.Get("Throttle").SetValue(!s_config["Throttle"].ValueAs<bool>());
          printf("Frame limiting: %s\n", s_config["Throttle"].ValueAs<bool>() ? "On" : "Off");
        } },
      { inputs->uiScreenshot, always, [&]() { Screenshot(); } },
    };

    while (!quit && !failed)
    {
      if (!inputs->Poll(&game, s_screen.x, s_screen.y, s_screen.width, s_screen.height))
        quit = true;

      if (paused)
        emulator->RenderFrame();
      else
        emulator->RunFrame();

      bool handleKeys = true;
      if (handleKeys)
      {
        bool handled = false;
        for (const Key &key : keys)
        {
          if (key.input->Pressed() && key.allowed())
          {
            key.action();
            handled = true;
            break;
          }
        }
        (void)handled;
        if (failed)
          break;
      }

      // RetroPixels: when the window is resized from outside (RetroPixels
      // docks it in its own window), follow once the size has held for three
      // frames.
      if (!s_config["FullScreen"].ValueAs<bool>())
      {
        int w = 0, h = 0;
        SDL_GetWindowSize(s_screen.window, &w, &h);
        const bool changed = w > 0 && h > 0 && (unsigned(w) != s_screen.fullWidth || unsigned(h) != s_screen.fullHeight);
        if (!changed)
          seenFrames = 0;
        else
        {
          if (w == seenW && h == seenH)
            seenFrames++;
          else
          {
            seenW = w;
            seenH = h;
            seenFrames = 0;
          }
          if (seenFrames >= 3)
          {
            seenFrames = 0;
            s_config.Get("XResolution").SetValue(w);
            s_config.Get("YResolution").SetValue(h);
            delete r.r2d;
            r.r2d = nullptr;
            s_screen.fullWidth = s_screen.width = unsigned(w);
            s_screen.fullHeight = s_screen.height = unsigned(h);
            if (SwitchWindow(!s_config["Stretch"].ValueAs<bool>(), false) != Result::OKAY || !r.Rebuild(emulator))
            {
              failed = true;
              break;
            }
          }
        }
      }

      // frame limiting (always while paused)
      if (paused || s_config["Throttle"].ValueAs<bool>())
      {
        WaitUntil(nextFrame, perSecond);
        nextFrame = SDL_GetPerformanceCounter() + perFrame;
      }

      // frames per second in the title, once a second
      const uint64_t now = SDL_GetPerformanceCounter();
      if (s_config["ShowFrameRate"].ValueAs<bool>())
      {
        framesCounted++;
        const uint64_t elapsed = now - countStart;
        if (elapsed >= perSecond)
        {
          char fps[160];
          snprintf(fps, sizeof(fps), "%s - %1.3f FPS%s", title.c_str(), double(framesCounted) / (double(elapsed) / double(perSecond)), paused ? " (Paused)" : "");
          SetTitle(fps);
          countStart = now;
          framesCounted = 0;
        }
      }

      if (timings && !paused)
      {
        if (CModel3 *model3 = dynamic_cast<CModel3 *>(emulator))
          model3->DumpTimings();
      }
    }

    if (failed)
      return fail();

    emulator->PauseThreads();
    SaveNVRAM(emulator);
    RetroPixels::Recorder::Stop();
    CloseAudio();
    return 0;
  }

  // Supermodel.ini is made on first start, never shipped, so an update
  // can't overwrite the player's settings.
  void WriteConfigFileIfMissing()
  {
    const std::string path = ConfigPath();
    if (FILE *existing = fopen(path.c_str(), "r"))
    {
      fclose(existing);
      return;
    }
    FILE *out = fopen(path.c_str(), "w");
    if (!out)
    {
      ErrorLog("Unable to write default configuration file to %s", path.c_str());
      return;
    }
    fputs(DefaultSettings::IniText().c_str(), out);
    fclose(out);
    InfoLog("Wrote default configuration file to %s", path.c_str());
  }

  // Loads the input assignments; with -config-inputs, lets the player set
  // them and saves them in Supermodel.ini (in the game's section when a game
  // was named, otherwise Global).
  Result SetUpInputs(CInputs *inputs, Util::Config::Node *fileConfig, Util::Config::Node *config, const Game &game, bool configure)
  {
    inputs->LoadFromConfig(*config);
    if (!configure)
      return Result::OKAY;

    SetTitle(game.name.empty() ? std::string("Supermodel - Configuring Default Inputs...") : "Supermodel - Configuring Inputs for: " + game.title);
    Util::Config::Node *section = game.name.empty() ? fileConfig : fileConfig->TryGet(game.name);
    if (!section)
      section = &fileConfig->Add(game.name);

    if (inputs->ConfigureInputs(game, s_screen.x, s_screen.y, s_screen.width, s_screen.height))
    {
      inputs->StoreToConfig(section);
      Util::Config::WriteINIFile(ConfigPath(), *fileConfig, ";\n; Supermodel.ini: settings for the Model 3 engine\n;\n");
      inputs->StoreToConfig(config);   // and for this run
    }
    else
      puts("Configuration aborted...");
    puts("");
    return Result::OKAY;
  }

  void PrintGames(const std::string &xmlFile, const std::map<std::string, Game> &games)
  {
    if (games.empty())
    {
      puts("No games defined.");
      return;
    }
    printf("Games defined in %s:\n\n", xmlFile.c_str());
    printf("    %-17sTitle\n", "ROM Set");
    printf("    %-17s-----\n", "-------");
    for (const auto &entry : games)
    {
      const Game &g = entry.second;
      const std::string title = g.version.empty() ? g.title : g.title + " (" + g.version + ")";
      printf("    %-17s%s\n", g.name.c_str(), title.c_str());
    }
  }

  void LogSettings(const Util::Config::Node &config)
  {
    InfoLog("Runtime configuration:");
    for (const auto &setting : config)
      InfoLog("  %s=%s", setting.Key().c_str(), setting.Empty() ? "<empty>" : setting.ValueAs<std::string>().c_str());
    InfoLog("");
  }
}

Util::Config::Node DefaultConfig()
{
  using Util::Config::Node;
  Node config("Global");

  // A setting shown in the settings window's tab for group; ranges and
  // choices are what the window offers.
  auto text = [&config](const char *key, const char *value, const char *group, const std::vector<std::string> &choices = {})
  {
    config.Set<std::string>(key, value, group, "", "", choices);
  };

  config.Set("GameXMLFile", GamesXmlPath());
  config.Set("InitStateFile", "");

  // Emulation
  config.Set("PowerPCFrequency", 0u, "Core", 0u, 200u);   // MHz; 0: the game's own
  config.Set("MultiThreaded", true, "Core");
  config.Set("GPUMultiThreaded", true, "Core");

  // Legacy 3D engine
  config.Set("MultiTexture", false, "Legacy3D");
  text("VertexShader", "", "Legacy3D");
  text("FragmentShader", "", "Legacy3D");

  // Sound
  config.Set("EmulateSound", true, "Sound");
  config.Set("Balance", 0.0f, "Sound", -100.f, 100.f);
  config.Set("BalanceLeftRight", 0.0f, "Sound", -100.f, 100.f);
  config.Set("BalanceFrontRear", 0.0f, "Sound", -100.f, 100.f);
  config.Set("NbSoundChannels", 4, "Sound", 0, 0, { 1, 2, 4 });
  config.Set("SoundFreq", 57.6f, "Sound", 0.0f, 0.0f, { 57.524160f, 60.f });
  config.Set("EmulateDSB", true, "Sound");
  config.Set("SoundVolume", 100, "Sound", 0, 200);
  config.Set("MusicVolume", 100, "Sound", 0, 200);
  config.Set("LegacySoundDSP", false, "Sound");
  config.Set("ForceFeedback", false, "ForceFeedback");

  // Picture and window
  config.Set("New3DEngine", true, "Video");
  config.Set("QuadRendering", false, "Video");
  config.Set("XResolution", int(kNativeWidth), "Video");
  config.Set("YResolution", int(kNativeHeight), "Video");
  config.SetEmpty("WindowXPosition");
  config.SetEmpty("WindowYPosition");
  config.Set("FullScreen", false, "Video");
  config.Set("BorderlessWindow", false, "Video");
  config.Set("Supersampling", 1, "Video", 1, 8);
  config.Set("CRTcolors", 0, "Video", 0, 0, { 0, 1, 2, 3, 4, 5 });
  config.Set("UpscaleMode", 2, "Video", 0, 0, { 0, 1, 2, 3 });
  config.Set("WideScreen", false, "Video");
  config.Set("Stretch", false, "Video");
  config.Set("WideBackground", false, "Video");
  config.Set("DesktopResolution", false, "Video");   // RetroPixels
  text("RPRecordDir", "", "RetroPixels");            // RetroPixels: -rp-record
  text("RPFFmpeg", "", "RetroPixels");               // RetroPixels: -rp-ffmpeg
  config.Set("VSync", true, "Video");
  config.Set("Throttle", true, "Video");
  config.Set("RefreshRate", 60.0f, "Video", 0.0f, 0.0f, { 57.5f, 60.f });
  config.Set("ShowFrameRate", false, "Video");
  config.Set("Crosshairs", 0, "Video", 0, 0, { 0, 1, 2, 3 });
  text("CrosshairStyle", "vector", "Video", { "bmp", "vector" });
  config.Set("NoWhiteFlash", false, "Video");
  config.Set("FlipStereo", false, "Sound");

  // Input system and force feedback strengths (percent)
#ifdef SUPERMODEL_WIN32
  text("InputSystem", "dinput", "Core", { "sdl", "sdlgamepad", "dinput", "xinput", "rawinput" });
  for (const char *key : { "DirectInputConstForceLeftMax", "DirectInputConstForceRightMax", "DirectInputSelfCenterMax", "DirectInputFrictionMax", "DirectInputVibrateMax" })
    config.Set(key, 100, "ForceFeedback", 0, 100);
  config.Set("XInputConstForceThreshold", 30, "ForceFeedback", 0, 100);
  config.Set("XInputConstForceMax", 100, "ForceFeedback", 0, 100);
  config.Set("XInputVibrateMax", 100, "ForceFeedback", 0, 100);
  config.Set("XInputStereoVibration", true, "ForceFeedback");
#else
  text("InputSystem", "sdl", "Core", { "sdl", "sdlgamepad" });
#endif
  for (const char *key : { "SDLConstForceMax", "SDLSelfCenterMax", "SDLFrictionMax", "SDLVibrateMax" })
    config.Set(key, 100, "ForceFeedback", 0, 100);
  config.Set("SDLConstForceThreshold", 30, "ForceFeedback", 0, 100);

  // Linked cabinets
  config.Set("Network", false, "Network");
  config.Set("SimulateNet", true, "Network");
  config.Set("PortIn", 1970u, "Network");
  config.Set("PortOut", 1971u, "Network");
  text("AddressOut", "127.0.0.1", "Network");

  // Cabinet outputs (lamps, motor) for other programs
#ifdef SUPERMODEL_WIN32
  text("Outputs", "none", "Misc", { "none", "win", "net" });
#else
  text("Outputs", "none", "Misc", { "none", "net" });
#endif
  config.Set<bool>("OutputsWithLF", false, "Misc");
  config.Set<unsigned>("OutputsTCPPort", 8000, "Misc", 1024, 65535);
  config.Set<unsigned>("OutputsUDPBroadcastPort", 8001, "Misc", 1024, 65535);
  config.Set("DumpMemory", false, "Misc");
  config.Set("DumpTextures", false, "Misc");

  // How digital controls drive analog ones, and mouse dead zones
  for (const char *key : { "InputDigitalSensitivity", "InputDigitalDecaySpeed", "InputKeySensitivity", "InputKeyDecaySpeed" })
  {
    const bool decay = strstr(key, "Decay") != nullptr;
    config.Set<unsigned>(key, decay ? DEFAULT_DIGITAL_DECAYSPEED : DEFAULT_DIGITAL_SENSITIVITY, "Sensitivity", 0, 100);
  }
  for (const char *key : { "InputMouseXDeadZone", "InputMouseYDeadZone", "InputMouseZDeadZone" })
    config.Set<unsigned>(key, DEFAULT_MSE_DEADZONE, "Sensitivity", 0, 100);

  // Controls
  for (const DefaultSettings::InputDefault &input : DefaultSettings::kInputs)
    config.Set<std::string>(std::string("Input") + input.name, input.mapping, "Input", "", "");

  return config;
}

namespace
{
  void Title()
  {
    puts("Supermodel: A Sega Model 3 Arcade Emulator (Version " SUPERMODEL_VERSION ")");
    puts("Copyright 2003-2026 by The Supermodel Team");
  }

  void Help()
  {
    const Util::Config::Node defaults = DefaultConfig();
    auto option = [](const char *name, const std::string &text) { printf("  %-24s%s\n", name, text.c_str()); };
    auto more = [](const std::string &text) { printf("  %-24s%s\n", "", text.c_str()); };
    auto heading = [](const char *name) { printf("\n%s:\n", name); };

    puts("Usage: Supermodel <romset> [options]");
    puts("The ROM set is a ZIP file holding one game.");

    heading("General");
    option("-?, -h, -help, --help", "Show this text");
    option("-print-games", "List the games Games.xml knows and quit");
    option("-game-xml-file=<file>", "Game definitions [Default: " + GamesXmlPath() + "]");
    option("-log-output=<outputs>", "Where the log goes [Default: " + LogPath() + "]");
    option("-log-level=<level>", "Least important messages logged [Default: info]");

    heading("Emulation");
    option("-ppc-frequency=<mhz>", "PowerPC clock (the game's own if not given)");
    option("-no-threads", "Run everything on one thread");
    option("-gpu-multi-threaded", "Draw on a thread of its own [Default]");
    option("-no-gpu-thread", "Draw on the main thread");
    option("-load-state=<file>", "Load this save state once started");

    heading("Picture");
    option("-res=<x>,<y>", "Window size [Default: 496,384]");
    option("-ss=<n>", "Supersampling, 1 to 8");
    option("-window-pos=<x>,<y>", "Window position [Default: centered]");
    option("-window", "In a window [Default]");
    option("-borderless", "In a window with no border");
    option("-fullscreen", "Full screen");
    option("-desktop-res", "Full screen at the desktop's own resolution");
    option("-wide-screen", "Widen the 3D view to the screen's width");
    option("-wide-bg", "With -wide-screen, widen the 2D background too");
    option("-stretch", "Fill the window, whatever its shape");
    option("-upscalemode=<n>", "2D layer smoothing, 0 to 3");
    option("-crtcolors=<n>", "Arcade monitor colours, 0 to 5");
    option("-no-throttle", "Don't hold the frame rate");
    option("-vsync", "Wait for the screen's refresh [Default]");
    option("-no-vsync", "Don't wait for the screen's refresh");
    option("-true-hz", "Run at the Model 3's real 57.524 Hz");
    option("-show-fps", "Frame rate in the title bar");
    option("-crosshairs=<n>", "Gun crosshairs: 0 none [Default], 1 player 1,");
    more("2 player 2, 3 both");
    option("-crosshair-style=<s>", "vector or bmp [Default: vector]");
    option("-new3d", "New 3D engine [Default]");
    option("-quad-rendering", "Draw quads as quads");
    option("-legacy3d", "Legacy 3D engine (faster, less exact)");
    option("-multi-texture", "Legacy engine: decode into 8 texture maps");
    option("-no-multi-texture", "Legacy engine: one texture map [Default]");
    option("-no-white-flash", "No white screen when a game turns 3D off");
    option("-vert-shader=<file>", "Legacy engine: vertex shader to use");
    option("-frag-shader=<file>", "Legacy engine: fragment shader to use");
    option("-print-gl-info", "Show the OpenGL driver's details and quit");

    heading("Sound");
    option("-sound-volume=<vol>", "Sound effects volume in % (with a Digital Sound");
    more("Board) [Default: 100]");
    option("-music-volume=<vol>", "Digital Sound Board volume in % [Default: 100]");
    option("-balance=<bal>", "Front/rear balance in % [Default: 0]");
    option("-channels=<c>", "Output channels: 1, 2 or 4 [Default: 4]");
    option("-flip-stereo", "Swap left and right");
    option("-no-sound", "No sound board (sound effects)");
    option("-no-dsb", "No Digital Sound Board (music)");
    option("-new-scsp", "SCSP sound engine based on MAME's [Default]");
    option("-legacy-scsp", "Older SCSP sound engine");

    heading("Linked cabinets");
    option("-no-net", "No net board [Default]");
    option("-net", "Net board");
    option("-simulate-netboard", "Simulate the net board [Default]");
    option("-emulate-netboard", "Emulate the net board (needs -no-threads)");

    heading("Controls");
    option("-force-feedback", "Force feedback (DirectInput, XInput)");
    option("-config-inputs", "Set up keyboards, mice and game controllers");
#ifdef SUPERMODEL_WIN32
    option("-input-system=<s>", "Input system [Default: " + defaults["InputSystem"].ValueAs<std::string>() + "]");
#endif
    option("-print-inputs", "Show the control assignments");

    heading("Outputs");
    option("-outputs=<s>", "Lamps and motor for other programs [Default: " + defaults["Outputs"].ValueAs<std::string>() + "]");

    heading("RetroPixels");
    option("-rp-record=<dir>", "Record picture and sound into <dir>");
    more("(Frames/*.mkv and Audio/*.wav; needs -rp-ffmpeg)");
    option("-rp-ffmpeg=<file>", "The ffmpeg program that encodes the recording");
    option("SUPERMODEL_HOME=<dir>", "(environment) Keep Supermodel.ini, NVRAM, saves,");
    more("the log and screenshots in <dir>");

    heading("Debugging");
    option("-dump-memory", "Write the memory regions to files on exit");
    option("-dump-textures", "Write the textures to bitmaps on exit");
#ifdef DEBUG
    option("-gfx-state=<file>", "Graphics analysis of a save state (legacy 3D");
    more("engine; needs a GraphicsAnalysis folder)");
#endif
    puts("");
  }

  struct CommandLine
  {
    Util::Config::Node config{ "CommandLine" };
    std::vector<std::string> romFiles;
    bool error = false;
    bool help = false;
    bool printGames = false;
    bool printGLInfo = false;
    bool configInputs = false;
    bool printInputs = false;
#ifdef DEBUG
    std::string gfxState;
#endif
  };

  // -option=value: the setting it sets.
  const std::map<std::string, std::string> kValueOptions =
  {
    { "-game-xml-file", "GameXMLFile" }, { "-load-state", "InitStateFile" },
    { "-ppc-frequency", "PowerPCFrequency" }, { "-crosshairs", "Crosshairs" },
    { "-crosshair-style", "CrosshairStyle" }, { "-vert-shader", "VertexShader" },
    { "-frag-shader", "FragmentShader" }, { "-sound-volume", "SoundVolume" },
    { "-music-volume", "MusicVolume" }, { "-balance", "Balance" },
    { "-channels", "NbSoundChannels" }, { "-soundfreq", "SoundFreq" },
    { "-input-system", "InputSystem" }, { "-outputs", "Outputs" },
    { "-log-output", "LogOutput" }, { "-log-level", "LogLevel" },
    { "-rp-record", "RPRecordDir" }, { "-rp-ffmpeg", "RPFFmpeg" },   // RetroPixels
  };

  // -option: the setting it turns on or off.
  const std::map<std::string, std::pair<std::string, bool>> kSwitches =
  {
    { "-threads", { "MultiThreaded", true } }, { "-no-threads", { "MultiThreaded", false } },
    { "-gpu-multi-threaded", { "GPUMultiThreaded", true } }, { "-no-gpu-thread", { "GPUMultiThreaded", false } },
    { "-window", { "FullScreen", false } }, { "-fullscreen", { "FullScreen", true } },
    { "-borderless", { "BorderlessWindow", true } },
    { "-wide-screen", { "WideScreen", true } }, { "-no-wide-screen", { "WideScreen", false } },
    { "-stretch", { "Stretch", true } }, { "-no-stretch", { "Stretch", false } },
    { "-wide-bg", { "WideBackground", true } }, { "-no-wide-bg", { "WideBackground", false } },
    { "-desktop-res", { "DesktopResolution", true } },   // RetroPixels
    { "-multi-texture", { "MultiTexture", true } }, { "-no-multi-texture", { "MultiTexture", false } },
    { "-throttle", { "Throttle", true } }, { "-no-throttle", { "Throttle", false } },
    { "-vsync", { "VSync", true } }, { "-no-vsync", { "VSync", false } },
    { "-show-fps", { "ShowFrameRate", true } }, { "-no-fps", { "ShowFrameRate", false } },
    { "-new3d", { "New3DEngine", true } }, { "-legacy3d", { "New3DEngine", false } },
    { "-quad-rendering", { "QuadRendering", true } },
    { "-flip-stereo", { "FlipStereo", true } }, { "-no-flip-stereo", { "FlipStereo", false } },
    { "-sound", { "EmulateSound", true } }, { "-no-sound", { "EmulateSound", false } },
    { "-dsb", { "EmulateDSB", true } }, { "-no-dsb", { "EmulateDSB", false } },
    { "-legacy-scsp", { "LegacySoundDSP", true } }, { "-new-scsp", { "LegacySoundDSP", false } },
    { "-no-white-flash", { "NoWhiteFlash", true } }, { "-white-flash", { "NoWhiteFlash", false } },
    { "-net", { "Network", true } }, { "-no-net", { "Network", false } },
    { "-simulate-netboard", { "SimulateNet", true } }, { "-emulate-netboard", { "SimulateNet", false } },
    { "-force-feedback", { "ForceFeedback", true } }, { "-no-force-feedback", { "ForceFeedback", false } },
    { "-dump-memory", { "DumpMemory", true } }, { "-dump-textures", { "DumpTextures", true } },
  };

  // name, or name=value, matches -<option>
  bool Is(const std::string &arg, const char *option)
  {
    const size_t n = strlen(option);
    return arg.compare(0, n, option) == 0 && (arg.size() == n || arg[n] == '=');
  }

  CommandLine ParseCommandLine(int argc, char **argv)
  {
    CommandLine cl;
    // Logging is set up from the command line alone, before anything else.
    cl.config.Set("LogOutput", LogPath().c_str());
    cl.config.Set("LogLevel", "info");

    auto bad = [&cl](const char *message)
    {
      ErrorLog("%s", message);
      cl.error = true;
    };

    // -name=<n>: a whole number, held to lo..hi
    auto number = [&](const std::string &arg, const char *key, int lo, int hi, const char *message)
    {
      const std::vector<std::string> parts = Util::Format(arg).Split('=');
      try
      {
        if (parts.size() != 2)
          throw 0;
        cl.config.Set(key, std::clamp(std::stoi(parts[1]), lo, hi));
      }
      catch (...)
      {
        bad(message);
      }
    };

    // -name=<a>,<b>
    auto pair = [&](const std::string &arg, const char *format, std::function<void(const char *)> read, const char *message)
    {
      if (Util::Format(arg).Split('=').size() != 2)
        bad(message);
      else
        read(format);
    };

    for (int i = 1; i < argc; i++)
    {
      const std::string arg = argv[i];
      if (arg[0] != '-')
      {
        cl.romFiles.push_back(arg);
        continue;
      }

      const size_t equals = arg.find('=');
      if (equals != std::string::npos)
      {
        const std::string name = arg.substr(0, equals);
        const std::string value = arg.substr(equals + 1);
        if (value.empty())
        {
          ErrorLog("Argument to '%s' cannot be blank.", name.c_str());
          cl.error = true;
          continue;
        }
        const auto setting = kValueOptions.find(name);
        if (setting != kValueOptions.end())
        {
          cl.config.Set(setting->second, value);
          continue;
        }
      }
      else
      {
        const auto sw = kSwitches.find(arg);
        if (sw != kSwitches.end())
        {
          cl.config.Set(sw->second.first, sw->second.second);
          continue;
        }
        if (kValueOptions.count(arg))
        {
          ErrorLog("'%s' requires an argument.", arg.c_str());
          cl.error = true;
          continue;
        }
      }

      const char *rest = argv[i];
      if (arg == "-?" || arg == "-h" || arg == "-help" || arg == "--help")
        cl.help = true;
      else if (arg == "-print-games")
        cl.printGames = true;
      else if (Is(arg, "-res"))
      {
        const char *message = "'-res' requires both a width and height (e.g., '-res=496,384').";
        pair(arg, "=%u,%u", [&](const char *format)
        {
          unsigned w = 0, h = 0;
          if (sscanf(rest + 4, format, &w, &h) != 2)
            return bad(message);
          cl.config.Set("XResolution", std::to_string(w));
          cl.config.Set("YResolution", std::to_string(h));
        }, message);
      }
      else if (Is(arg, "-window-pos"))
      {
        const char *message = "'-window-pos' requires both an X and Y position (e.g., '-window-pos=10,0').";
        pair(arg, "=%d,%d", [&](const char *format)
        {
          int x = 0, y = 0;
          if (sscanf(rest + 11, format, &x, &y) != 2)
            return bad(message);
          cl.config.Set("WindowXPosition", x);
          cl.config.Set("WindowYPosition", y);
        }, message);
      }
      else if (Is(arg, "-ss"))
        number(arg, "Supersampling", 1, 8, "'-ss' requires an integer argument (e.g., '-ss=2').");
      else if (Is(arg, "-crtcolors"))
        number(arg, "CRTcolors", 0, 5, "'-crtcolors' requires an integer argument (e.g., '-crtcolors=1').");
      else if (Is(arg, "-upscalemode"))
        number(arg, "UpscaleMode", 0, 3, "'-upscalemode' requires an integer argument (e.g., '-upscalemode=1').");
      else if (arg == "-true-hz")
        cl.config.Set("RefreshRate", 57.524f);
      else if (arg == "-print-gl-info")
        cl.printGLInfo = true;
      else if (arg == "-config-inputs")
        cl.configInputs = true;
      else if (arg == "-print-inputs")
        cl.printInputs = true;
#ifdef DEBUG
      else if (Is(arg, "-gfx-state"))
      {
        const std::vector<std::string> parts = Util::Format(arg).Split('=');
        if (parts.size() != 2)
          bad("'-gfx-state' requires a file name.");
        else
          cl.gfxState = parts[1];
      }
#endif
      else
      {
        ErrorLog("Ignoring unrecognized option: %s", argv[i]);
        cl.error = true;
      }
    }
    return cl;
  }

  // The input system the settings name.
  std::shared_ptr<CInputSystem> MakeInputSystem(const std::string &name)
  {
    if (name == "sdl")
      return std::make_shared<CSDLInputSystem>(s_config, false);
    if (name == "sdlgamepad")
      return std::make_shared<CSDLInputSystem>(s_config, true);
#ifdef SUPERMODEL_WIN32
    if (name == "dinput")
      return std::make_shared<CDirectInputSystem>(s_config, s_screen.window, false, false);
    if (name == "xinput")
      return std::make_shared<CDirectInputSystem>(s_config, s_screen.window, false, true);
    if (name == "rawinput")
      return std::make_shared<CDirectInputSystem>(s_config, s_screen.window, true, false);
#endif
    return nullptr;
  }

  // The outputs the settings name; false for a name not known.
  bool MakeOutputs(const std::string &name, COutputs **outputs)
  {
    *outputs = nullptr;
    if (name == "none")
      return true;
    if (name == "net")
    {
      CNetOutputs *net = new CNetOutputs();
      if (s_config["OutputsWithLF"].ValueAs<bool>())
        net->SetFrameEnding("\r\n");
      net->SetTcpPort(s_config["OutputsTCPPort"].ValueAs<unsigned>());
      net->SetUdpBroadcastPort(s_config["OutputsUDPBroadcastPort"].ValueAs<unsigned>());
      *outputs = net;
      return true;
    }
#ifdef SUPERMODEL_WIN32
    if (name == "win")
    {
      *outputs = new CWinOutputs();
      return true;
    }
#endif
    return false;
  }

  // From here the program needs the window and SDL; returns the exit code.
  int Run(CommandLine &cl, const Game &game, ROMSet &romSet, Util::Config::Node &fileConfig, std::shared_ptr<CLogger> logger)
  {
    if (SDL_Init(0) != 0)
      return ErrorLog("Unable to initialize SDL: %s\n", SDL_GetError()), 1;
    // so drivers don't scale the window's content
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "system");

    s_screen.supersampling = s_config["Supersampling"].ValueAs<int>();
    s_screen.colours = CRTcolor(s_config["CRTcolors"].ValueAs<int>());

    int code = 0;
    IEmulator *emulator = nullptr;
    CInputs *inputs = nullptr;
    COutputs *outputs = nullptr;
    std::shared_ptr<CInputSystem> inputSystem;
    const std::string systemName = s_config["InputSystem"].ValueAs<std::string>();
    const bool romGiven = !cl.romFiles.empty();

    // Each step either works or says why and stops.
    auto steps = [&]() -> int
    {
      s_screen.width = kNativeWidth;
      s_screen.height = kNativeHeight;
      if (OpenWindow(s_config["New3DEngine"].ValueAs<bool>(), s_config["QuadRendering"].ValueAs<bool>(), "Supermodel", false, false, false) != Result::OKAY)
        return 1;

      s_crosshair = new CCrosshair(s_config);
      if (s_crosshair->Init() != Result::OKAY)
        return ErrorLog("Unable to load bitmap crosshair texture\n"), 1;

#ifdef DEBUG
      if (!s_gfxStatePath.empty())
        emulator = new CModel3GraphicsState(s_config, s_gfxStatePath);
      else
#endif
        emulator = new CModel3(s_config);

      inputSystem = MakeInputSystem(systemName);
      if (!inputSystem)
        return ErrorLog("Unknown input system: %s\n", systemName.c_str()), 1;
      inputs = new CInputs(inputSystem);
      if (!inputs->Initialize())
        return ErrorLog("Unable to initalize inputs.\n"), 1;

      // fileConfig (not the merged settings): input settings go to its
      // global section, and a rewritten file keeps every section but no
      // built-in defaults.
      if (SetUpInputs(inputs, &fileConfig, &s_config, game, cl.configInputs) != Result::OKAY)
        return 1;
      if (cl.printInputs)
      {
        inputs->PrintInputs(nullptr);
        inputSystem->PrintSettings();
      }
      if (!romGiven)
        return 0;

      const std::string outputsName = s_config["Outputs"].ValueAs<std::string>();
      if (!MakeOutputs(outputsName, &outputs))
        return ErrorLog("Unknown outputs: %s\n", outputsName.c_str()), 1;
      if (outputs && !outputs->Initialize())
        return ErrorLog("Unable to initialize outputs.\n"), 1;

      (void)logger;
      const int result = RunGame(game, &romSet, emulator, inputs, outputs);
      delete emulator;
      return result;
    };
    code = steps();

    delete inputs;
    delete outputs;
    delete s_crosshair;
    CloseWindow();
    SDL_Quit();
    InfoLog(code ? "Program terminated due to an error." : "Program terminated normally.");
    return code;
  }
}

int main(int argc, char **argv)
{
  Title();
  WriteConfigFileIfMissing();

  // Until the command line says where the log goes: errors on the console.
  SetLogger(std::make_shared<CConsoleErrorLogger>());

  CommandLine cl = ParseCommandLine(argc, argv);
  if (cl.error)
    return 1;

  // No arguments at all: the settings window, which may pick a game.
  if (argc <= 1)
  {
    Util::Config::Node inFile("Global"), settings("Global");
    Util::Config::FromINIFile(&inFile, ConfigPath());
    Util::Config::MergeINISections(&settings, DefaultConfig(), inFile);
    cl.romFiles = RunGUI(ConfigPath(), settings);
    if (cl.romFiles.empty())
      return 0;
  }

  std::shared_ptr<CLogger> logger = CreateLogger(cl.config);
  if (!logger)
  {
    ErrorLog("Unable to initialize logging system.");
    return 1;
  }
  SetLogger(logger);
  InfoLog("Supermodel Version " SUPERMODEL_VERSION " (" RETROPIXELS_PATCH_LEVEL ")");
  InfoLog("Started as:");
  for (int i = 0; i < argc; i++)
    InfoLog("  argv[%d] = %s", i, argv[i]);

  if (cl.help)
  {
    Help();
    return 0;
  }
  if (cl.printGLInfo)
  {
    // the command line can pick the 3D engine to ask about
    Util::Config::MergeINISections(&s_config, DefaultConfig(), cl.config);
    PrintGLInfo(true, false, false);
    return 0;
  }

#ifdef DEBUG
  s_gfxStatePath = cl.gfxState;
#endif
  const bool romGiven = !cl.romFiles.empty();
  if (!romGiven && !cl.printGames && !cl.configInputs && !cl.printInputs)
  {
    ErrorLog("No ROM file specified.");
    return 0;
  }

  // Settings: defaults < Supermodel.ini [Global] < command line < the
  // game's section < command line again (so it wins over the game's too).
  Game game;
  ROMSet romSet;
  Util::Config::Node fileConfig("Global");
  {
    Util::Config::Node withFile("Global"), withCommandLine("Global"), withGame("Global");
    Util::Config::FromINIFile(&fileConfig, ConfigPath());
    Util::Config::MergeINISections(&withFile, DefaultConfig(), fileConfig);
    Util::Config::MergeINISections(&withCommandLine, withFile, cl.config);
    if (romGiven || cl.printGames)
    {
      const std::string xmlFile = withCommandLine["GameXMLFile"].ValueAs<std::string>();
      GameLoader loader(xmlFile);
      if (cl.printGames)
      {
        PrintGames(xmlFile, loader.GetGames());
        return 0;
      }
      if (loader.Load(&game, &romSet, cl.romFiles.front()))
        return 1;
      Util::Config::MergeINISections(&withGame, withCommandLine, fileConfig[game.name]);
    }
    else
      withGame = withCommandLine;
    Util::Config::MergeINISections(&s_config, withGame, cl.config);
  }
  LogSettings(s_config);

  return Run(cl, game, romSet, fileConfig, logger);
}

// Called by the emulator around each frame it draws.
bool BeginFrameVideo()
{
  return true;
}

void EndFrameVideo()
{
  if (s_gunInputs)
    s_crosshair->Update(s_gameInputs, s_gunInputs, s_screen.x, s_screen.y, s_screen.width, s_screen.height);
  if (RetroPixels::Recorder::Active())   // RetroPixels: the frame for the recording
    RetroPixels::Recorder::Frame(s_screen.fullWidth, s_screen.fullHeight);
  SDL_GL_SwapWindow(s_screen.window);
}
