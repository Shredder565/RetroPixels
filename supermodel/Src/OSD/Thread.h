// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Threads and the things threads wait on, as the emulator's CPU, graphics
// and sound threads use them. SDL/Thread.cpp implements them with SDL.

#ifndef INCLUDED_THREADS_H
#define INCLUDED_THREADS_H

#include "Types.h"

#include <string>

class CSemaphore;
class CMutex;
class CCondVar;

// A thread's body: gets the parameter given at creation, returns its status.
typedef int (*ThreadStart)(void *startParam);

class CThread
{
public:
  // Pauses the calling thread for ms milliseconds.
  static void Sleep(UINT32 ms);

  // Milliseconds since the program started.
  static UINT32 GetTicks();

  // Starts a thread running start(startParam). NULL if it couldn't be made.
  static CThread *CreateThread(const std::string &name, ThreadStart start, void *startParam);

  // Makers for the other objects; NULL on failure.
  static CSemaphore *CreateSemaphore(UINT32 initVal);
  static CCondVar *CreateCondVar();
  static CMutex *CreateMutex();

  // Text describing why the last call above failed.
  static const char *GetLastError();

  // Wait() must have been called first; an error is logged otherwise.
  ~CThread();

  const std::string &GetName() const;
  UINT32 GetId();

  // Blocks until the thread has finished; returns its status (-1 if it was
  // already waited for).
  int Wait();

private:
  CThread(const std::string &name, void *impl);

  const std::string m_name;
  void *m_impl;
};

class CSemaphore
{
  friend class CThread;

public:
  ~CSemaphore();

  UINT32 GetValue();

  // Takes one count, waiting while there is none. False on error.
  bool Wait();

  // Gives one count back, waking a waiting thread. False on error.
  bool Post();

private:
  CSemaphore(void *impl);

  void *m_impl;
};

class CCondVar
{
  friend class CThread;

public:
  ~CCondVar();

  // Releases mutex (which the caller holds), waits to be signalled, then
  // takes the mutex again.
  bool Wait(CMutex *mutex);

  // Wakes one waiting thread / every waiting thread.
  bool Signal();
  bool SignalAll();

private:
  CCondVar(void *impl);

  void *m_impl;
};

class CMutex
{
  friend class CThread;
  friend class CCondVar;

public:
  ~CMutex();

  bool Lock();
  bool Unlock();

private:
  CMutex(void *impl);

  void *m_impl;
};

#endif // INCLUDED_THREADS_H
