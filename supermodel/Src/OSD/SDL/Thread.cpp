// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// OSD/Thread.h on top of SDL's threads, semaphores, condition variables and
// mutexes. Each object keeps the SDL handle as its opaque pointer.

#include "Thread.h"

#include "Supermodel.h"
#include "SDLIncludes.h"

namespace
{
  inline SDL_Thread *AsThread(void *p) { return static_cast<SDL_Thread *>(p); }
  inline SDL_sem *AsSem(void *p) { return static_cast<SDL_sem *>(p); }
  inline SDL_cond *AsCond(void *p) { return static_cast<SDL_cond *>(p); }
  inline SDL_mutex *AsMutex(void *p) { return static_cast<SDL_mutex *>(p); }
}

// ---------------------------------------------------------------- CThread

void CThread::Sleep(UINT32 ms)
{
  SDL_Delay(ms);
}

UINT32 CThread::GetTicks()
{
  return SDL_GetTicks();
}

CThread *CThread::CreateThread(const std::string &name, ThreadStart start, void *startParam)
{
  SDL_Thread *t = SDL_CreateThread(start, name.c_str(), startParam);
  return t ? new CThread(name, t) : nullptr;
}

CSemaphore *CThread::CreateSemaphore(UINT32 initVal)
{
  SDL_sem *s = SDL_CreateSemaphore(initVal);
  return s ? new CSemaphore(s) : nullptr;
}

CCondVar *CThread::CreateCondVar()
{
  SDL_cond *c = SDL_CreateCond();
  return c ? new CCondVar(c) : nullptr;
}

CMutex *CThread::CreateMutex()
{
  SDL_mutex *m = SDL_CreateMutex();
  return m ? new CMutex(m) : nullptr;
}

const char *CThread::GetLastError()
{
  return SDL_GetError();
}

CThread::CThread(const std::string &name, void *impl)
  : m_name(name), m_impl(impl)
{
}

CThread::~CThread()
{
  if (m_impl != nullptr)
    ErrorLog("Runaway thread error. A thread was not properly halted: %s\n", m_name.c_str());
}

const std::string &CThread::GetName() const
{
  return m_name;
}

UINT32 CThread::GetId()
{
  return SDL_GetThreadID(AsThread(m_impl));
}

int CThread::Wait()
{
  if (m_impl == nullptr)
    return -1;
  int status = 0;
  SDL_WaitThread(AsThread(m_impl), &status);
  m_impl = nullptr;
  return status;
}

// ------------------------------------------------------------- CSemaphore

CSemaphore::CSemaphore(void *impl) : m_impl(impl)
{
}

CSemaphore::~CSemaphore()
{
  SDL_DestroySemaphore(AsSem(m_impl));
}

UINT32 CSemaphore::GetValue()
{
  return SDL_SemValue(AsSem(m_impl));
}

bool CSemaphore::Wait()
{
  return SDL_SemWait(AsSem(m_impl)) == 0;
}

bool CSemaphore::Post()
{
  return SDL_SemPost(AsSem(m_impl)) == 0;
}

// --------------------------------------------------------------- CCondVar

CCondVar::CCondVar(void *impl) : m_impl(impl)
{
}

CCondVar::~CCondVar()
{
  SDL_DestroyCond(AsCond(m_impl));
}

bool CCondVar::Wait(CMutex *mutex)
{
  return SDL_CondWait(AsCond(m_impl), AsMutex(mutex->m_impl)) == 0;
}

bool CCondVar::Signal()
{
  return SDL_CondSignal(AsCond(m_impl)) == 0;
}

bool CCondVar::SignalAll()
{
  return SDL_CondBroadcast(AsCond(m_impl)) == 0;
}

// ----------------------------------------------------------------- CMutex

CMutex::CMutex(void *impl) : m_impl(impl)
{
}

CMutex::~CMutex()
{
  SDL_DestroyMutex(AsMutex(m_impl));
}

bool CMutex::Lock()
{
  return SDL_LockMutex(AsMutex(m_impl)) == 0;
}

bool CMutex::Unlock()
{
  return SDL_UnlockMutex(AsMutex(m_impl)) == 0;
}
