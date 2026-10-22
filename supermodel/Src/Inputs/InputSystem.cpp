// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "InputSystem.h"

#include "Supermodel.h"
#include "Input.h"
#include "OSD/Thread.h"
#include "Util/Format.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
  //
  // Names used in mapping strings
  //

  // Every key name a mapping may use. The order is the order ReadMapping
  // lists keys pressed together in.
  const char *const kKeyNames[] =
  {
    // General keys
    "BACKSPACE", "TAB", "CLEAR", "RETURN", "PAUSE", "ESCAPE", "SPACE", "EXCLAIM", "DBLQUOTE", "HASH",
    "DOLLAR", "AMPERSAND", "QUOTE", "LEFTPAREN", "RIGHTPAREN", "ASTERISK", "PLUS", "COMMA", "MINUS",
    "PERIOD", "SLASH",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "COLON", "SEMICOLON", "LESS", "EQUALS", "GREATER", "QUESTION", "AT", "LEFTBRACKET", "BACKSLASH",
    "RIGHTBRACKET", "CARET", "UNDERSCORE", "BACKQUOTE",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "DEL",
    // Keypad
    "KEYPAD0", "KEYPAD1", "KEYPAD2", "KEYPAD3", "KEYPAD4", "KEYPAD5", "KEYPAD6", "KEYPAD7", "KEYPAD8",
    "KEYPAD9", "KEYPADPERIOD", "KEYPADDIVIDE", "KEYPADMULTIPLY", "KEYPADMINUS", "KEYPADPLUS",
    "KEYPADENTER", "KEYPADEQUALS",
    // Cursor and editing keys
    "UP", "DOWN", "RIGHT", "LEFT", "INSERT", "HOME", "END", "PGUP", "PGDN",
    // Function keys
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15",
    // Lock and modifier keys
    "NUMLOCK", "CAPSLOCK", "SCROLLLOCK", "SHIFT", "RIGHTSHIFT", "LEFTSHIFT", "CTRL", "RIGHTCTRL",
    "LEFTCTRL", "ALT", "RIGHTALT", "LEFTALT", "RIGHTMETA", "LEFTMETA", "RIGHTWINDOWS", "LEFTWINDOWS",
    "ALTGR", "COMPOSE",
    // Others
    "HELP", "PRINT", "SYSREQ", "BREAK", "MENU", "POWER", "EURO", "UNDO"
  };

  const char *const kAxisIds[NUM_JOY_AXES] = { "X", "Y", "Z", "RX", "RY", "RZ", "S1", "S2" };
  const char *const kAxisNames[NUM_JOY_AXES] = { "X-Axis", "Y-Axis", "Z-Axis", "RX-Axis", "RY-Axis", "RZ-Axis", "Slider1", "Slider2" };
  const char *const kAxisWays[4] = { "", "_INV", "_POS", "_NEG" };  // AXIS_FULL, _INVERTED, _POS, _NEG

  bool IsAsciiSpace(char c)
  {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
  }

  bool IsDigit(char c)
  {
    return c >= '0' && c <= '9';
  }

  // Case-insensitive (ASCII) comparison of a whole string.
  bool SameText(const std::string &a, const char *b)
  {
    return Util::Stricmp(a.c_str(), b) == 0;
  }

  bool StartsWithText(const std::string &s, const char *prefix)
  {
    size_t n = strlen(prefix);
    return s.size() >= n && Util::Stricmp(s.substr(0, n).c_str(), prefix) == 0;
  }

  bool IsKeyName(const std::string &s)
  {
    for (const char *name : kKeyNames)
      if (SameText(s, name))
        return true;
    return false;
  }

  // The names of a device's parts. The first name given for a part is the
  // one ReadMapping writes; the others are accepted as well.
  struct PartNames
  {
    std::vector<std::string> canonical;               // by part number
    std::vector<std::pair<std::string, int>> all;     // every accepted name

    void Add(const std::string &name, int part)
    {
      if (part >= (int)canonical.size())
        canonical.resize(part + 1);
      if (canonical[part].empty())
        canonical[part] = name;
      all.emplace_back(name, part);
    }

    int Find(const std::string &name) const
    {
      for (const auto &entry : all)
        if (SameText(name, entry.first.c_str()))
          return entry.second;
      return -1;
    }
  };

  const PartNames &MouseNames()
  {
    static const PartNames names = []()
    {
      PartNames n;
      const char *const ids[NUM_MOUSE_AXES] = { "XAXIS", "YAXIS", "ZAXIS" };
      // Direction words for the positive and negative halves.
      const char *const posWord[NUM_MOUSE_AXES] = { "RIGHT", "DOWN", "WHEEL_UP" };
      const char *const negWord[NUM_MOUSE_AXES] = { "LEFT", "UP", "WHEEL_DOWN" };
      for (int a = 0; a < NUM_MOUSE_AXES; a++)
      {
        int base = MouseXAxis + 4 * a;
        for (int w = 0; w < 4; w++)
          n.Add(std::string(ids[a]) + kAxisWays[w], base + w);
        n.Add(posWord[a], base + AXIS_POS);
        n.Add(negWord[a], base + AXIS_NEG);
        for (int w = 0; w < 4; w++)
          n.Add("AXIS" + std::to_string(a + 1) + kAxisWays[w], base + w);
      }
      const char *const buttons[NUM_MOUSE_BUTTONS] = { "LEFT_BUTTON", "MIDDLE_BUTTON", "RIGHT_BUTTON", "BUTTONX1", "BUTTONX2" };
      for (int b = 0; b < NUM_MOUSE_BUTTONS; b++)
      {
        n.Add(buttons[b], MouseButtonLeft + b);
        n.Add("BUTTON" + std::to_string(b + 1), MouseButtonLeft + b);
      }
      return n;
    }();
    return names;
  }

  const PartNames &JoyNames()
  {
    static const PartNames names = []()
    {
      PartNames n;
      const char *const ids[NUM_JOY_AXES] = { "XAXIS", "YAXIS", "ZAXIS", "RXAXIS", "RYAXIS", "RZAXIS", "SLIDER1", "SLIDER2" };
      for (int a = 0; a < NUM_JOY_AXES; a++)
      {
        int base = JoyXAxis + 4 * a;
        for (int w = 0; w < 4; w++)
          n.Add(std::string(ids[a]) + kAxisWays[w], base + w);
        if (a == AXIS_X)
        {
          n.Add("RIGHT", base + AXIS_POS);
          n.Add("LEFT", base + AXIS_NEG);
        }
        else if (a == AXIS_Y)
        {
          n.Add("DOWN", base + AXIS_POS);
          n.Add("UP", base + AXIS_NEG);
        }
        for (int w = 0; w < 4; w++)
          n.Add("AXIS" + std::to_string(a + 1) + kAxisWays[w], base + w);
      }
      const char *const dirs[4] = { "_UP", "_DOWN", "_LEFT", "_RIGHT" };  // POV_UP ... POV_RIGHT
      for (int p = 0; p < NUM_JOY_POVS; p++)
        for (int d = 0; d < 4; d++)
          n.Add("POV" + std::to_string(p + 1) + dirs[d], JoyPOV0Up + 4 * p + d);
      for (int b = 0; b < NUM_JOY_BUTTONS; b++)
        n.Add("BUTTON" + std::to_string(b + 1), JoyButton0 + b);
      return n;
    }();
    return names;
  }

  // For "<DEV>[n]_<part>": the position of <part> and the device (n - 1, or
  // -1 for none or 0), or npos if 'str' is not of that form.
  size_t ParseDeviceMapping(const std::string &str, const char *dev, int &devNum)
  {
    if (!StartsWithText(str, dev))
      return std::string::npos;
    const size_t size = str.size();
    const size_t devLen = strlen(dev);
    size_t at = devLen;
    while (at < size && IsDigit(str[at]))
      at++;
    devNum = -1;
    if (at > devLen && at < size)
    {
      long long n = 0;
      for (size_t i = devLen; i < at; i++)
      {
        n = n * 10 + (str[i] - '0');
        if (n > INT_MAX)
          return std::string::npos;  // too big a number: not a mapping
      }
      devNum = int(n) - 1;
    }
    // The part needs at least one character after the '_'.
    if (at + 1 < size && str[at] == '_')
      return at + 1;
    return std::string::npos;
  }

  // The next item of a ',' or '+' separated list, starting at 'from'.
  // Leading white space and double quotes are skipped (but never the very
  // last character) and trailing white space is dropped. An item that is
  // only white space comes back as the rest of the string from where the
  // skipping stopped (which then fails to parse as a single control). A
  // lone trailing quote gives an empty item. Returns where the item ended
  // (the separator's position, or the string's size).
  size_t NextListItem(const std::string &str, size_t from, char separator, std::string &item)
  {
    const size_t size = str.size();
    size_t first = from;
    while (first < size - 1 && (IsAsciiSpace(str[first]) || str[first] == '"'))
      first++;
    size_t end = str.find(separator, first);
    if (end == std::string::npos)
      end = size;
    size_t last = end;
    if (str[first] == '"')
      last = 0;  // (only possible for a quote in the last place)
    else
      while (last > 0 && IsAsciiSpace(str[last - 1]))
        last--;
    item = last >= first ? str.substr(first, last - first) : str.substr(first);
    return end;
  }

  //
  // RetroPixels: flight stick feel for a gamepad's thumbstick
  // (RP_FLIGHTSTICK)
  //
  // RP_FLIGHTSTICK (set by RetroPixels for Star Wars Trilogy Arcade) is
  // "curve=N,dead=N,smooth=N" (curve and dead in percent, smooth in ms), or
  // "off"/"0"/unset. A thumbstick has a round gate, a short throw and no
  // spring weight; to feel like the cabinet's yoke both axes of the stick
  // are read together and
  //   - the dead zone is round, and the travel past it starts from zero;
  //   - the round gate is stretched to a square, so a diagonal reaches the
  //     corner;
  //   - a cubic response curve gives fine aim near the centre;
  //   - the result is smoothed over time (an exponential filter with a time
  //     constant of 'smooth'), so jitter is calmed and letting go glides
  //     back to the centre.
  // Only whole stick axes (X/Y and RX/RY) are shaped.
  //

  struct FlightStick
  {
    bool on = false;
    double curve = 0.60;
    double dead = 0.05;
    double smooth = 0.060;  // seconds (0: no smoothing)
  };

  double Limit(double v, double lo, double hi)
  {
    return std::max(lo, std::min(hi, v));
  }

  const FlightStick &FlightStickSettings()
  {
    static const FlightStick settings = []()
    {
      FlightStick fs;
      const char *env = getenv("RP_FLIGHTSTICK");
      if (!env || !*env || !strcmp(env, "off") || !strcmp(env, "0"))
        return fs;
      fs.on = true;
      std::string spec(env);
      for (size_t pos = 0; pos < spec.size(); )
      {
        size_t comma = spec.find(',', pos);
        if (comma == std::string::npos)
          comma = spec.size();
        std::string item = spec.substr(pos, comma - pos);
        size_t eq = item.find('=');
        if (eq != std::string::npos)
        {
          std::string key = item.substr(0, eq);
          double percent = atof(item.c_str() + eq + 1) / 100.0;
          if (key == "curve")
            fs.curve = Limit(percent, 0.0, 1.0);
          else if (key == "dead")
            fs.dead = Limit(percent, 0.0, 0.5);
          else if (key == "smooth")
            fs.smooth = Limit(percent / 10.0, 0.0, 0.5);  // ms / 1000
        }
        pos = comma + 1;
      }
      return fs;
    }();
    return settings;
  }

  // A raw axis value as -1..1 around its rest value.
  double StickFraction(int v, int minVal, int offVal, int maxVal)
  {
    double d = (double)(v - offVal);
    double range = d > 0 ? (double)(maxVal - offVal) : (double)(offVal - minVal);
    if (range == 0)
      return 0;
    return Limit(d / std::fabs(range), -1.0, 1.0);
  }

  //
  // Digital controls (keys, buttons, POV directions): on/off, and for an
  // analog input a value that ramps up while held and back down after.
  //

  class DigitalSource : public CInputSource
  {
  public:
    DigitalSource(unsigned sensitivity, unsigned decaySpeed)
      : CInputSource(SourceSwitch), m_level(0)
    {
      // Sensitivity 1-100 %; decay 1-200 % of the rise.
      int s = Clamp((int)sensitivity, 1, 100);
      int d = Clamp((int)decaySpeed, 1, 200);
      m_rise = 100 * s;
      m_fall = d * s;
    }

    bool GetValueAsSwitch(bool &val) const
    {
      if (!Held())
        return false;
      val = true;
      return true;
    }

    bool GetValueAsAnalog(int &val, int minVal, int offVal, int maxVal)
    {
      m_level = Held() ? std::min(kFull, m_level + m_rise) : std::max(0, m_level - m_fall);
      if (m_level == 0)
        return false;
      val = Scale(m_level, 0, 0, kFull, minVal, offVal, maxVal);
      return true;
    }

  protected:
    virtual bool Held() const = 0;

  private:
    static constexpr int kFull = 10000;
    int m_rise;
    int m_fall;
    int m_level;
  };
}

//
// The sources
//

class CInputSystem::CKeyInputSource : public DigitalSource
{
public:
  CKeyInputSource(CInputSystem *system, int kbdNum, int keyIndex, unsigned sensitivity, unsigned decaySpeed)
    : DigitalSource(sensitivity, decaySpeed), m_system(system), m_kbdNum(kbdNum), m_keyIndex(keyIndex)
  {}

protected:
  bool Held() const { return m_system->IsKeyPressed(m_kbdNum, m_keyIndex); }

private:
  CInputSystem *m_system;
  int m_kbdNum;
  int m_keyIndex;
};

class CInputSystem::CMseButInputSource : public DigitalSource
{
public:
  CMseButInputSource(CInputSystem *system, int mseNum, int butNum, unsigned sensitivity, unsigned decaySpeed)
    : DigitalSource(sensitivity, decaySpeed), m_system(system), m_mseNum(mseNum), m_butNum(butNum)
  {}

protected:
  bool Held() const { return m_system->IsMouseButPressed(m_mseNum, m_butNum); }

private:
  CInputSystem *m_system;
  int m_mseNum;
  int m_butNum;
};

class CInputSystem::CJoyPOVInputSource : public DigitalSource
{
public:
  CJoyPOVInputSource(CInputSystem *system, int joyNum, int povNum, int povDir, unsigned sensitivity, unsigned decaySpeed)
    : DigitalSource(sensitivity, decaySpeed), m_system(system), m_joyNum(joyNum), m_povNum(povNum), m_povDir(povDir)
  {}

protected:
  bool Held() const { return m_system->IsJoyPOVInDir(m_joyNum, m_povNum, m_povDir); }

private:
  CInputSystem *m_system;
  int m_joyNum;
  int m_povNum;
  int m_povDir;
};

class CInputSystem::CJoyButInputSource : public DigitalSource
{
public:
  CJoyButInputSource(CInputSystem *system, int joyNum, int butNum, unsigned sensitivity, unsigned decaySpeed)
    : DigitalSource(sensitivity, decaySpeed), m_system(system), m_joyNum(joyNum), m_butNum(butNum)
  {}

protected:
  bool Held() const { return m_system->IsJoyButPressed(m_joyNum, m_butNum); }

private:
  CInputSystem *m_system;
  int m_joyNum;
  int m_butNum;
};

// A mouse axis. X and Y are measured from the middle of the display, with
// the dead zone (a share of the display size) around it; the wheel (Z) runs
// from -100 to 100.
class CInputSystem::CMseAxisInputSource : public CInputSource
{
public:
  CMseAxisInputSource(CInputSystem *system, int mseNum, int axisNum, int axisDir, unsigned deadZone)
    : CInputSource(axisDir == AXIS_FULL || axisDir == AXIS_INVERTED ? SourceFullAxis : SourceHalfAxis),
      m_system(system), m_mseNum(mseNum), m_axisNum(axisNum), m_axisDir(axisDir)
  {
    int percent = Clamp((int)deadZone, 0, 99);
    if (axisNum == AXIS_X || axisNum == AXIS_Y)
    {
      unsigned extent = axisNum == AXIS_X ? system->m_dispW : system->m_dispH;
      m_deadSize = (int)((double)percent / 100.0 * (double)extent);
    }
    else
      m_deadSize = percent;
  }

  bool GetValueAsSwitch(bool &val) const
  {
    if (m_axisNum == AXIS_Z)
    {
      // The wheel counts as pressed while it turns the right way.
      int dir = m_system->GetMouseWheelDir(m_mseNum);
      bool upward = m_axisDir == AXIS_POS || m_axisDir == AXIS_FULL;
      if (upward ? dir <= 0 : dir >= 0)
        return false;
    }
    else if (Position(0, 0, 3) < 2)
      return false;
    val = true;
    return true;
  }

  bool GetValueAsAnalog(int &val, int minVal, int offVal, int maxVal)
  {
    int v = Position(minVal, offVal, maxVal);
    if (v == offVal)
      return false;
    val = v;
    return true;
  }

private:
  int Position(int minVal, int offVal, int maxVal) const
  {
    int v = m_system->GetMouseAxisValue(m_mseNum, m_axisNum);
    int lo, hi;
    if (m_axisNum == AXIS_X || m_axisNum == AXIS_Y)
    {
      bool x = m_axisNum == AXIS_X;
      int extent = (int)(x ? m_system->m_dispW : m_system->m_dispH);
      if (extent == 0)
        return offVal;
      lo = -extent / 2;
      hi = lo + extent - 1;
      v = (int)(v - (x ? m_system->m_dispX : m_system->m_dispY)) - extent / 2;
    }
    else
    {
      lo = -100;
      hi = 100;
    }
    if (v == 0)
      return offVal;
    // The dead zone is split between the two sides (the odd pixel on the
    // negative one).
    int dead = v > 0 ? m_deadSize / 2 : m_deadSize - m_deadSize / 2;
    switch (m_axisDir)
    {
    case AXIS_POS: return Scale(v, dead, dead, hi, minVal, offVal, maxVal);
    case AXIS_NEG: return Scale(v, -dead, -dead, lo, minVal, offVal, maxVal);
    case AXIS_FULL:
      return v > 0 ? Scale(v, dead, dead, hi, minVal, offVal, maxVal) : Scale(v, lo, -dead, -dead, minVal, offVal, maxVal);
    default:  // AXIS_INVERTED
      return v > 0 ? Scale(v, dead, dead, hi, maxVal, offVal, minVal) : Scale(v, lo, -dead, -dead, maxVal, offVal, minVal);
    }
  }

  CInputSystem *m_system;
  int m_mseNum;
  int m_axisNum;
  int m_axisDir;
  int m_deadSize;  // pixels for X/Y, wheel units for Z
};

// A joystick axis, with its raw range, rest value, dead zone and saturation
// (both as shares of each half's travel).
class CInputSystem::CJoyAxisInputSource : public CInputSource
{
public:
  CJoyAxisInputSource(CInputSystem *system, int joyNum, int axisNum, int axisDir, int axisMinVal, int axisOffVal, int axisMaxVal,
    unsigned deadZone, unsigned saturation)
    : CInputSource(axisDir == AXIS_FULL || axisDir == AXIS_INVERTED ? SourceFullAxis : SourceHalfAxis),
      m_system(system), m_joyNum(joyNum), m_axisNum(axisNum), m_axisDir(axisDir),
      m_rawMin(axisMinVal), m_rawOff(axisOffVal), m_rawMax(axisMaxVal), m_reversed(axisMaxVal < axisMinVal)
  {
    double dead = (double)Clamp((int)deadZone, 0, 99) / 100.0;
    double sat = (double)Clamp((int)saturation, (int)deadZone + 1, 200) / 100.0;
    m_posDead = m_rawOff + (int)(dead * (m_rawMax - m_rawOff));
    m_negDead = m_rawOff + (int)(dead * (m_rawMin - m_rawOff));
    m_posSat = m_rawOff + (int)(sat * (m_rawMax - m_rawOff));
    m_negSat = m_rawOff + (int)(sat * (m_rawMin - m_rawOff));
  }

  bool GetValueAsSwitch(bool &val) const
  {
    if (Position(0, 0, 3) < 2)
      return false;
    val = true;
    return true;
  }

  bool GetValueAsAnalog(int &val, int minVal, int offVal, int maxVal)
  {
    int v = Position(minVal, offVal, maxVal);
    if (v == offVal)
      return false;
    val = v;
    return true;
  }

  bool SendForceFeedbackCmd(ForceFeedbackCmd ffCmd)
  {
    return m_system->SendForceFeedbackCmd(m_joyNum, m_axisNum, ffCmd);
  }

private:
  int Position(int minVal, int offVal, int maxVal) const
  {
    int raw = Shaped(m_system->GetJoyAxisValue(m_joyNum, m_axisNum));
    if (raw == m_rawOff)
      return offVal;
    if (m_axisDir == AXIS_POS)
      return Scale(raw, m_posDead, m_posDead, m_posSat, minVal, offVal, maxVal);
    if (m_axisDir == AXIS_NEG)
      return Scale(raw, m_negDead, m_negDead, m_negSat, minVal, offVal, maxVal);
    // Whole axis (reversed for AXIS_INVERTED); each half is scaled on its own.
    int toMin = m_axisDir == AXIS_FULL ? minVal : maxVal;
    int toMax = m_axisDir == AXIS_FULL ? maxVal : minVal;
    bool positiveHalf = m_reversed ? raw < m_rawOff : raw > m_rawOff;
    if (positiveHalf)
      return Scale(raw, m_posDead, m_posDead, m_posSat, toMin, offVal, toMax);
    return Scale(raw, m_negSat, m_negDead, m_negDead, toMin, offVal, toMax);
  }

  // RetroPixels: the raw value reshaped like a flight stick when
  // RP_FLIGHTSTICK is set (see above); otherwise unchanged.
  int Shaped(int raw) const
  {
    const FlightStick &fs = FlightStickSettings();
    if (!fs.on || (m_axisDir != AXIS_FULL && m_axisDir != AXIS_INVERTED) || m_rawMax <= m_rawMin)
      return raw;
    int other;
    switch (m_axisNum)
    {
    case AXIS_X:  other = AXIS_Y;  break;
    case AXIS_Y:  other = AXIS_X;  break;
    case AXIS_RX: other = AXIS_RY; break;
    case AXIS_RY: other = AXIS_RX; break;
    default:      return raw;
    }
    // The other axis of the stick is taken to have the same raw range (as
    // every gamepad read through SDL or XInput has).
    double u = StickFraction(raw, m_rawMin, m_rawOff, m_rawMax);
    double w = StickFraction(m_system->GetJoyAxisValue(m_joyNum, other), m_rawMin, m_rawOff, m_rawMax);
    double r = std::sqrt(u * u + w * w);
    double x = 0;
    if (r > fs.dead && r > 0)
    {
      double travel = std::min(1.0, (r - fs.dead) / (1.0 - fs.dead));  // past the round dead zone
      double square = std::max(std::fabs(u), std::fabs(w));               // round gate to square
      x = Limit(u * travel / square, -1.0, 1.0);
      double ax = std::fabs(x);
      ax = (1.0 - fs.curve) * ax + fs.curve * ax * ax * ax;
      x = x < 0 ? -ax : ax;
    }
    if (fs.smooth > 0)
    {
      // By elapsed time, so it feels the same at any frame rate and however
      // often the axis is read.
      auto now = std::chrono::steady_clock::now();
      double dt = m_smoothedYet ? std::chrono::duration<double>(now - m_smoothedAt).count() : 1.0;
      if (dt > 0.25)
        m_smoothed = x;  // not read for a while: start where the stick is
      else if (dt > 0)
        m_smoothed += (x - m_smoothed) * (1.0 - std::exp(-dt / fs.smooth));
      m_smoothedAt = now;
      m_smoothedYet = true;
      x = m_smoothed;
    }
    if (std::fabs(x) < 0.002)
      return m_rawOff;
    double span = x > 0 ? (double)(m_rawMax - m_rawOff) : (double)(m_rawOff - m_rawMin);
    int out = m_rawOff + (int)std::lround(x * span);
    if (out == m_rawOff)
      out += x > 0 ? 1 : -1;  // a pushed stick is never exactly centred
    return out;
  }

  CInputSystem *m_system;
  int m_joyNum;
  int m_axisNum;
  int m_axisDir;
  int m_rawMin;
  int m_rawOff;
  int m_rawMax;
  bool m_reversed;  // raw max below raw min
  int m_posDead;
  int m_negDead;
  int m_posSat;
  int m_negSat;

  // Flight stick smoothing state.
  mutable double m_smoothed = 0;
  mutable std::chrono::steady_clock::time_point m_smoothedAt;
  mutable bool m_smoothedYet = false;
};

//
// CInputSystem
//

const char *CInputSystem::GetDefaultAxisName(int axisNum)
{
  return axisNum >= 0 && axisNum < NUM_JOY_AXES ? kAxisNames[axisNum] : "";
}

CInputSystem::CInputSystem(const char *systemName)
  : m_numKbds(0), m_numMice(0), m_numJoys(0), m_haveCache(false),
    m_digitalSensitivity(DEFAULT_DIGITAL_SENSITIVITY), m_digitalDecaySpeed(DEFAULT_DIGITAL_DECAYSPEED),
    m_dispX(0), m_dispY(0), m_dispW(0), m_dispH(0), m_grabMouse(false), name(systemName)
{
  m_emptySource = new CMultiInputSource();
  m_emptySource->Acquire();
}

CInputSystem::~CInputSystem()
{
  m_emptySource->Release();
  ClearSettings();
  ClearSourceCache(true);
}

void CInputSystem::CreateSourceCache()
{
  // Slot 0 is "any device"; then one per device when they can be told apart.
  m_keySources.assign(1 + std::max(m_numKbds, 0), std::vector<CInputSource *>());
  m_mseSources.assign(1 + std::max(m_numMice, 0), std::vector<CInputSource *>(NUM_MOUSE_PARTS, nullptr));
  m_joySources.assign(1 + std::max(m_numJoys, 0), std::vector<CInputSource *>(NUM_JOY_PARTS, nullptr));
  m_haveCache = true;
}

void CInputSystem::ClearSourceCache(bool deleteCache)
{
  for (SourceCache *cache : { &m_keySources, &m_mseSources, &m_joySources })
  {
    for (auto &device : *cache)
    {
      for (CInputSource *&source : device)
      {
        if (source)
          source->Release();
        source = nullptr;
      }
    }
    if (deleteCache)
      cache->clear();
  }
  if (deleteCache)
    m_haveCache = false;
}

template <class Make>
CInputSource *CInputSystem::CachedSource(SourceCache &cache, int numDevs, int devNum, int part, Make make)
{
  // A numbered device that is not there (or numbers at all when devices
  // cannot be told apart) gives the empty source.
  if (devNum != -1 && devNum >= numDevs)
    return m_emptySource;
  size_t slot = devNum == -1 ? 0 : size_t(devNum) + 1;
  if (cache.size() <= slot)
    cache.resize(slot + 1);
  std::vector<CInputSource *> &parts = cache[slot];
  if (parts.size() <= size_t(part))
    parts.resize(size_t(part) + 1, nullptr);
  if (!parts[part])
  {
    parts[part] = make();
    parts[part]->Acquire();
  }
  return parts[part];
}

CInputSource *CInputSystem::GetKeySource(int kbdNum, int keyIndex)
{
  return CachedSource(m_keySources, m_numKbds, kbdNum, keyIndex, [&]()
  {
    if (kbdNum == ANY_KEYBOARD && m_numKbds != ANY_KEYBOARD)
      return CreateAnyKeySource(keyIndex);
    return CreateKeySource(kbdNum, keyIndex);
  });
}

CInputSource *CInputSystem::GetMouseSource(int mseNum, EMousePart msePart)
{
  return CachedSource(m_mseSources, m_numMice, mseNum, (int)msePart, [&]()
  {
    if (mseNum == ANY_MOUSE && m_numMice != ANY_MOUSE)
      return CreateAnyMouseSource(msePart);
    return CreateMouseSource(mseNum, msePart);
  });
}

CInputSource *CInputSystem::GetJoySource(int joyNum, EJoyPart joyPart)
{
  return CachedSource(m_joySources, m_numJoys, joyNum, (int)joyPart, [&]()
  {
    if (joyNum == ANY_JOYSTICK && m_numJoys != ANY_JOYSTICK)
      return CreateAnyJoySource(joyPart);
    return CreateJoySource(joyNum, joyPart);
  });
}

// Adds 'source' (now active) to the scan's list and its mapping to the text,
// unless it is already listed or to be ignored.
static void NoteActive(CInputSource *source, const char *dev, int devNum, const std::string &part,
  std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore)
{
  if (!source || !source->IsActive())
    return;
  if (std::find(sources.begin(), sources.end(), source) != sources.end() ||
      std::find(ignore.begin(), ignore.end(), source) != ignore.end())
    return;
  if (sources.empty())
    mapping.clear();
  else
    mapping += '+';
  mapping += dev;
  if (devNum >= 0)
    mapping += std::to_string(devNum + 1);
  mapping += '_';
  mapping += part;
  sources.push_back(source);
}

void CInputSystem::CheckAllSources(unsigned readFlags, bool fullAxisOnly, bool &mseCentered, std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore)
{
  if (readFlags & READ_KEYBOARD)
  {
    if ((readFlags & READ_MERGE_KEYBOARD) || m_numKbds == ANY_KEYBOARD)
      CheckKeySources(ANY_KEYBOARD, sources, mapping, ignore);
    else
      for (int k = 0; k < m_numKbds; k++)
        CheckKeySources(k, sources, mapping, ignore);
  }
  if (readFlags & READ_MOUSE)
  {
    if (!mseCentered)
      mseCentered = ConfigMouseCentered();
    if ((readFlags & READ_MERGE_MOUSE) || m_numMice == ANY_MOUSE)
      CheckMouseSources(ANY_MOUSE, fullAxisOnly, mseCentered, sources, mapping, ignore);
    else
      for (int m = 0; m < m_numMice; m++)
        CheckMouseSources(m, fullAxisOnly, mseCentered, sources, mapping, ignore);
  }
  if (readFlags & READ_JOYSTICK)
  {
    if ((readFlags & READ_MERGE_JOYSTICK) || m_numJoys == ANY_JOYSTICK)
      CheckJoySources(ANY_JOYSTICK, fullAxisOnly, sources, mapping, ignore);
    else
      for (int j = 0; j < m_numJoys; j++)
        CheckJoySources(j, fullAxisOnly, sources, mapping, ignore);
  }
}

void CInputSystem::CheckKeySources(int kbdNum, std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore)
{
  for (const char *keyName : kKeyNames)
  {
    int keyIndex = GetKeyIndex(keyName);
    if (keyIndex >= 0)
      NoteActive(GetKeySource(kbdNum, keyIndex), "KEY", kbdNum, keyName, sources, mapping, ignore);
  }
}

void CInputSystem::CheckMouseSources(int mseNum, bool fullAxisOnly, bool mseCentered, std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore)
{
  for (int p = 0; p < NUM_MOUSE_PARTS; p++)
  {
    EMousePart part = (EMousePart)p;
    int axisNum, axisDir;
    if (GetAxisDetails(part, axisNum, axisDir))
    {
      // X/Y only once the pointer has been centred; whole or half axes as asked.
      if ((axisNum != AXIS_Z && !mseCentered) || IsFullAxis(part) != fullAxisOnly)
        continue;
    }
    NoteActive(GetMouseSource(mseNum, part), "MOUSE", mseNum, MouseNames().canonical[p], sources, mapping, ignore);
  }
}

void CInputSystem::CheckJoySources(int joyNum, bool fullAxisOnly, std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore)
{
  for (int p = 0; p < NUM_JOY_PARTS; p++)
  {
    EJoyPart part = (EJoyPart)p;
    if (IsAxis(part) && IsFullAxis(part) != fullAxisOnly)
      continue;
    NoteActive(GetJoySource(joyNum, part), "JOY", joyNum, JoyNames().canonical[p], sources, mapping, ignore);
  }
}

CInputSource* CInputSystem::ParseMultiSource(const std::string& str, bool fullAxisOnly, bool isOr)
{
  if (str.empty() || SameText(str, "NONE"))
    return nullptr;

  // "A,B" items are themselves "A+B" lists.
  std::vector<CInputSource*> parsed;
  const char separator = isOr ? ',' : '+';
  size_t at = 0;
  do
  {
    std::string item;
    at = NextListItem(str, at, separator, item) + 1;
    CInputSource *source = isOr ? ParseMultiSource(item, fullAxisOnly, false) : ParseSingleSource(item);
    if (!source || source->type == SourceInvalid)
      continue;
    // Axis inputs take whole axes only; the others anything but.
    bool fits = source->type == SourceEmpty || ((source->type == SourceFullAxis) == fullAxisOnly);
    if (fits)
      parsed.push_back(source);
  }
  while (at < str.size());

  if (parsed.empty())
    return nullptr;
  if (parsed.size() == 1)
    return parsed[0];
  return new CMultiInputSource(isOr, parsed);
}

CInputSource* CInputSystem::ParseKeyName(int kbdNum, const std::string &keyName)
{
  int keyIndex = GetKeyIndex(keyName.c_str());
  if (keyIndex >= 0)
    return GetKeySource(kbdNum, keyIndex);
  // SHIFT, CTRL and ALT mean either one of the pair when the back end has
  // no key of its own for them.
  if (SameText(keyName, "SHIFT") || SameText(keyName, "CTRL") || SameText(keyName, "ALT"))
  {
    std::vector<CInputSource*> sides;
    for (const char *side : { "LEFT", "RIGHT" })
    {
      int index = GetKeyIndex((side + keyName).c_str());
      if (index >= 0)
        if (CInputSource *s = GetKeySource(kbdNum, index))
          sides.push_back(s);
    }
    if (!sides.empty())
      return new CMultiInputSource(true, sides);
  }
  return m_emptySource;
}

CInputSource *CInputSystem::ParseSingleSource(std::string str)
{
  // "!A": active while A is not (spaces may follow the '!').
  if (!str.empty() && str[0] == '!')
  {
    size_t skip = 1;
    while (skip < str.size() && str[skip] == ' ')
      skip++;
    CInputSource *inner = ParseSingleSource(str.substr(skip));
    if (inner && inner != m_emptySource)
      return new CNegInputSource(inner);
    return inner;
  }

  int devNum;
  size_t part = ParseDeviceMapping(str, "KEY", devNum);
  if (part != std::string::npos && IsKeyName(str.substr(part)))
    return ParseKeyName(devNum, str.substr(part));

  part = ParseDeviceMapping(str, "MOUSE", devNum);
  if (part != std::string::npos)
  {
    int mousePart = MouseNames().Find(str.substr(part));
    if (mousePart >= 0)
      return GetMouseSource(devNum, (EMousePart)mousePart);
  }

  part = ParseDeviceMapping(str, "JOY", devNum);
  if (part != std::string::npos)
  {
    int joyPart = JoyNames().Find(str.substr(part));
    if (joyPart >= 0)
      return GetJoySource(devNum, (EJoyPart)joyPart);
  }

  // A bare key name, as older ini files have.
  if (IsKeyName(str))
  {
    int keyIndex = GetKeyIndex(str.c_str());
    return keyIndex >= 0 ? GetKeySource(ANY_KEYBOARD, keyIndex) : m_emptySource;
  }
  return nullptr;
}

//
// Settings
//

void CInputSystem::PrintCommonSettings() const
{
  printf(" Sensitivity = %u %%\n", m_digitalSensitivity);
  printf(" Decay Speed = %u %%\n", m_digitalDecaySpeed);
}

void CInputSystem::LoadCommonSettings(const Util::Config::Node &config)
{
  m_digitalSensitivity = config["InputDigitalSensitivity"].ValueAsDefault<unsigned>(DEFAULT_DIGITAL_SENSITIVITY);
  m_digitalDecaySpeed = config["InputDigitalDecaySpeed"].ValueAsDefault<unsigned>(DEFAULT_DIGITAL_DECAYSPEED);
}

void CInputSystem::StoreCommonSettings(Util::Config::Node *config)
{
  config->Set("InputDigitalSensitivity", m_digitalSensitivity);
  config->Set("InputDigitalDecaySpeed", m_digitalDecaySpeed);
}

// "Input<Kind>" for the shared settings, "Input<Kind><n>" for device n-1.
static std::string SettingsKey(const char *kind, int devNum)
{
  std::string key = std::string("Input") + kind;
  if (devNum != -1)
    key += std::to_string(devNum + 1);
  return key;
}

void CInputSystem::PrintKeySettings(int, KeySettings *settings)
{
  printf(" Sensitivity = %u %%\n", settings->sensitivity);
  printf(" Decay Speed = %u %%\n", settings->decaySpeed);
}

KeySettings *CInputSystem::LoadKeySettings(const Util::Config::Node &config, int kbdNum)
{
  // A keyboard starts from the shared keyboard settings, which start from
  // the defaults. Without a setting of its own a keyboard takes the
  // InputDigital... value (and then counts as configured if that differs).
  KeySettings *base = kbdNum != ANY_KEYBOARD ? GetKeySettings(ANY_KEYBOARD, true) : &m_defKeySettings;
  KeySettings *settings = new KeySettings(*base);
  settings->kbdNum = kbdNum;
  std::string key = SettingsKey("Key", kbdNum);
  bool found = false;

  if (const Util::Config::Node *node = config.TryGet(key + "Sensitivity"))
  {
    settings->sensitivity = node->ValueAs<unsigned>();
    found = true;
  }
  else
  {
    settings->sensitivity = m_digitalSensitivity;
    found |= settings->sensitivity != base->sensitivity;
  }
  if (const Util::Config::Node *node = config.TryGet(key + "DecaySpeed"))
  {
    settings->decaySpeed = node->ValueAs<unsigned>();
    found = true;
  }
  else
  {
    settings->decaySpeed = m_digitalDecaySpeed;
    found |= settings->decaySpeed != base->decaySpeed;
  }

  if (found)
    return settings;
  delete settings;
  return nullptr;
}

void CInputSystem::StoreKeySettings(Util::Config::Node *config, KeySettings *settings)
{
  // Only what differs from the shared (or default) settings is written.
  KeySettings *base = settings->kbdNum != ANY_KEYBOARD ? GetKeySettings(ANY_KEYBOARD, true) : &m_defKeySettings;
  std::string key = SettingsKey("Key", settings->kbdNum);
  if (settings->sensitivity != base->sensitivity)
    config->Set(key + "Sensitivity", settings->sensitivity);
  if (settings->decaySpeed != base->decaySpeed)
    config->Set(key + "DecaySpeed", settings->decaySpeed);
}

void CInputSystem::PrintMouseSettings(int, MouseSettings *settings)
{
  for (int a = 0; a < NUM_MOUSE_AXES; a++)
    printf(" %-7s Dead Zone = %u %%\n", kAxisNames[a], settings->deadZones[a]);
}

MouseSettings *CInputSystem::LoadMouseSettings(const Util::Config::Node &config, int mseNum)
{
  MouseSettings *base = mseNum != ANY_MOUSE ? GetMouseSettings(ANY_MOUSE, true) : &m_defMseSettings;
  MouseSettings *settings = new MouseSettings(*base);
  settings->mseNum = mseNum;
  std::string key = SettingsKey("Mouse", mseNum);
  bool found = false;
  for (int a = 0; a < NUM_MOUSE_AXES; a++)
  {
    if (const Util::Config::Node *node = config.TryGet(key + kAxisIds[a] + "DeadZone"))
    {
      settings->deadZones[a] = node->ValueAs<unsigned>();
      found = true;
    }
  }
  if (found)
    return settings;
  delete settings;
  return nullptr;
}

void CInputSystem::StoreMouseSettings(Util::Config::Node *config, MouseSettings *settings)
{
  MouseSettings *base = settings->mseNum != ANY_MOUSE ? GetMouseSettings(ANY_MOUSE, true) : &m_defMseSettings;
  std::string key = SettingsKey("Mouse", settings->mseNum);
  for (int a = 0; a < NUM_MOUSE_AXES; a++)
    if (settings->deadZones[a] != base->deadZones[a])
      config->Set(key + kAxisIds[a] + "DeadZone", settings->deadZones[a]);
}

void CInputSystem::PrintJoySettings(int joyNum, JoySettings *settings)
{
  const JoyDetails *joy = joyNum != ANY_JOYSTICK ? GetJoyDetails(joyNum) : nullptr;
  for (int a = 0; a < NUM_JOY_AXES; a++)
  {
    if (joy && !joy->hasAxis[a])
      continue;
    const char *n = kAxisNames[a];
    printf(" %-7s Min Value        = %d\n", n, settings->axisMinVals[a]);
    printf(" %-7s Center/Off Value = %d\n", n, settings->axisOffVals[a]);
    printf(" %-7s Max Value        = %d\n", n, settings->axisMaxVals[a]);
    printf(" %-7s Dead Zone        = %u %%\n", n, settings->deadZones[a]);
    printf(" %-7s Saturation       = %u %%\n", n, settings->saturations[a]);
  }
}

JoySettings *CInputSystem::LoadJoySettings(const Util::Config::Node &config, int joyNum)
{
  JoySettings *base = joyNum != ANY_JOYSTICK ? GetJoySettings(ANY_JOYSTICK, true) : &m_defJoySettings;
  JoySettings *settings = new JoySettings(*base);
  settings->joyNum = joyNum;
  std::string key = SettingsKey("Joy", joyNum);
  bool found = false;
  for (int a = 0; a < NUM_JOY_AXES; a++)
  {
    std::string axisKey = key + kAxisIds[a];
    auto readInt = [&](const char *what, int &out)
    {
      if (const Util::Config::Node *node = config.TryGet(axisKey + what))
      {
        out = node->ValueAs<int>();
        found = true;
      }
    };
    auto readUnsigned = [&](const char *what, unsigned &out)
    {
      if (const Util::Config::Node *node = config.TryGet(axisKey + what))
      {
        out = node->ValueAs<unsigned>();
        found = true;
      }
    };
    readInt("MinVal", settings->axisMinVals[a]);
    readInt("OffVal", settings->axisOffVals[a]);
    readInt("MaxVal", settings->axisMaxVals[a]);
    readUnsigned("DeadZone", settings->deadZones[a]);
    readUnsigned("Saturation", settings->saturations[a]);
  }
  if (found)
    return settings;
  delete settings;
  return nullptr;
}

void CInputSystem::StoreJoySettings(Util::Config::Node *config, JoySettings *settings)
{
  JoySettings *base = settings->joyNum != ANY_JOYSTICK ? GetJoySettings(ANY_JOYSTICK, true) : &m_defJoySettings;
  std::string key = SettingsKey("Joy", settings->joyNum);
  for (int a = 0; a < NUM_JOY_AXES; a++)
  {
    std::string axisKey = key + kAxisIds[a];
    if (settings->axisMinVals[a] != base->axisMinVals[a])
      config->Set(axisKey + "MinVal", settings->axisMinVals[a]);
    if (settings->axisOffVals[a] != base->axisOffVals[a])
      config->Set(axisKey + "OffVal", settings->axisOffVals[a]);
    if (settings->axisMaxVals[a] != base->axisMaxVals[a])
      config->Set(axisKey + "MaxVal", settings->axisMaxVals[a]);
    if (settings->deadZones[a] != base->deadZones[a])
      config->Set(axisKey + "DeadZone", settings->deadZones[a]);
    if (settings->saturations[a] != base->saturations[a])
      config->Set(axisKey + "Saturation", settings->saturations[a]);
  }
}

// The entry for 'devNum', else (with useDefault) the shared entry or the
// defaults.
template <class Settings>
static Settings *FindSettings(std::vector<Settings *> &list, int devNum, bool useDefault, Settings *defaults, int Settings::*number)
{
  Settings *shared = nullptr;
  for (Settings *s : list)
  {
    if (s->*number == devNum)
      return s;
    if (s->*number == -1)
      shared = s;
  }
  if (!useDefault)
    return nullptr;
  return shared ? shared : defaults;
}

KeySettings *CInputSystem::GetKeySettings(int kbdNum, bool useDefault)
{
  return FindSettings(m_keySettings, kbdNum, useDefault, &m_defKeySettings, &KeySettings::kbdNum);
}

MouseSettings *CInputSystem::GetMouseSettings(int mseNum, bool useDefault)
{
  return FindSettings(m_mseSettings, mseNum, useDefault, &m_defMseSettings, &MouseSettings::mseNum);
}

JoySettings *CInputSystem::GetJoySettings(int joyNum, bool useDefault)
{
  return FindSettings(m_joySettings, joyNum, useDefault, &m_defJoySettings, &JoySettings::joyNum);
}

//
// Parts
//

bool CInputSystem::IsAxis(EMousePart msePart)
{
  return msePart >= MouseXAxis && msePart <= MouseZAxisNeg;
}

bool CInputSystem::IsFullAxis(EMousePart msePart)
{
  int axisNum, axisDir;
  return GetAxisDetails(msePart, axisNum, axisDir) && (axisDir == AXIS_FULL || axisDir == AXIS_INVERTED);
}

bool CInputSystem::GetAxisDetails(EMousePart msePart, int &axisNum, int &axisDir)
{
  if (!IsAxis(msePart))
    return false;
  axisNum = (msePart - MouseXAxis) / 4;
  axisDir = (msePart - MouseXAxis) % 4;
  return true;
}

bool CInputSystem::IsButton(EMousePart msePart)
{
  return msePart >= MouseButtonLeft && msePart <= MouseButtonX2;
}

int CInputSystem::GetButtonNumber(EMousePart msePart)
{
  return IsButton(msePart) ? msePart - MouseButtonLeft : -1;
}

EMousePart CInputSystem::GetMouseAxis(int axisNum, int axisDir)
{
  if (axisNum < 0 || axisNum >= NUM_MOUSE_AXES || axisDir < 0 || axisDir > 3)
    return MouseUnknown;
  return (EMousePart)(MouseXAxis + 4 * axisNum + axisDir);
}

EMousePart CInputSystem::GetMouseButton(int butNum)
{
  if (butNum < 0 || butNum >= NUM_MOUSE_BUTTONS)
    return MouseUnknown;
  return (EMousePart)(MouseButtonLeft + butNum);
}

bool CInputSystem::IsAxis(EJoyPart joyPart)
{
  return joyPart >= JoyXAxis && joyPart <= JoyS2AxisNeg;
}

bool CInputSystem::IsFullAxis(EJoyPart joyPart)
{
  int axisNum, axisDir;
  return GetAxisDetails(joyPart, axisNum, axisDir) && (axisDir == AXIS_FULL || axisDir == AXIS_INVERTED);
}

bool CInputSystem::IsSliderAxis(EJoyPart joyPart)
{
  return joyPart >= JoyS1Axis && joyPart <= JoyS2AxisNeg;
}

bool CInputSystem::GetAxisDetails(EJoyPart joyPart, int &axisNum, int &axisDir)
{
  if (!IsAxis(joyPart))
    return false;
  axisNum = (joyPart - JoyXAxis) / 4;
  axisDir = (joyPart - JoyXAxis) % 4;
  return true;
}

bool CInputSystem::IsPOV(EJoyPart joyPart)
{
  return joyPart >= JoyPOV0Up && joyPart <= JoyPOV3Right;
}

bool CInputSystem::GetPOVDetails(EJoyPart joyPart, int &povNum, int &povDir)
{
  if (!IsPOV(joyPart))
    return false;
  povNum = (joyPart - JoyPOV0Up) / 4;
  povDir = (joyPart - JoyPOV0Up) % 4;
  return true;
}

bool CInputSystem::IsButton(EJoyPart joyPart)
{
  return joyPart >= JoyButton0 && joyPart <= JoyButton31;
}

int CInputSystem::GetButtonNumber(EJoyPart joyPart)
{
  return IsButton(joyPart) ? joyPart - JoyButton0 : -1;
}

EJoyPart CInputSystem::GetJoyAxis(int axisNum, int axisDir)
{
  if (axisNum < 0 || axisNum >= NUM_JOY_AXES || axisDir < 0 || axisDir > 3)
    return JoyUnknown;
  return (EJoyPart)(JoyXAxis + 4 * axisNum + axisDir);
}

EJoyPart CInputSystem::GetJoyPOV(int povNum, int povDir)
{
  if (povNum < 0 || povNum >= NUM_JOY_POVS)
    return JoyUnknown;
  return (EJoyPart)(JoyPOV0Up + 4 * povNum + povDir);
}

EJoyPart CInputSystem::GetJoyButton(int butNum)
{
  if (butNum < 0 || butNum >= NUM_JOY_BUTTONS)
    return JoyUnknown;
  return (EJoyPart)(JoyButton0 + butNum);
}

bool CInputSystem::ConfigMouseCentered()
{
  // Inside the middle half of the display, edges included.
  int x = GetMouseAxisValue(ANY_MOUSE, AXIS_X);
  int y = GetMouseAxisValue(ANY_MOUSE, AXIS_Y);
  unsigned left = m_dispX + m_dispW / 4;
  unsigned top = m_dispY + m_dispH / 4;
  return x >= (int)left && x <= (int)(left + m_dispW / 2) && y >= (int)top && y <= (int)(top + m_dispH / 2);
}

//
// Creating sources
//

CInputSource *CInputSystem::CreateAnyKeySource(int keyIndex)
{
  std::vector<CInputSource*> each;
  for (int k = 0; k < m_numKbds; k++)
    if (CInputSource *s = CreateKeySource(k, keyIndex))
      each.push_back(s);
  return new CMultiInputSource(true, each);
}

CInputSource *CInputSystem::CreateAnyMouseSource(EMousePart msePart)
{
  std::vector<CInputSource*> each;
  for (int m = 0; m < m_numMice; m++)
    if (CInputSource *s = CreateMouseSource(m, msePart))
      each.push_back(s);
  return new CMultiInputSource(true, each);
}

CInputSource *CInputSystem::CreateAnyJoySource(EJoyPart joyPart)
{
  std::vector<CInputSource*> each;
  for (int j = 0; j < m_numJoys; j++)
    if (CInputSource *s = CreateJoySource(j, joyPart))
      each.push_back(s);
  return new CMultiInputSource(true, each);
}

CInputSource *CInputSystem::CreateKeySource(int kbdNum, int keyIndex)
{
  KeySettings *settings = GetKeySettings(kbdNum, true);
  return new CKeyInputSource(this, kbdNum, keyIndex, settings->sensitivity, settings->decaySpeed);
}

CInputSource *CInputSystem::CreateMouseSource(int mseNum, EMousePart msePart)
{
  MouseSettings *settings = GetMouseSettings(mseNum, true);
  int axisNum, axisDir;
  if (GetAxisDetails(msePart, axisNum, axisDir))
    return new CMseAxisInputSource(this, mseNum, axisNum, axisDir, settings->deadZones[axisNum]);
  int button = GetButtonNumber(msePart);
  if (button >= 0)
    return new CMseButInputSource(this, mseNum, button, m_digitalSensitivity, m_digitalDecaySpeed);
  return nullptr;
}

CInputSource *CInputSystem::CreateJoySource(int joyNum, EJoyPart joyPart)
{
  // A part the joystick lacks is not an error; it just never does anything.
  const JoyDetails *joy = GetJoyDetails(joyNum);
  JoySettings *settings = GetJoySettings(joyNum, true);
  int num, dir;
  if (GetAxisDetails(joyPart, num, dir))
  {
    if (!joy->hasAxis[num])
      return m_emptySource;
    return new CJoyAxisInputSource(this, joyNum, num, dir, settings->axisMinVals[num], settings->axisOffVals[num], settings->axisMaxVals[num],
      settings->deadZones[num], settings->saturations[num]);
  }
  if (GetPOVDetails(joyPart, num, dir))
  {
    if (num >= joy->numPOVs)
      return m_emptySource;
    return new CJoyPOVInputSource(this, joyNum, num, dir, m_digitalSensitivity, m_digitalDecaySpeed);
  }
  int button = GetButtonNumber(joyPart);
  if (button >= 0)
  {
    if (button >= joy->numButtons)
      return m_emptySource;
    return new CJoyButInputSource(this, joyNum, button, m_digitalSensitivity, m_digitalDecaySpeed);
  }
  return nullptr;
}

//
// Public interface
//

bool CInputSystem::Initialize()
{
  if (!InitializeSystem())
    return false;
  m_numKbds = GetNumKeyboards();
  m_numMice = GetNumMice();
  m_numJoys = GetNumJoysticks();
  CreateSourceCache();
  GrabMouse();
  return true;
}

void CInputSystem::SetDisplayGeom(unsigned dispX, unsigned dispY, unsigned dispW, unsigned dispH)
{
  m_dispX = dispX;
  m_dispY = dispY;
  m_dispW = dispW;
  m_dispH = dispH;
}

CInputSource* CInputSystem::ParseSource(const char *mapping, bool fullAxisOnly)
{
  return ParseMultiSource(mapping, fullAxisOnly, true);
}

void CInputSystem::ClearSettings()
{
  for (KeySettings *s : m_keySettings)
    delete s;
  m_keySettings.clear();
  for (MouseSettings *s : m_mseSettings)
    delete s;
  m_mseSettings.clear();
  for (JoySettings *s : m_joySettings)
    delete s;
  m_joySettings.clear();
}

void CInputSystem::PrintSettings()
{
  puts("Input System Settings");
  puts("---------------------");
  puts("");
  printf("Input System: %s\n", name);
  puts("");
  PrintDevices();
  puts("");

  puts("Common Settings:");
  PrintCommonSettings();

  if (m_numKbds == ANY_KEYBOARD)
  {
    puts("Common Keyboard Settings:");
    PrintKeySettings(ANY_KEYBOARD, GetKeySettings(ANY_KEYBOARD, true));
  }
  else
  {
    for (int k = 0; k < m_numKbds; k++)
    {
      printf("Keyboard %d Settings:\n", k + 1);
      PrintKeySettings(k, GetKeySettings(k, true));
    }
  }

  if (m_numMice == ANY_MOUSE)
  {
    puts("Common Mouse Settings:");
    PrintMouseSettings(ANY_MOUSE, GetMouseSettings(ANY_MOUSE, true));
  }
  else
  {
    for (int m = 0; m < m_numMice; m++)
    {
      printf("Mouse %d Settings:\n", m + 1);
      PrintMouseSettings(m, GetMouseSettings(m, true));
    }
  }

  if (m_numJoys == ANY_JOYSTICK)
  {
    puts("Common Joystick Settings:");
    PrintJoySettings(ANY_JOYSTICK, GetJoySettings(ANY_JOYSTICK, true));
  }
  else
  {
    for (int j = 0; j < m_numJoys; j++)
    {
      printf("Joystick %d Settings:\n", j + 1);
      PrintJoySettings(j, GetJoySettings(j, true));
    }
  }

  puts("");
}

void CInputSystem::LoadFromConfig(const Util::Config::Node &config)
{
  ClearSettings();
  ClearSourceCache();
  LoadCommonSettings(config);

  // The shared settings first, so each device's can start from them (the
  // shared ones are read even when the count is ANY_xxx, i.e. -1).
  auto loadAll = [&](auto &list, int numDevs, auto load)
  {
    if (auto *s = load(-1))  // ANY_KEYBOARD / ANY_MOUSE / ANY_JOYSTICK
      list.push_back(s);
    for (int n = 0; n < numDevs; n++)
      if (auto *s = load(n))
        list.push_back(s);
  };
  loadAll(m_keySettings, m_numKbds, [&](int n) { return LoadKeySettings(config, n); });
  loadAll(m_mseSettings, m_numMice, [&](int n) { return LoadMouseSettings(config, n); });
  loadAll(m_joySettings, m_numJoys, [&](int n) { return LoadJoySettings(config, n); });
}

void CInputSystem::StoreToConfig(Util::Config::Node *config)
{
  StoreCommonSettings(config);
  for (KeySettings *s : m_keySettings)
    StoreKeySettings(config, s);
  for (MouseSettings *s : m_mseSettings)
    StoreMouseSettings(config, s);
  for (JoySettings *s : m_joySettings)
    StoreJoySettings(config, s);
}

bool CInputSystem::WaitForRelease(CInputSource *source)
{
  while (source->IsActive())
  {
    if (!Poll())
      return false;
    CThread::Sleep(1000 / 60);
  }
  return true;
}

bool CInputSystem::ReadMapping(char *buffer, unsigned bufSize, bool fullAxisOnly, unsigned readFlags, const char *escapeMapping)
{
  CInputSource *escape = ParseSource(escapeMapping);
  if (escape)
    escape->Acquire();

  // Whatever is already active when we start (such as a badly calibrated
  // axis that never returns to rest) is ignored from then on.
  std::vector<CInputSource*> stuck;
  std::vector<CInputSource*> pressed;
  std::string unused;
  std::string mapping;
  bool mseCentered = false;
  bool ok = Poll();
  if (ok)
    CheckAllSources(readFlags, fullAxisOnly, mseCentered, stuck, unused, pressed);

  while (ok)
  {
    if (!Poll())
    {
      ok = false;
      break;
    }
    if (escape && escape->IsActive())
    {
      WaitForRelease(escape);
      ok = false;
      break;
    }

    CheckAllSources(readFlags, fullAxisOnly, mseCentered, pressed, mapping, stuck);

    // Once something has been operated, wait for all of it to be let go.
    if (!pressed.empty())
    {
      bool held = false;
      for (CInputSource *s : pressed)
        if (s->IsActive())
        {
          held = true;
          break;
        }
      if (!held)
      {
        ESourceType type = CMultiInputSource::GetCombinedType(pressed);
        if (type != SourceInvalid && ((type == SourceFullAxis) == fullAxisOnly))
          break;
        // Not a usable combination for this input: start again.
        mapping.clear();
        pressed.clear();
        mseCentered = false;
      }
    }
    CThread::Sleep(1000 / 60);
  }

  if (ok)
  {
    strncpy(buffer, mapping.c_str(), bufSize - 1);
    buffer[bufSize - 1] = '\0';
  }
  if (escape)
    escape->Release();
  return ok;
}

void CInputSystem::GrabMouse()
{
  m_grabMouse = true;
}

void CInputSystem::UngrabMouse()
{
  m_grabMouse = false;
  SetMouseVisibility(true);
}

bool CInputSystem::SendForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd)
{
  const JoyDetails *joy = GetJoyDetails(joyNum);
  if (!joy->hasFFeedback || !joy->axisHasFF[axisNum])
    return false;
  return ProcessForceFeedbackCmd(joyNum, axisNum, ffCmd);
}

// Sources for the console tools' keys, held for the duration of a tool.
namespace
{
  class HeldSources
  {
  public:
    void Add(CInputSource *&slot, CInputSource *source)
    {
      slot = source;
      if (source)
      {
        source->Acquire();
        m_held.push_back(source);
      }
    }

    ~HeldSources()
    {
      for (CInputSource *s : m_held)
        s->Release();
    }

  private:
    std::vector<CInputSource *> m_held;
  };
}

bool CInputSystem::DetectJoystickAxis(unsigned joyNum, unsigned &axisNum, const char *escapeMapping, const char *confirmMapping)
{
  const JoyDetails *joy = GetJoyDetails(joyNum);
  if (!joy)
  {
    puts("No such joystick!");
    return false;
  }

  HeldSources held;
  CInputSource *escape, *confirm, *show;
  held.Add(escape, ParseSource(escapeMapping));
  held.Add(confirm, ParseSource(confirmMapping));
  held.Add(show, ParseSource("KEY_SHIFT"));  // hold Shift to see the raw values

  printf("Move axis around and then press Return (or press Esc to cancel): ");
  fflush(stdout);

  int lowest[NUM_JOY_AXES];
  int highest[NUM_JOY_AXES];
  for (unsigned a = 0; a < NUM_JOY_AXES; a++)
    if (joy->hasAxis[a])
      lowest[a] = highest[a] = GetJoyAxisValue(joyNum, a);

  bool firstShow = true;
  bool cancelled = false;
  for (;;)
  {
    if (!Poll())
    {
      cancelled = true;
      break;
    }
    if (escape && escape->IsActive())
    {
      WaitForRelease(escape);
      cancelled = true;
      break;
    }
    if (confirm && confirm->IsActive())
    {
      if (!WaitForRelease(confirm))
        cancelled = true;
      break;
    }
    for (unsigned a = 0; a < NUM_JOY_AXES; a++)
    {
      if (!joy->hasAxis[a])
        continue;
      int v = GetJoyAxisValue(joyNum, a);
      lowest[a] = std::min(lowest[a], v);
      highest[a] = std::max(highest[a], v);
    }
    if (show && show->IsActive())
    {
      if (firstShow)
        puts("");
      printf(" [");
      for (unsigned a = 0; a < NUM_JOY_AXES; a++)
        printf("%s%s: %d", a > 0 ? ", " : "", kAxisIds[a], GetJoyAxisValue(joyNum, a));
      puts("]");
      firstShow = false;
    }
    CThread::Sleep(1000 / 60);
  }

  if (cancelled)
  {
    puts("[Cancelled]");
    return false;
  }

  // The axis that moved the furthest, if it moved far enough.
  unsigned widest = 0;
  for (unsigned a = 0; a < NUM_JOY_AXES; a++)
  {
    if (!joy->hasAxis[a])
      continue;
    unsigned range = unsigned(highest[a] - lowest[a]);
    if (range > widest)
    {
      widest = range;
      axisNum = a;
    }
  }
  if (widest > 3000)
  {
    printf("Detected %s\n", joy->axisName[axisNum]);
    return true;
  }
  puts("Not Detected");
  return false;
}

// The instructions for each calibration step.
static void PrintCalibrationStep(unsigned step, unsigned axisNum)
{
  bool horizontal = axisNum == AXIS_X || axisNum == AXIS_RX || axisNum == AXIS_Z || axisNum == AXIS_RZ;
  bool vertical = axisNum == AXIS_Y || axisNum == AXIS_RY || axisNum == AXIS_Z || axisNum == AXIS_RZ;
  bool slider = axisNum == AXIS_S1 || axisNum == AXIS_S2;
  switch (step)
  {
  case 0:
    puts("Step 1:");
    puts(" Move axis now to its furthest positive/'on' position and hold, ie:");
    if (horizontal) puts(" - for a horizontal joystick axis, push it all the way to the right.");
    if (vertical)   puts(" - for a vertical joystick axis, push it all the way downwards.");
    if (slider)     puts(" - for a joystick slider, push it all the way to its extreme.");
    puts(" - for a steering wheel, turn it all the way to the right.");
    puts(" - for a pedal, press it all the way to the floor.");
    break;
  case 1:
    puts("Step 2:");
    puts(" Move axis the other way to its furthest negative position and hold, ie:");
    if (horizontal) puts(" - for a horizontal joystick axis, push it all the way to the left.");
    if (vertical)   puts(" - for a vertical joystick axis, push it all the way updwards.");
    if (slider)     puts(" - for a joystick slider, return it to its off position.");
    puts(" - for a steering wheel, turn it all the way to the left.");
    puts(" - for a pedal, let go of the pedal completely.  If there is another pedal");
    puts("   that shares the same axis then press that one all the way to the floor.");
    break;
  default:
    puts("Step 3:");
    puts(" Return axis to its central/'off' position and hold, ie:");
    if (horizontal || vertical) puts(" - for a joystick axis, let it return to the middle.");
    else if (slider)            puts(" - for a joystick slider, keep it at its off position.");
    puts(" - for a steering weel, turn it back to the center.");
    puts(" - for a pedal, let go of pedal completely.  Likewise for any other pedal");
    puts("   that shares the same axis.");
    break;
  }
}

bool CInputSystem::CalibrateJoystickAxis(unsigned joyNum, unsigned axisNum, const char *escapeMapping, const char *confirmMapping)
{
  const JoyDetails *joy = GetJoyDetails(joyNum);
  if (!joy || axisNum >= NUM_JOY_AXES || !joy->hasAxis[axisNum])
  {
    puts("No such axis or joystick!");
    return false;
  }

  HeldSources held;
  CInputSource *escape, *show;
  held.Add(escape, ParseSource(escapeMapping));
  held.Add(show, ParseSource("KEY_SHIFT"));

  char key[50];
  // Reads keys until 'confirm' (true), 'yes'/'no' (true, with answer), or
  // Esc / the window closing (false).
  auto readKey = [&](const char *want) -> bool
  {
    for (;;)
    {
      if (!ReadMapping(key, sizeof(key), false, READ_KEYBOARD | READ_MERGE, escapeMapping))
        return false;
      if (!want || Util::Stricmp(key, want) == 0)
        return true;
    }
  };
  // Asks y/n: 1 yes, 0 no, -1 cancelled.
  auto askYesNo = [&]() -> int
  {
    while (ReadMapping(key, sizeof(key), false, READ_KEYBOARD | READ_MERGE, escapeMapping))
    {
      if (Util::Stricmp(key, "KEY_N") == 0)
        return 0;
      if (Util::Stricmp(key, "KEY_Y") == 0)
        return 1;
    }
    return -1;
  };

  for (;;)
  {
    printf("Calibrating %s of joystick '%s'.\n\n", joy->axisName[axisNum], joy->name);

    // Three readings: pushed fully one way, the other way, and at rest.
    int posVal = 0, negVal = 0, offVal = 0;
    unsigned posRange = 0, negRange = 0, posOffRange = 0, negOffRange = 0;
    bool cancelled = false;
    for (unsigned step = 0; step < 3 && !cancelled; step++)
    {
      PrintCalibrationStep(step, axisNum);
      printf("\nPress Return when done (or press Esc to cancel): ");
      fflush(stdout);
      if (!readKey(confirmMapping))
      {
        cancelled = true;
        break;
      }
      printf("Calibrating... ");
      fflush(stdout);

      // Watch the axis for three seconds.
      int v = GetJoyAxisValue(joyNum, axisNum);
      int lo = v, hi = v;
      bool firstShow = true;
      for (unsigned frame = 0; frame < 3 * 60; frame++)
      {
        if (!Poll())
        {
          cancelled = true;
          break;
        }
        if (escape && escape->IsActive())
        {
          WaitForRelease(escape);
          cancelled = true;
          break;
        }
        v = GetJoyAxisValue(joyNum, axisNum);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        if (show && show->IsActive())
        {
          if (firstShow)
            puts("");
          printf(" [value: %d, min: %d, max: %d]\n", v, lo, hi);
          firstShow = false;
        }
        CThread::Sleep(1000 / 60);
      }
      if (cancelled)
        break;
      printf("Done\n\n");

      if (step == 0)
        posVal = abs(hi) >= abs(lo) ? hi : lo;
      else if (step == 1)
        negVal = abs(lo) >= abs(hi) ? lo : hi;
      else
      {
        if (lo <= 0 && hi >= 0)
          offVal = 0;
        else if (lo == DEFAULT_JOY_AXISMINVAL)
          offVal = DEFAULT_JOY_AXISMINVAL;
        else if (hi == DEFAULT_JOY_AXISMAXVAL)
          offVal = DEFAULT_JOY_AXISMAXVAL;
        else
          offVal = (lo + hi) / 2;
        posRange = abs(posVal - offVal);
        negRange = abs(negVal - offVal);
        // How far the rest reading wandered towards each side.
        posOffRange = (unsigned)(posVal > offVal ? hi - offVal : offVal - lo);
        negOffRange = (unsigned)(posVal > offVal ? offVal - lo : hi - offVal);
      }
    }
    if (cancelled)
    {
      puts("[Cancelled]");
      return false;
    }

    unsigned totalRange = posRange + negRange;
    unsigned posDeadZone = (unsigned)ceil(100.0 * (double)posOffRange / (double)posRange);
    unsigned negDeadZone = (unsigned)ceil(100.0 * (double)negOffRange / (double)negRange);
    unsigned deadZone = std::max<unsigned>(1, std::max<unsigned>(negDeadZone, posDeadZone));
    bool restBetween = posVal > negVal ? (negVal <= offVal && offVal <= posVal) : (posVal <= offVal && offVal <= negVal);
    bool okay = restBetween && totalRange > 3000 && deadZone < 90;

    if (okay)
    {
      JoySettings *shared = GetJoySettings(ANY_JOYSTICK, true);
      JoySettings *own = GetJoySettings(joyNum, false);
      if (!own)
      {
        own = new JoySettings(*shared);
        m_joySettings.push_back(own);
        own->joyNum = joyNum;
      }
      printf("Calibrated Axis Settings:\n\n");
      printf(" Min Value        = %d\n", negVal);
      printf(" Center/Off Value = %d\n", offVal);
      printf(" Max Value        = %d\n", posVal);
      printf(" Dead Zone        = %u %%\n", deadZone);
      printf("\nAccept these settings: y/n? ");
      fflush(stdout);
      if (askYesNo() == 1)
      {
        own->axisMinVals[axisNum] = negVal;
        own->axisMaxVals[axisNum] = posVal;
        own->axisOffVals[axisNum] = offVal;
        own->deadZones[axisNum] = deadZone;
        ClearSourceCache();
        puts("Accepted");
        return true;
      }
      puts("[Cancelled]");
      return false;
    }

    puts("There was a problem calibrating the axis.  This may be because the steps");
    puts("were not followed correctly or the joystick is sending invalid data.");
    printf("\nTry calibrating again: y/n? ");
    fflush(stdout);
    if (askYesNo() != 1)
    {
      puts("[Cancelled]");
      return false;
    }
    puts("[Cancelled]");  // this attempt; start over
  }
}

void CInputSystem::PrintDevices()
{
  puts("Keyboards:");
  if (m_numKbds == 0)
    puts(" None");
  else if (m_numKbds == ANY_KEYBOARD)
    puts(" System Keyboard");
  else
    for (int k = 0; k < m_numKbds; k++)
      printf(" %d: %s\n", k + 1, GetKeyDetails(k)->name);

  puts("Mice:");
  if (m_numMice == 0)
    puts(" None");
  else if (m_numMice == ANY_MOUSE)
    puts(" System Mouse");
  else
    for (int m = 0; m < m_numMice; m++)
      printf(" %d: %s\n", m + 1, GetMouseDetails(m)->name);

  puts("Joysticks:");
  if (m_numJoys == 0)
    puts(" None");
  else if (m_numJoys == ANY_JOYSTICK)
    puts(" System Joystick");
  else
  {
    for (int j = 0; j < m_numJoys; j++)
    {
      const JoyDetails *joy = GetJoyDetails(j);
      printf(joy->hasFFeedback ? " %d: %s [Force Feedback Available]\n" : " %d: %s\n", j + 1, joy->name);
    }
  }
}
