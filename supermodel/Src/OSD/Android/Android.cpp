// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// RetroPixels for Android: SDL's hints, the touch controls' pad, the
// console's output and the recorder (see Android.h).

#include "OSD/Android/Android.h"
#include "OSD/RetroPixels.h"
#include "OSD/FileSystemPath.h"
#include "OSD/Logger.h"

#include <GL/glew.h>
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
  const char kActivityClass[] = "io/github/shredder565/retropixels/SupermodelActivity";

  JavaVM *s_vm = nullptr;
  jclass s_activity = nullptr;          // global reference
  jmethodID s_recordFrame = nullptr;    // static void recordFrame(byte[], int, int, short[], int)

  /****************************************************************************
   The touch controls' pad
  ****************************************************************************/

  // Set from the app's UI thread, read when SDL updates its pads.
  std::atomic<int> s_touchX(0), s_touchY(0), s_touchButtons(0);
  SDL_Joystick *s_touchPad = nullptr;
  int s_touchPadIndex = -1;

  // The pad's buttons, in the order SDL numbers a virtual controller's
  // (A, B, Back, Start), and its two axes (the left stick).
  const int kTouchButtons = 4;

  void SDLCALL UpdateTouchPad(void *)
  {
    if (!s_touchPad)
      return;
    SDL_JoystickSetVirtualAxis(s_touchPad, 0, Sint16(s_touchX.load()));
    SDL_JoystickSetVirtualAxis(s_touchPad, 1, Sint16(s_touchY.load()));
    const int buttons = s_touchButtons.load();
    for (int i = 0; i < kTouchButtons; i++)
      SDL_JoystickSetVirtualButton(s_touchPad, i, (buttons >> i) & 1 ? SDL_PRESSED : SDL_RELEASED);
  }

  void AttachTouchPad()
  {
    SDL_VirtualJoystickDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = 2;
    desc.nbuttons = kTouchButtons;
    desc.button_mask = (1u << SDL_CONTROLLER_BUTTON_A) | (1u << SDL_CONTROLLER_BUTTON_B) |
                       (1u << SDL_CONTROLLER_BUTTON_BACK) | (1u << SDL_CONTROLLER_BUTTON_START);
    desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_LEFTX) | (1u << SDL_CONTROLLER_AXIS_LEFTY);
    desc.name = "RetroPixels touch controls";
    desc.Update = UpdateTouchPad;
    s_touchPadIndex = SDL_JoystickAttachVirtualEx(&desc);
    if (s_touchPadIndex < 0)
    {
      ErrorLog("The touch controls couldn't be added: %s", SDL_GetError());
      return;
    }
    s_touchPad = SDL_JoystickOpen(s_touchPadIndex);
  }

  void DetachTouchPad()
  {
    if (s_touchPad)
      SDL_JoystickClose(s_touchPad);
    s_touchPad = nullptr;
    if (s_touchPadIndex >= 0)
      SDL_JoystickDetachVirtual(s_touchPadIndex);
    s_touchPadIndex = -1;
  }

  /****************************************************************************
   Recorder
  ****************************************************************************/

  const unsigned kRecordLines = 384;    // the Model 3's own; GameRecorder scales up by whole numbers

  std::atomic<bool> s_recording(false); // the app wants frames
  std::atomic<bool> s_paused(false);    // Supermodel's own pause
  std::atomic<int> s_recordingRun(0);   // one more each time recording starts or stops
  std::mutex s_pcmMutex;
  std::vector<int16_t> s_pcm;           // sound since the last frame handed over (s16 stereo)

  struct Readback
  {
    GLuint fbo = 0, colour = 0;
    GLuint pbo[2] = { 0, 0 };
    unsigned w = 0, h = 0;
    unsigned pboW[2] = { 0, 0 }, pboH[2] = { 0, 0 };
    bool pending[2] = { false, false };
    int next = 0;
    int run = 0;                        // the s_recordingRun the pending pictures belong to
    std::vector<uint8_t> flipped;
    jbyteArray picArray = nullptr;      // global references, reused
    jsize picLength = 0;
    jshortArray pcmArray = nullptr;
    jsize pcmLength = 0;
  } s_rb;

  int16_t Clamp16(float v)
  {
    if (v > 32767.0f)
      return 32767;
    if (v < -32768.0f)
      return -32768;
    return int16_t(v);
  }

  void FreeArrays(JNIEnv *env)
  {
    if (env && s_rb.picArray)
      env->DeleteGlobalRef(s_rb.picArray);
    if (env && s_rb.pcmArray)
      env->DeleteGlobalRef(s_rb.pcmArray);
    s_rb.picArray = nullptr;
    s_rb.pcmArray = nullptr;
    s_rb.picLength = s_rb.pcmLength = 0;
  }

  void FreeGL()
  {
    if (s_rb.fbo)
      glDeleteFramebuffers(1, &s_rb.fbo);
    if (s_rb.colour)
      glDeleteRenderbuffers(1, &s_rb.colour);
    if (s_rb.pbo[0])
      glDeleteBuffers(2, s_rb.pbo);
    s_rb.fbo = s_rb.colour = 0;
    s_rb.pbo[0] = s_rb.pbo[1] = 0;
    s_rb.w = s_rb.h = 0;
    s_rb.pending[0] = s_rb.pending[1] = false;
  }

  // The picture of an earlier frame, now read back, to the app with the
  // sound made since the last one.
  void Deliver(int slot)
  {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env || !s_activity || !s_recordFrame)
      return;
    const unsigned w = s_rb.pboW[slot], h = s_rb.pboH[slot];
    const size_t rowBytes = size_t(w) * 4, bytes = rowBytes * h;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_rb.pbo[slot]);
    const uint8_t *src = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(bytes), GL_MAP_READ_BIT);
    if (!src)
      return;
    s_rb.flipped.resize(bytes);
    for (unsigned y = 0; y < h; y++)    // GL's rows come bottom first
      memcpy(&s_rb.flipped[size_t(h - 1 - y) * rowBytes], src + size_t(y) * rowBytes, rowBytes);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

    if (s_rb.picLength < jsize(bytes))
    {
      if (s_rb.picArray)
        env->DeleteGlobalRef(s_rb.picArray);
      jbyteArray local = env->NewByteArray(jsize(bytes));
      s_rb.picArray = local ? (jbyteArray)env->NewGlobalRef(local) : nullptr;
      env->DeleteLocalRef(local);
      s_rb.picLength = s_rb.picArray ? jsize(bytes) : 0;
      if (!s_rb.picArray)
        return;
    }
    env->SetByteArrayRegion(s_rb.picArray, 0, jsize(bytes), (const jbyte *)s_rb.flipped.data());

    std::vector<int16_t> pcm;
    {
      std::lock_guard<std::mutex> lock(s_pcmMutex);
      pcm.swap(s_pcm);
    }
    if (s_rb.pcmLength < jsize(pcm.size()) || !s_rb.pcmArray)
    {
      if (s_rb.pcmArray)
        env->DeleteGlobalRef(s_rb.pcmArray);
      const jsize n = std::max<jsize>(jsize(pcm.size()), 4096);
      jshortArray local = env->NewShortArray(n);
      s_rb.pcmArray = local ? (jshortArray)env->NewGlobalRef(local) : nullptr;
      env->DeleteLocalRef(local);
      s_rb.pcmLength = s_rb.pcmArray ? n : 0;
      if (!s_rb.pcmArray)
        return;
    }
    if (!pcm.empty())
      env->SetShortArrayRegion(s_rb.pcmArray, 0, jsize(pcm.size()), (const jshort *)pcm.data());
    env->CallStaticVoidMethod(s_activity, s_recordFrame, s_rb.picArray, jint(w), jint(h), s_rb.pcmArray, jint(pcm.size()));
    if (env->ExceptionCheck())
    {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  }
}

namespace RetroPixels
{
  namespace Android
  {
    void Start()
    {
      // landscape only; controllers come through Android's own input
      // (SDL's HIDAPI drivers would want USB and Bluetooth access); touches
      // aren't a mouse
      SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
      SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
      SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");

      // printf output (Supermodel's own messages) next to the log
      const std::string log = RetroPixels::HomePath(FileSystemPath::Log);
      if (!log.empty())
      {
        const std::string file = log + "console.txt";
        if (freopen(file.c_str(), "w", stdout))
          setvbuf(stdout, nullptr, _IOLBF, 0);
        if (freopen(file.c_str(), "a", stderr))
          setvbuf(stderr, nullptr, _IONBF, 0);
      }

      if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        ErrorLog("SDL's game controllers couldn't start: %s", SDL_GetError());
      else
        AttachTouchPad();
    }

    void Stop()
    {
      DetachTouchPad();
      SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
      FreeArrays((JNIEnv *)SDL_AndroidGetJNIEnv());
      fflush(stdout);
    }
  }

  namespace Recorder
  {
    bool Start(const std::string &, const std::string &)
    {
      ErrorLog("-rp-record isn't used on Android; the app records the game itself.");
      return false;
    }

    bool Active()
    {
      return s_recording.load() && !s_paused.load();
    }

    void Frame(unsigned, unsigned)
    {
    }

    void FrameArea(unsigned x, unsigned y, unsigned width, unsigned height)
    {
      if (!Active() || width == 0 || height == 0)
        return;
      const unsigned outH = kRecordLines;
      const unsigned outW = std::max(2u, unsigned(std::lround(double(width) * outH / height)) & ~1u);

      GLint readFBO = 0, drawFBO = 0, packBuffer = 0, packAlign = 4, renderbuffer = 0;
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFBO);
      glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
      glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
      glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
      const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
      const GLboolean discard = glIsEnabled(GL_RASTERIZER_DISCARD);

      if (!s_rb.pbo[0])
        glGenBuffers(2, s_rb.pbo);
      if (s_rb.w != outW || s_rb.h != outH)
      {
        if (!s_rb.fbo)
          glGenFramebuffers(1, &s_rb.fbo);
        if (s_rb.colour)
          glDeleteRenderbuffers(1, &s_rb.colour);
        glGenRenderbuffers(1, &s_rb.colour);
        glBindRenderbuffer(GL_RENDERBUFFER, s_rb.colour);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, GLsizei(outW), GLsizei(outH));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_rb.fbo);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_rb.colour);
        s_rb.w = outW;
        s_rb.h = outH;
      }

      // the game's picture, scaled, into our buffer...
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_RASTERIZER_DISCARD);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_rb.fbo);
      glBlitFramebuffer(GLint(x), GLint(y), GLint(x + width), GLint(y + height), 0, 0, GLint(outW), GLint(outH), GL_COLOR_BUFFER_BIT, GL_LINEAR);

      // ...read back into a buffer without waiting; the one read a frame ago
      // goes to the app now
      if (s_rb.run != s_recordingRun.load())  // pictures from before don't go into a new recording
      {
        s_rb.run = s_recordingRun.load();
        s_rb.pending[0] = s_rb.pending[1] = false;
      }
      const int slot = s_rb.next;
      glBindFramebuffer(GL_READ_FRAMEBUFFER, s_rb.fbo);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, s_rb.pbo[slot]);
      if (s_rb.pboW[slot] != outW || s_rb.pboH[slot] != outH)
      {
        glBufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(outW) * outH * 4, nullptr, GL_STREAM_READ);
        s_rb.pboW[slot] = outW;
        s_rb.pboH[slot] = outH;
      }
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glReadPixels(0, 0, GLsizei(outW), GLsizei(outH), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      s_rb.pending[slot] = true;
      const int previous = 1 - slot;
      if (s_rb.pending[previous])
      {
        s_rb.pending[previous] = false;
        Deliver(previous);
      }
      s_rb.next = previous;

      glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(packBuffer));
      glBindRenderbuffer(GL_RENDERBUFFER, GLuint(renderbuffer));
      glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFBO));
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFBO));
      if (scissor)
        glEnable(GL_SCISSOR_TEST);
      if (discard)
        glEnable(GL_RASTERIZER_DISCARD);
    }

    void Audio(unsigned numSamples, const float *leftFront, const float *rightFront, const float *leftRear, const float *rightRear)
    {
      if (!Active())
        return;
      std::lock_guard<std::mutex> lock(s_pcmMutex);
      if (s_pcm.size() > 44100 * 2)     // no frames taking it (a second's worth): start again
        s_pcm.clear();
      for (unsigned i = 0; i < numSamples; i++)
      {
        // front and rear speakers mixed down to stereo, as the PC recorder does
        s_pcm.push_back(Clamp16((leftFront[i] + leftRear[i]) * 0.5f));
        s_pcm.push_back(Clamp16((rightFront[i] + rightRear[i]) * 0.5f));
      }
    }

    void SetPaused(bool paused)
    {
      s_paused = paused;
      if (paused)
      {
        std::lock_guard<std::mutex> lock(s_pcmMutex);
        s_pcm.clear();
      }
    }

    void Stop()
    {
      s_recording = false;
      if (SDL_GL_GetCurrentContext())
        FreeGL();
      std::lock_guard<std::mutex> lock(s_pcmMutex);
      s_pcm.clear();
    }
  }
}

/******************************************************************************
 Called by the app (SupermodelActivity)
******************************************************************************/

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *)
{
  s_vm = vm;
  JNIEnv *env = nullptr;
  if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;
  jclass local = env->FindClass(kActivityClass);
  if (!local)
  {
    env->ExceptionClear();
    return JNI_VERSION_1_6;   // the program still runs; there is just no recording
  }
  s_activity = (jclass)env->NewGlobalRef(local);
  env->DeleteLocalRef(local);
  s_recordFrame = env->GetStaticMethodID(s_activity, "recordFrame", "([BII[SI)V");
  if (!s_recordFrame)
    env->ExceptionClear();
  return JNI_VERSION_1_6;
}

// The folder with Config/Games.xml, Config/Music.xml and Assets/, which
// Supermodel finds from the working folder. Returns false if it can't go there.
extern "C" JNIEXPORT jboolean JNICALL Java_io_github_shredder565_retropixels_SupermodelActivity_nativePrepare(JNIEnv *env, jclass, jstring dataDir)
{
  const char *dir = env->GetStringUTFChars(dataDir, nullptr);
  const bool ok = dir && chdir(dir) == 0;
  if (dir)
    env->ReleaseStringUTFChars(dataDir, dir);
  return ok ? JNI_TRUE : JNI_FALSE;
}

// The touch controls: stick position (-1..1 each way, down positive) and
// buttons (bit 0 fire, 1 event, 2 coin, 3 start).
extern "C" JNIEXPORT void JNICALL Java_io_github_shredder565_retropixels_SupermodelActivity_nativeTouch(JNIEnv *, jclass, jfloat x, jfloat y, jint buttons)
{
  auto axis = [](float v) { return int(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f)); };
  s_touchX = axis(x);
  s_touchY = axis(y);
  s_touchButtons = int(buttons);
}

extern "C" JNIEXPORT void JNICALL Java_io_github_shredder565_retropixels_SupermodelActivity_nativeSetRecording(JNIEnv *, jclass, jboolean on)
{
  s_recording = on == JNI_TRUE;
  s_recordingRun++;
  if (!on)
  {
    std::lock_guard<std::mutex> lock(s_pcmMutex);
    s_pcm.clear();
  }
}
