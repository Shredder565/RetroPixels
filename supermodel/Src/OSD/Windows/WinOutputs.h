// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Lamp and motor outputs for Windows helpers that speak MAMEHooker's
// window-message protocol. A hidden window named "MAMEOutput" is created;
// helpers find it, register themselves, and are then posted every change.

#ifndef INCLUDED_WINOUTPUTS_H
#define INCLUDED_WINOUTPUTS_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "OSD/Outputs.h"

#include <vector>

class CWinOutputs : public COutputs
{
public:
  CWinOutputs();
  ~CWinOutputs() override;

  // Registers the window class and messages and makes the hidden window.
  // False (with an error logged) when any of that fails.
  bool Initialize();

  // Announces to every top-level window that outputs are available.
  void Attached();

protected:
  void SendOutput(EOutputs output, UINT8 prevValue, UINT8 value) override;

private:
  // A helper that registered with us: the id it chose and where it listens.
  struct Listener
  {
    LPARAM id;
    HWND   window;
  };

  // Message numbers, as given by RegisterWindowMessage for the protocol names.
  struct Messages
  {
    UINT start = 0;        // we broadcast: outputs available
    UINT stop = 0;         // we broadcast: outputs going away
    UINT update = 0;       // we post: (output id, value)
    UINT subscribe = 0;    // helper sends: (its window, its id)
    UINT unsubscribe = 0;  // helper sends: (its window, its id)
    UINT askName = 0;      // helper sends: (its window, output id)
  };

  static LRESULT CALLBACK Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

  LRESULT Subscribe(HWND window, LPARAM id);
  LRESULT Unsubscribe(LPARAM id);
  LRESULT TellName(HWND window, LPARAM id);
  void    PostEverything(HWND window);

  HWND                  m_window = nullptr;
  Messages              m_msg;
  std::vector<Listener> m_listeners;
};

#endif // INCLUDED_WINOUTPUTS_H
