// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// pixelu-wii-dock-check: checks PixelU-Wii's game window on an X server the way RetroPixels
// uses it (embed_linux.go), without RetroPixels: it plays the part of RetroPixels' app window.
//
//   pixelu-wii-dock-check MODE PROGRAM DOL USERDIR [BACKEND]
//
//   MODE     plain    PixelU-Wii as an ordinary window: it appears, shows the game, and
//                     SIGINT ends it (exit status 0).
//            nogui    the same for dolphin-emu-nogui -p x11 (PROGRAM is then that program).
//            dock     PIXELU_EMBED=1: the game window waits marked (_RETROPIXELS_EMBED) and
//                     not viewable (no flash on the desktop, not in the window manager's
//                     list), is docked into a window carrying _RETROPIXELS_DOCK exactly as
//                     RetroPixels does it, stays docked past the 20 s deadline, shows the game,
//                     follows a resize, and WM_DELETE_WINDOW stops it; closing its windows then
//                     ends the program with status 0.
//            rp-embed the same with --rp-embed on the command line instead of PIXELU_EMBED.
//            give-up  PIXELU_EMBED=1 and nobody docks it: after the deadline it is an ordinary
//                     viewable window, unmarked; SIGTERM ends it with status 0.
//   PROGRAM  PixelU-Wii (the Qt program), or dolphin-emu-nogui for MODE nogui
//   DOL      a test program (tools/pixelu-wii-test-dol.py: the screen turns green 0xC0)
//   USERDIR  PixelU-Wii's user folder for the run (-u)
//   BACKEND  the video backend, OGL (default) or Vulkan
//
// One "ok"/"FAIL" line per check; exit status 0 when all pass. Needs DISPLAY (Xvfb is fine,
// with or without a window manager). No Dolphin code: Xlib only.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

Display* g_display = nullptr;
pid_t g_child = -1;
int g_failures = 0;

void Report(bool ok, const std::string& what)
{
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  std::fflush(stdout);
  if (!ok)
    ++g_failures;
}

// X errors (a window that went away between two requests) are expected here.
int IgnoreXError(Display*, XErrorEvent*)
{
  return 0;
}

Atom Intern(const char* name)
{
  return XInternAtom(g_display, name, False);
}

std::optional<unsigned long> Cardinal(Window window, const char* property)
{
  Atom type = 0;
  int format = 0;
  unsigned long count = 0;
  unsigned long after = 0;
  unsigned char* data = nullptr;
  const int status = XGetWindowProperty(g_display, window, Intern(property), 0, 1, False,
                                        XA_CARDINAL, &type, &format, &count, &after, &data);
  std::optional<unsigned long> value;
  if (status == Success && data != nullptr && format == 32 && count == 1)
    value = *reinterpret_cast<unsigned long*>(data);
  if (data != nullptr)
    XFree(data);
  return value;
}

void SetCardinal(Window window, const char* property, unsigned long value)
{
  XChangeProperty(g_display, window, Intern(property), XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&value), 1);
}

Window ParentOf(Window window)
{
  Window root = 0;
  Window parent = 0;
  Window* children = nullptr;
  unsigned int count = 0;
  if (!XQueryTree(g_display, window, &root, &parent, &children, &count))
    return 0;
  if (children != nullptr)
    XFree(children);
  return parent;
}

void Walk(Window window, const std::function<void(Window)>& visit)
{
  visit(window);
  Window root = 0;
  Window parent = 0;
  Window* children = nullptr;
  unsigned int count = 0;
  if (!XQueryTree(g_display, window, &root, &parent, &children, &count))
    return;
  const std::vector<Window> list(children, children + count);
  if (children != nullptr)
    XFree(children);
  for (const Window child : list)
    Walk(child, visit);
}

std::vector<Window> WindowsOf(pid_t pid)
{
  std::vector<Window> found;
  Walk(DefaultRootWindow(g_display), [&](Window w) {
    if (Cardinal(w, "_NET_WM_PID") == static_cast<unsigned long>(pid))
      found.push_back(w);
  });
  return found;
}

int MapState(Window window)
{
  XWindowAttributes attributes{};
  if (!XGetWindowAttributes(g_display, window, &attributes))
    return -1;
  return attributes.map_state;
}

bool Exists(Window window)
{
  XWindowAttributes attributes{};
  return XGetWindowAttributes(g_display, window, &attributes) != 0;
}

bool WaitFor(std::chrono::milliseconds limit, const std::function<bool()>& done)
{
  const auto end = Clock::now() + limit;
  while (Clock::now() < end)
  {
    XSync(g_display, False);
    if (done())
      return true;
    std::this_thread::sleep_for(50ms);
  }
  XSync(g_display, False);
  return done();
}

// The game window: a window of the program of at least 100x100, marked if `marked`.
Window FindGameWindow(bool marked, bool viewable)
{
  for (const Window w : WindowsOf(g_child))
  {
    XWindowAttributes a{};
    if (!XGetWindowAttributes(g_display, w, &a) || a.c_class != InputOutput || a.width < 100 ||
        a.height < 100)
    {
      continue;
    }
    if (marked && Cardinal(w, "_RETROPIXELS_EMBED") != 1ul)
      continue;
    if (viewable && a.map_state != IsViewable)
      continue;
    return w;
  }
  return 0;
}

bool InClientList(Window window)
{
  Atom type = 0;
  int format = 0;
  unsigned long count = 0;
  unsigned long after = 0;
  unsigned char* data = nullptr;
  bool listed = false;
  if (XGetWindowProperty(g_display, DefaultRootWindow(g_display), Intern("_NET_CLIENT_LIST"), 0,
                         4096, False, XA_WINDOW, &type, &format, &count, &after,
                         &data) == Success &&
      data != nullptr && format == 32)
  {
    const auto* const windows = reinterpret_cast<unsigned long*>(data);
    listed = std::find(windows, windows + count, window) != windows + count;
  }
  if (data != nullptr)
    XFree(data);
  return listed;
}

// Mean green (0..1) of what is on screen where `window` is.
double MeanGreen(Window window)
{
  XWindowAttributes a{};
  if (!XGetWindowAttributes(g_display, window, &a) || a.map_state != IsViewable)
    return -1;
  int x = 0;
  int y = 0;
  Window unused = 0;
  const Window root = DefaultRootWindow(g_display);
  XTranslateCoordinates(g_display, window, root, 0, 0, &x, &y, &unused);
  XImage* const image = XGetImage(g_display, root, x, y, static_cast<unsigned>(a.width),
                                  static_cast<unsigned>(a.height), AllPlanes, ZPixmap);
  if (image == nullptr)
    return -1;
  const unsigned long mask = image->green_mask;
  int shift = 0;
  while (mask != 0 && ((mask >> shift) & 1) == 0)
    ++shift;
  const double top = static_cast<double>(mask >> shift);
  double sum = 0;
  for (int row = 0; row < image->height; ++row)
  {
    for (int column = 0; column < image->width; ++column)
      sum += static_cast<double>((XGetPixel(image, column, row) & mask) >> shift) / top;
  }
  const double mean = sum / (static_cast<double>(image->width) * image->height);
  XDestroyImage(image);
  return mean;
}

void SendDelete(Window window)
{
  XEvent event{};
  event.xclient.type = ClientMessage;
  event.xclient.window = window;
  event.xclient.message_type = Intern("WM_PROTOCOLS");
  event.xclient.format = 32;
  event.xclient.data.l[0] = static_cast<long>(Intern("WM_DELETE_WINDOW"));
  event.xclient.data.l[1] = CurrentTime;
  XSendEvent(g_display, window, False, NoEventMask, &event);
  XFlush(g_display);
}

std::optional<int> WaitExit(std::chrono::milliseconds limit)
{
  const auto end = Clock::now() + limit;
  while (Clock::now() < end)
  {
    int status = 0;
    if (waitpid(g_child, &status, WNOHANG) == g_child)
    {
      g_child = -1;
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    std::this_thread::sleep_for(50ms);
  }
  return std::nullopt;
}

pid_t Start(const std::vector<std::string>& arguments, bool embed_env)
{
  std::vector<char*> argv;
  for (const std::string& argument : arguments)
    argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid == 0)
  {
    if (embed_env)
      setenv("PIXELU_EMBED", "1", 1);
    else
      unsetenv("PIXELU_EMBED");
    execv(argv[0], argv.data());
    std::perror("execv");
    _exit(127);
  }
  return pid;
}

Window MakeWindow(Window parent, int x, int y, int width, int height, const char* name)
{
  XSetWindowAttributes attributes{};
  attributes.background_pixel = BlackPixel(g_display, DefaultScreen(g_display));
  const Window w = XCreateWindow(g_display, parent, x, y, static_cast<unsigned>(width),
                                 static_cast<unsigned>(height), 0, CopyFromParent, InputOutput,
                                 CopyFromParent, CWBackPixel, &attributes);
  XStoreName(g_display, w, name);
  return w;
}

// What RetroPixels' embed_linux.go does to dock a window: take it from the window manager,
// put it in a box (a child of the app window, carrying _RETROPIXELS_DOCK) and map it there.
void Dock(Window game, Window box, int width, int height)
{
  XUnmapWindow(g_display, game);
  XEvent unmap{};
  unmap.xunmap.type = UnmapNotify;
  unmap.xunmap.event = DefaultRootWindow(g_display);
  unmap.xunmap.window = game;
  XSendEvent(g_display, DefaultRootWindow(g_display), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &unmap);
  XAddToSaveSet(g_display, game);
  XReparentWindow(g_display, game, box, 0, 0);
  XMoveResizeWindow(g_display, game, 0, 0, static_cast<unsigned>(width),
                    static_cast<unsigned>(height));
  XMapWindow(g_display, game);
  XSync(g_display, False);
}

int Run(const std::string& mode, const std::string& program, const std::string& dol,
        const std::string& user, const std::string& backend)
{
  const bool nogui = mode == "nogui";
  const bool plain = mode == "plain" || nogui;
  const bool give_up = mode == "give-up";
  const bool flag = mode == "rp-embed";

  // Without docking the window would wait for 20 s; a short message box must not stop the run.
  std::vector<std::string> arguments{program, "-u",
                                     user,    "-b",
                                     "-v",    backend,
                                     "-C",    "Dolphin.Display.Fullscreen=False",
                                     "-C",    "Dolphin.Display.RenderToMain=False",
                                     "-C",    "Dolphin.Interface.ConfirmStop=False",
                                     "-C",    "Dolphin.Interface.PauseOnFocusLost=False",
                                     "-C",    "Dolphin.Input.BackgroundInput=True",
                                     "-C",    "Dolphin.Interface.UsePanicHandlers=False",
                                     "-e",    dol};
  if (flag)
    arguments.insert(arguments.begin() + 1, "--rp-embed");
  if (nogui)
    arguments = {program, "-u", user, "-p", "x11", "-v", backend, "-e", dol};

  // RetroPixels' app window, 800x600.
  const Window root = DefaultRootWindow(g_display);
  const Window host = MakeWindow(root, 50, 50, 800, 600, "RetroPixels");
  XMapWindow(g_display, host);
  XSync(g_display, False);

  g_child = Start(arguments, !plain && !flag);
  Report(g_child > 0, "started " + program);
  if (g_child <= 0)
    return 1;

  if (plain)
  {
    Window game = 0;
    Report(WaitFor(60s, [&] { return (game = FindGameWindow(false, true)) != 0; }),
           "the game window appears");
    if (game != 0)
    {
      double green = 0;
      WaitFor(20s, [&] { return (green = MeanGreen(game)) > 0.5; });
      Report(green > 0.5, "the game shows in it (mean green " + std::to_string(green) + ")");
    }
    kill(g_child, SIGINT);
    const std::optional<int> status = WaitExit(20s);
    Report(status == 0, "SIGINT ends it with status 0 (" +
                            (status ? std::to_string(*status) : std::string("still running")) +
                            ")");
    return g_failures == 0 ? 0 : 1;
  }

  Window game = 0;
  Report(WaitFor(60s, [&] { return (game = FindGameWindow(true, false)) != 0; }),
         "a window marked _RETROPIXELS_EMBED appears");
  if (game == 0)
  {
    kill(g_child, SIGKILL);
    WaitExit(5s);
    return 1;
  }
  const auto found_at = Clock::now();

  // Waiting: never viewable, never the window manager's, for a while.
  bool hidden = true;
  bool unmanaged = true;
  WaitFor(3s, [&] {
    hidden = hidden && MapState(game) != IsViewable;
    unmanaged = unmanaged && !InClientList(game) && ParentOf(game) != root;
    return false;
  });
  Report(hidden, "it is not viewable while it waits");
  Report(unmanaged, "the window manager never gets it (not a root child, not in _NET_CLIENT_LIST)");

  if (give_up)
  {
    bool released = WaitFor(
        30s, [&] { return MapState(game) == IsViewable && !Cardinal(game, "_RETROPIXELS_EMBED"); });
    const double waited = std::chrono::duration<double>(Clock::now() - found_at).count();
    Report(released && waited > 15,
           "nobody docks it: after the deadline it is an ordinary window, unmarked (after " +
               std::to_string(waited) + " s)");
    double green = 0;
    WaitFor(10s, [&] { return (green = MeanGreen(game)) > 0.5; });
    Report(green > 0.5, "the game shows in it (mean green " + std::to_string(green) + ")");
    kill(g_child, SIGTERM);
    const std::optional<int> status = WaitExit(20s);
    Report(status == 0, "SIGTERM ends it with status 0");
    return g_failures == 0 ? 0 : 1;
  }

  // Dock it below a 40-pixel game bar, as RetroPixels does.
  const Window box = MakeWindow(host, 0, 40, 800, 560, "RetroPixels game");
  SetCardinal(box, "_RETROPIXELS_DOCK", static_cast<unsigned long>(getpid()));
  XMapWindow(g_display, box);
  Dock(game, box, 800, 560);
  Report(WaitFor(5s, [&] { return MapState(game) == IsViewable && ParentOf(game) == box; }),
         "docked: viewable inside the dock window");

  double green = 0;
  WaitFor(20s, [&] { return (green = MeanGreen(game)) > 0.5; });
  Report(green > 0.5,
         "the game shows in RetroPixels' window (mean green " + std::to_string(green) + ")");

  // Past the 20 s deadline it must stay where RetroPixels put it.
  std::this_thread::sleep_until(found_at + 24s);
  XSync(g_display, False);
  Report(ParentOf(game) == box && MapState(game) == IsViewable, "still docked after the deadline");

  // RetroPixels resizes the box and the game window; the picture follows.
  XResizeWindow(g_display, box, 640, 400);
  XResizeWindow(g_display, game, 640, 400);
  XSync(g_display, False);
  std::this_thread::sleep_for(2s);
  green = MeanGreen(game);
  Report(green > 0.5, "the picture follows a resize (mean green " + std::to_string(green) + ")");

  // Stop game: WM_DELETE_WINDOW to the game window stops the game; the window goes.
  SendDelete(game);
  Report(WaitFor(15s, [&] { return !Exists(game) || MapState(game) != IsViewable; }),
         "WM_DELETE_WINDOW stops the game (the game window goes)");

  // RetroPixels then closes the program's remaining windows.
  for (const Window w : WindowsOf(g_child))
    SendDelete(w);
  std::optional<int> status = WaitExit(20s);
  if (!status)
  {
    kill(g_child, SIGTERM);
    status = WaitExit(10s);
  }
  Report(status == 0, "the program then ends with status 0 (" +
                          (status ? std::to_string(*status) : std::string("still running")) + ")");
  return g_failures == 0 ? 0 : 1;
}
}  // namespace

int main(int argc, char* argv[])
{
  if (argc < 5)
  {
    std::fprintf(stderr,
                 "usage: %s plain|nogui|dock|rp-embed|give-up PROGRAM DOL USERDIR [OGL|Vulkan]\n",
                 argv[0]);
    return 2;
  }
  const std::string mode = argv[1];
  if (mode != "plain" && mode != "nogui" && mode != "dock" && mode != "rp-embed" &&
      mode != "give-up")
  {
    std::fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
  }
  g_display = XOpenDisplay(nullptr);
  if (g_display == nullptr)
  {
    std::fputs("cannot open the X display\n", stderr);
    return 2;
  }
  XSetErrorHandler(IgnoreXError);
  const int result = Run(mode, argv[2], argv[3], argv[4], argc > 5 ? argv[5] : "OGL");
  if (g_child > 0)
  {
    kill(g_child, SIGKILL);
    waitpid(g_child, nullptr, 0);
  }
  XCloseDisplay(g_display);
  std::printf("%s\n", result == 0 ? "all checks passed" : "some checks failed");
  return result;
}
