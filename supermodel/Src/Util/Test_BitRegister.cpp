// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Stand-alone checks of a bit-string register of the kind a JTAG or serial
// shift chain needs: a string of bits of any length, written as binary
// ("101", "%101", "0b101") or hex ("0x1d", "$1d", four bits per digit),
// that bits can be shifted into and out of at either end.
//
// The header this test was written for (Util/BitRegister.h) is no longer
// part of the code, so the register is defined here and the test builds on
// its own:  c++ -std=c++17 -I.. Test_BitRegister.cpp
// Prints one line per failed check and returns non-zero if any failed.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
  // Bit 0 is the leftmost (most significant) bit.
  class BitString
  {
  public:
    void Clear() { m_bits.clear(); }
    size_t Size() const { return m_bits.size(); }
    void Fill(size_t count, bool one) { m_bits.assign(count, one); }

    // Replaces the contents with the bits written in 'text'.
    void Assign(const std::string &text)
    {
      m_bits = Parse(text);
    }

    // Overwrites bits starting at 'pos' with those of 'text' (bits past the
    // end are dropped).
    void Overwrite(size_t pos, const std::string &text)
    {
      std::vector<bool> bits = Parse(text);
      for (size_t i = 0; i < bits.size() && pos + i < m_bits.size(); i++)
        m_bits[pos + i] = bits[i];
    }

    void PushLeft(bool bit) { m_bits.insert(m_bits.begin(), bit); }
    void PushRight(bool bit) { m_bits.push_back(bit); }

    bool PopLeft()
    {
      bool bit = m_bits.front();
      m_bits.erase(m_bits.begin());
      return bit;
    }

    bool PopRight()
    {
      bool bit = m_bits.back();
      m_bits.pop_back();
      return bit;
    }

    void DropLeft(size_t n) { while (n-- && !m_bits.empty()) PopLeft(); }
    void DropRight(size_t n) { while (n-- && !m_bits.empty()) PopRight(); }

    // Shift keeping the length; zeros come in.
    void ShiftLeft(size_t n)  { for (; n; n--) { PopLeft(); PushRight(false); } }
    void ShiftRight(size_t n) { for (; n; n--) { PopRight(); PushLeft(false); } }

    // Shift by one keeping the length, putting 'in' at the far end, and
    // return the bit that fell out.
    bool ShiftOutLeft(bool in)  { bool out = PopLeft(); PushRight(in); return out; }
    bool ShiftOutRight(bool in) { bool out = PopRight(); PushLeft(in); return out; }

    std::string Binary() const
    {
      std::string s;
      for (bool b : m_bits)
        s += b ? '1' : '0';
      return s;
    }

    // Groups of four from the right; a short leftmost group is zero-padded.
    std::string Hex() const
    {
      size_t digits = (m_bits.size() + 3) / 4;
      if (digits == 0)
        return "0x";
      size_t pad = digits * 4 - m_bits.size();
      std::string s = "0x";
      for (size_t d = 0; d < digits; d++)
      {
        unsigned v = 0;
        for (size_t k = 0; k < 4; k++)
        {
          size_t i = d * 4 + k;
          v = v * 2 + (i >= pad && m_bits[i - pad] ? 1u : 0u);
        }
        s += "0123456789abcdef"[v];
      }
      return s;
    }

    // 'count' bits starting at 'pos' as an integer.
    uint64_t Bits(size_t pos, size_t count) const
    {
      uint64_t v = 0;
      for (size_t i = 0; i < count; i++)
        v = (v << 1) | (m_bits[pos + i] ? 1u : 0u);
      return v;
    }

    uint64_t Bits() const { return Bits(0, m_bits.size()); }

  private:
    static std::vector<bool> Parse(const std::string &text)
    {
      std::vector<bool> bits;
      bool hex = false;
      size_t at = 0;
      if (!text.empty() && text[0] == '$')
        hex = true, at = 1;
      else if (!text.empty() && text[0] == '%')
        at = 1;
      else if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        hex = true, at = 2;
      else if (text.size() >= 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
        at = 2;
      for (; at < text.size(); at++)
      {
        char c = text[at];
        if (!hex)
        {
          bits.push_back(c == '1');
          continue;
        }
        unsigned v = (c >= '0' && c <= '9') ? unsigned(c - '0')
                   : (c >= 'a' && c <= 'f') ? unsigned(c - 'a' + 10)
                   : (c >= 'A' && c <= 'F') ? unsigned(c - 'A' + 10) : 0;
        for (int k = 3; k >= 0; k--)
          bits.push_back((v >> k) & 1);
      }
      return bits;
    }

    std::vector<bool> m_bits;
  };

  int g_failed = 0;
  int g_count = 0;

  void Expect(const std::string &got, const std::string &want)
  {
    g_count++;
    if (got != want)
    {
      std::cout << "Check " << g_count << " FAILED: wanted \"" << want << "\", got \"" << got << "\"" << std::endl;
      g_failed++;
    }
  }

  std::string Bit(bool b) { return b ? "1" : "0"; }
}

int main()
{
  BitString r;

  r.Fill(3, false);   Expect(r.Binary(), "000");
  r.Fill(2, true);    Expect(r.Binary(), "11");
  r.Clear();          Expect(r.Binary(), "");

  for (const char *text : { "1011001", "%1011001", "0b1011001", "0B1011001" })
  {
    r.Assign(text);
    Expect(r.Binary(), "1011001");
  }
  for (const char *text : { "0x2e", "0X2E", "$2e" })
  {
    r.Assign(text);
    Expect(r.Binary(), "00101110");
  }

  r.Assign("100110");
  Expect(Bit(r.PopRight()), "0");  Expect(r.Binary(), "10011");
  Expect(Bit(r.PopRight()), "1");  Expect(r.Binary(), "1001");
  Expect(Bit(r.PopLeft()), "1");   Expect(r.Binary(), "001");
  Expect(Bit(r.PopLeft()), "0");   Expect(r.Binary(), "01");

  r.Assign("0xa5f");
  r.DropLeft(2);       Expect(r.Binary(), "1001011111");
  r.DropRight(3);      Expect(r.Binary(), "1001011");
  r.Assign("0xa5f");
  r.ShiftLeft(3);      Expect(r.Binary(), "001011111000");
  r.ShiftRight(5);     Expect(r.Binary(), "000000010111");

  r.Clear();
  for (bool b : { true, false, false, true, true })
    r.PushRight(b);
  Expect(r.Binary(), "10011");
  r.Clear();
  for (bool b : { true, false, false, true, true })
    r.PushLeft(b);
  Expect(r.Binary(), "11001");

  r.Assign("10010");
  Expect(Bit(r.ShiftOutLeft(true)), "1");
  Expect(Bit(r.ShiftOutLeft(false)), "0");
  Expect(r.Binary(), "01010");
  Expect(Bit(r.ShiftOutRight(true)), "0");
  Expect(r.Binary(), "10101");

  r.Fill(16, false);
  r.Overwrite(12, "0xff");  Expect(r.Binary(), "0000000000001111");
  r.Overwrite(4, "$3c");    Expect(r.Binary(), "0000001111001111");
  r.Overwrite(0, "11");     Expect(r.Binary(), "1100001111001111");

  r.Fill(1, true);    Expect(r.Hex(), "0x1");
  r.Fill(4, false);   Expect(r.Hex(), "0x0");
  r.Fill(5, false);   Expect(r.Hex(), "0x00");
  r.Assign("10110");  Expect(r.Hex(), "0x16");
  r.Assign("111111001"); Expect(r.Hex(), "0x1f9");

  r.Assign("0xc0ffee42");
  Expect(r.Bits() == 0xc0ffee42u ? "ok" : "bad", "ok");
  Expect(r.Bits(0, 8) == 0xc0 ? "ok" : "bad", "ok");
  Expect(r.Bits(28, 4) == 0x2 ? "ok" : "bad", "ok");

  if (g_failed == 0)
    std::cout << "All " << g_count << " checks passed." << std::endl;
  return g_failed ? 1 : 0;
}
