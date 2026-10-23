// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// See WinOutputs.h. Output ids on the wire are the EOutputs value plus one;
// id 0 stands for the game itself when a helper asks for a name.

#include "OSD/Windows/WinOutputs.h"
#include "Supermodel.h"

#include <cstring>
#include <string>

namespace
{
  const char kWindowName[] = "MAMEOutput";

  // Data tag on the WM_COPYDATA reply that carries a name.
  constexpr ULONG_PTR kNameReply = 1;

  // Layout of that reply: the id asked about, then the name and its 0.
  struct NameReplyHead
  {
    UINT32 id;
    char   text[1];
  };

  // The class is registered once per process and removed by the last user.
  bool s_classReady = false;

  bool RegisterMessage(UINT &number, const char *name)
  {
    number = RegisterWindowMessageA(name);
    if (number == 0)
    {
      ErrorLog("Unable to register window message '%s' for Windows outputs", name);
      return false;
    }
    return true;
  }

  LPARAM WireId(unsigned output)
  {
    return (LPARAM)output + 1;
  }
}

CWinOutputs::CWinOutputs() = default;

CWinOutputs::~CWinOutputs()
{
  if (m_window)
    PostMessageA(HWND_BROADCAST, m_msg.stop, (WPARAM)m_window, 0);
  if (s_classReady && UnregisterClassA(kWindowName, GetModuleHandleA(nullptr)))
    s_classReady = false;
}

bool CWinOutputs::Initialize()
{
  if (!s_classReady)
  {
    WNDCLASSA type{};
    type.lpszClassName = kWindowName;
    type.hInstance = GetModuleHandleA(nullptr);
    type.lpfnWndProc = Dispatch;
    s_classReady = RegisterClassA(&type) != 0;
  }
  if (!s_classReady)
  {
    ErrorLog("Unable to register window class for Windows outputs");
    return false;
  }

  m_window = CreateWindowExA(0, kWindowName, kWindowName, WS_OVERLAPPEDWINDOW,
                             0, 0, 1, 1, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if (!m_window)
  {
    ErrorLog("Unable to create window handle for Windows outputs");
    return false;
  }

  bool ok = RegisterMessage(m_msg.start, "MAMEOutputStart")
         && RegisterMessage(m_msg.stop, "MAMEOutputStop")
         && RegisterMessage(m_msg.update, "MAMEOutputUpdateState")
         && RegisterMessage(m_msg.subscribe, "MAMEOutputRegister")
         && RegisterMessage(m_msg.unsubscribe, "MAMEOutputUnregister")
         && RegisterMessage(m_msg.askName, "MAMEOutputGetIDString");
  if (!ok)
    return false;

  // Messages reach this object through the window's user data
  SetWindowLongPtrA(m_window, GWLP_USERDATA, (LONG_PTR)this);
  return true;
}

void CWinOutputs::Attached()
{
  PostMessageA(HWND_BROADCAST, m_msg.start, (WPARAM)m_window, 0);
}

void CWinOutputs::SendOutput(EOutputs output, UINT8, UINT8 value)
{
  for (const Listener &listener : m_listeners)
    PostMessageA(listener.window, m_msg.update, WireId(output), value);
}

LRESULT CALLBACK CWinOutputs::Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
  auto *self = reinterpret_cast<CWinOutputs *>(GetWindowLongPtrA(window, GWLP_USERDATA));
  return self ? self->Handle(window, message, wParam, lParam) : 1;
}

LRESULT CWinOutputs::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
  if (message == m_msg.subscribe)
    return Subscribe((HWND)wParam, lParam);
  if (message == m_msg.unsubscribe)
    return Unsubscribe(lParam);
  if (message == m_msg.askName)
    return TellName((HWND)wParam, lParam);
  return DefWindowProcA(window, message, wParam, lParam);
}

// 1 when the id was already known (its window is updated), 0 for a new one.
// Either way the helper is sent every output's current value.
LRESULT CWinOutputs::Subscribe(HWND window, LPARAM id)
{
  for (Listener &listener : m_listeners)
  {
    if (listener.id == id)
    {
      listener.window = window;
      PostEverything(window);
      return 1;
    }
  }
  m_listeners.push_back({ id, window });
  PostEverything(window);
  return 0;
}

// 0 when at least one helper with that id was dropped, 1 when none was found.
LRESULT CWinOutputs::Unsubscribe(LPARAM id)
{
  size_t before = m_listeners.size();
  for (size_t i = m_listeners.size(); i-- > 0; )
  {
    if (m_listeners[i].id == id)
      m_listeners.erase(m_listeners.begin() + i);
  }
  return m_listeners.size() == before ? 1 : 0;
}

void CWinOutputs::PostEverything(HWND window)
{
  for (unsigned output = 0; output < NUM_OUTPUTS; output++)
    PostMessageA(window, m_msg.update, WireId(output), GetValue((EOutputs)output));
}

LRESULT CWinOutputs::TellName(HWND window, LPARAM id)
{
  std::string text;
  if (id == 0)
    text = GetGame().name;
  else if (const char *name = GetOutputName((EOutputs)(id - 1)))
    text = name;

  // The reply's size counts the head (with its one-char text, padded) plus
  // the name's characters; the terminating 0 fits in the head's text.
  std::vector<char> buffer(sizeof(NameReplyHead) + text.size(), 0);
  auto *head = reinterpret_cast<NameReplyHead *>(buffer.data());
  head->id = (UINT32)id;
  std::memcpy(head->text, text.c_str(), text.size() + 1);

  COPYDATASTRUCT reply{};
  reply.dwData = kNameReply;
  reply.cbData = (DWORD)buffer.size();
  reply.lpData = buffer.data();
  SendMessageA(window, WM_COPYDATA, (WPARAM)m_window, (LPARAM)&reply);
  return 0;
}
